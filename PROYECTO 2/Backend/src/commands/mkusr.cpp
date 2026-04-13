#include "commands/mkusr.hpp"

#include "commands/login.hpp"   // hasActiveSession(), getSessionPartition(), sessionUid()
#include "Structures.hpp"
#include "ext2/Bitmap.hpp"

#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cctype>

// ---------------- IO helpers ----------------
static bool readAt(const std::string& path, int32_t offset, void* data, size_t sz, std::string& err) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) { err = "Error: no se pudo abrir el disco: " + path; return false; }
    file.seekg(offset, std::ios::beg);
    file.read(reinterpret_cast<char*>(data), (std::streamsize)sz);
    if (!file) { err = "Error: no se pudo leer del disco (offset=" + std::to_string(offset) + ")."; return false; }
    return true;
}

static bool writeAt(const std::string& path, int32_t offset, const void* data, size_t sz, std::string& err) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) { err = "Error: no se pudo abrir el disco: " + path; return false; }
    file.seekp(offset, std::ios::beg);
    file.write(reinterpret_cast<const char*>(data), (std::streamsize)sz);
    if (!file) { err = "Error: no se pudo escribir en el disco (offset=" + std::to_string(offset) + ")."; return false; }
    file.flush();
    return true;
}

// ---------------- Path helpers ----------------
static std::vector<std::string> splitPath(const std::string& p) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : p) {
        if (c == '/') {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
        } else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

static std::string nameFrom12(const char b_name[12]) {
    size_t len = 0;
    while (len < 12 && b_name[len] != '\0') len++;
    return std::string(b_name, b_name + len);
}

static bool readInodeByIndex(const std::string& disk, const Superblock& sb, int32_t inoIndex,
                             Inode& out, std::string& err) {
    int32_t pos = sb.s_inode_start + inoIndex * (int32_t)sizeof(Inode);
    return readAt(disk, pos, &out, sizeof(Inode), err);
}

static bool findEntryInDir(const std::string& disk, const Superblock& sb, const Inode& dirIno,
                           const std::string& name, int32_t& outInode, std::string& err) {
    for (int i = 0; i < 12; i++) {
        int32_t blk = dirIno.i_block[i];
        if (blk < 0) continue;

        FolderBlock fb{};
        int32_t pos = sb.s_block_start + blk * 64;
        if (!readAt(disk, pos, &fb, sizeof(FolderBlock), err)) return false;

        for (int j = 0; j < 4; j++) {
            if (fb.b_content[j].b_inodo < 0) continue;
            if (nameFrom12(fb.b_content[j].b_name) == name) {
                outInode = fb.b_content[j].b_inodo;
                return true;
            }
        }
    }
    outInode = -1;
    return true;
}

static bool resolvePathToInode(const std::string& disk, const Superblock& sb,
                              const std::string& absPath, int32_t& inodeIndex, std::string& err) {
    if (absPath.empty() || absPath[0] != '/') {
        err = "Error: la ruta debe iniciar con '/'.";
        return false;
    }

    auto parts = splitPath(absPath);
    int32_t current = 0; // root

    for (size_t k = 0; k < parts.size(); k++) {
        Inode cur{};
        if (!readInodeByIndex(disk, sb, current, cur, err)) return false;

        if (cur.i_type != '0') {
            err = "Error: se esperaba carpeta en ruta: " + absPath;
            return false;
        }

        int32_t next = -1;
        if (!findEntryInDir(disk, sb, cur, parts[k], next, err)) return false;
        if (next < 0) { err = "Error: no existe la ruta: " + absPath; return false; }

        current = next;
    }

    inodeIndex = current;
    return true;
}

// ---------------- File read/write direct ----------------
static bool readFileContentDirect(const std::string& disk, const Superblock& sb, const Inode& fileIno,
                                  std::string& out, std::string& err) {
    int32_t remaining = fileIno.i_s;
    out.clear();

    for (int i = 0; i < 12 && remaining > 0; i++) {
        int32_t blk = fileIno.i_block[i];
        if (blk < 0) continue;

        Block64 b{};
        int32_t pos = sb.s_block_start + blk * 64;
        if (!readAt(disk, pos, &b, sizeof(Block64), err)) return false;

        int32_t take = std::min<int32_t>(remaining, 64);
        out.append(b.bytes, b.bytes + take);
        remaining -= take;
    }
    return true;
}

static int32_t findFirstFreeBit(const std::string& disk, int32_t bmStart, int32_t count, std::string& err) {
    std::ifstream file(disk, std::ios::binary);
    if (!file.is_open()) { err = "Error: no se pudo abrir el disco: " + disk; return -1; }

    file.seekg(bmStart, std::ios::beg);
    for (int32_t i = 0; i < count; i++) {
        char c = '1';
        file.read(&c, 1);
        if (!file) { err = "Error: no se pudo leer bitmap."; return -1; }
        if (c == '0') return i;
    }
    return -1;
}

// crecimiento (solo directos 12)
static bool writeFileContentDirectGrow(const std::string& disk, Superblock& sb,
                                       int32_t inodeIndex, Inode& fileIno,
                                       const std::string& newContent,
                                       std::string& err) {
    int32_t neededBytes  = (int32_t)newContent.size();
    int32_t neededBlocks = (neededBytes + 63) / 64;
    if (neededBlocks <= 0) neededBlocks = 1;

    if (neededBlocks > 12) {
        err = "Error: users.txt excede el límite actual (solo 12 bloques directos).";
        return false;
    }

    int32_t currentBlocks = 0;
    for (int i = 0; i < 12; i++) {
        if (fileIno.i_block[i] >= 0) currentBlocks++;
        else break;
    }

    for (int i = currentBlocks; i < neededBlocks; i++) {
        int32_t freeIdx = findFirstFreeBit(disk, sb.s_bm_block_start, sb.s_blocks_count, err);
        if (freeIdx < 0) { err = "Error: no hay bloques libres para users.txt."; return false; }

        if (!Bitmap::setOne(disk, sb.s_bm_block_start, freeIdx, err)) return false;

        fileIno.i_block[i] = freeIdx;
        sb.s_free_blocks_count -= 1;
        sb.s_first_blo = freeIdx + 1;
    }

    int32_t written = 0;
    for (int i = 0; i < neededBlocks; i++) {
        Block64 b{};
        std::memset(b.bytes, 0, 64);

        int32_t take = std::min<int32_t>(64, neededBytes - written);
        if (take > 0) {
            std::memcpy(b.bytes, newContent.data() + written, (size_t)take);
            written += take;
        }

        int32_t blk = fileIno.i_block[i];
        int32_t pos = sb.s_block_start + blk * 64;
        if (!writeAt(disk, pos, &b, sizeof(Block64), err)) return false;
    }

    fileIno.i_s = neededBytes;
    fileIno.i_mtime = std::time(nullptr);

    int32_t inoPos = sb.s_inode_start + inodeIndex * (int32_t)sizeof(Inode);
    if (!writeAt(disk, inoPos, &fileIno, sizeof(Inode), err)) return false;

    return true;
}

// ---------------- users.txt parsing ----------------
static std::string trimSpaces(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b-1])) b--;
    return s.substr(a, b-a);
}

static std::vector<std::string> splitCSV(const std::string& line) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : line) {
        if (c == ',') {
            parts.push_back(trimSpaces(cur));
            cur.clear();
        } else cur.push_back(c);
    }
    parts.push_back(trimSpaces(cur));
    return parts;
}

namespace cmd {

bool mkusr(const std::string& user,
           const std::string& pass,
           const std::string& grp,
           std::string& outMsg) {
    if (!cmd::hasActiveSession()) {
        outMsg = "Error: no existe una sesión activa.";
        return false;
    }

    if (cmd::sessionUid() != 1) {
        outMsg = "Error: mkusr solo puede ser ejecutado por el usuario root.";
        return false;
    }

    if (user.empty() || pass.empty() || grp.empty()) {
        outMsg = "Error: mkusr requiere -user, -pass y -grp.";
        return false;
    }

    if (user.size() > 10) { outMsg = "Error: -user máximo 10 caracteres."; return false; }
    if (pass.size() > 10) { outMsg = "Error: -pass máximo 10 caracteres."; return false; }
    if (grp.size()  > 10) { outMsg = "Error: -grp máximo 10 caracteres."; return false; }

    // Partición actual
    std::string disk;
    int32_t partStart = 0, partSize = 0;
    std::string err;

    if (!cmd::getSessionPartition(disk, partStart, partSize, err)) {
        outMsg = err;
        return false;
    }

    // Superblock
    Superblock sb{};
    if (!readAt(disk, partStart, &sb, sizeof(Superblock), err)) { outMsg = err; return false; }
    if (sb.s_magic != 0xEF53) { outMsg = "Error: la partición no está formateada en EXT2."; return false; }

    // /users.txt
    int32_t usersInoIndex = -1;
    if (!resolvePathToInode(disk, sb, "/users.txt", usersInoIndex, err)) { outMsg = err; return false; }

    Inode usersIno{};
    if (!readInodeByIndex(disk, sb, usersInoIndex, usersIno, err)) { outMsg = err; return false; }
    if (usersIno.i_type != '1') { outMsg = "Error: /users.txt no es un archivo."; return false; }

    std::string content;
    if (!readFileContentDirect(disk, sb, usersIno, content, err)) { outMsg = err; return false; }

    // validar:
    // 1) grp debe existir y estar activo (id>0) en líneas tipo G
    // 2) user no debe existir activo (id>0) en líneas tipo U
    // 3) calcular next user id (max id de U o G + 1 es válido, pero aquí usamos max id global +1)

    std::istringstream iss(content);
    std::string line;
    bool grpExists = false;
    bool userExists = false;
    int maxId = 0;

    while (std::getline(iss, line)) {
        std::string t = trimSpaces(line);
        if (t.empty()) continue;

        auto cols = splitCSV(t);
        if (cols.size() < 3) continue;

        int idNum = 0;
        try { idNum = std::stoi(cols[0]); } catch (...) { continue; }

        if (idNum > maxId) maxId = idNum;

        std::string tipo = cols[1];

        if ((tipo == "G" || tipo == "g") && cols.size() >= 3) {
            // id,G,group
            if (idNum > 0 && cols[2] == grp) grpExists = true;
        }

        if ((tipo == "U" || tipo == "u") && cols.size() >= 5) {
            // id,U,grp,user,pass
            if (idNum > 0 && cols[3] == user) userExists = true;
        }
    }

    if (!grpExists) {
        outMsg = "Error: el grupo '" + grp + "' no existe en la partición.";
        return false;
    }

    if (userExists) {
        outMsg = "Error: el usuario '" + user + "' ya existe.";
        return false;
    }

    int newId = maxId + 1;

    // agregar línea al final (formato sin espacios, como tus ejemplos)
    std::string newLine = std::to_string(newId) + ",U," + grp + "," + user + "," + pass + "\n";
    std::string newContent = content;
    if (!newContent.empty() && newContent.back() != '\n') newContent.push_back('\n');
    newContent += newLine;

    // escribir (puede crecer)
    if (!writeFileContentDirectGrow(disk, sb, usersInoIndex, usersIno, newContent, err)) {
        outMsg = err;
        return false;
    }

    // escribir SB si cambió free blocks
    if (!writeAt(disk, partStart, &sb, sizeof(Superblock), err)) { outMsg = err; return false; }

    outMsg = "Usuario creado correctamente: " + user;
    return true;
}

} // namespace cmd