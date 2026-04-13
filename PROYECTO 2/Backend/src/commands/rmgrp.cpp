#include "commands/rmgrp.hpp"

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

// ---------------- Read file direct ----------------
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

// ---------------- Bitmap scan for free block ----------------
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

// ---------------- Write file direct (grow only) ----------------
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

    // cuantos bloques ya tiene
    int32_t currentBlocks = 0;
    for (int i = 0; i < 12; i++) {
        if (fileIno.i_block[i] >= 0) currentBlocks++;
        else break;
    }

    // asignar si faltan
    for (int i = currentBlocks; i < neededBlocks; i++) {
        int32_t freeIdx = findFirstFreeBit(disk, sb.s_bm_block_start, sb.s_blocks_count, err);
        if (freeIdx < 0) { err = "Error: no hay bloques libres para users.txt."; return false; }

        if (!Bitmap::setOne(disk, sb.s_bm_block_start, freeIdx, err)) return false;

        fileIno.i_block[i] = freeIdx;
        sb.s_free_blocks_count -= 1;
        sb.s_first_blo = freeIdx + 1;
    }

    // escribir contenido
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

    // actualizar inodo
    fileIno.i_s = neededBytes;
    fileIno.i_mtime = std::time(nullptr);

    int32_t inoPos = sb.s_inode_start + inodeIndex * (int32_t)sizeof(Inode);
    if (!writeAt(disk, inoPos, &fileIno, sizeof(Inode), err)) return false;

    return true;
}

// ---------------- users.txt parsing helpers ----------------
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

bool rmgrp(const std::string& name, std::string& outMsg) {
    if (!cmd::hasActiveSession()) {
        outMsg = "Error: no existe una sesión activa.";
        return false;
    }

    if (cmd::sessionUid() != 1) {
        outMsg = "Error: rmgrp solo puede ser ejecutado por el usuario root.";
        return false;
    }

    if (name.empty()) {
        outMsg = "Error: rmgrp requiere -name.";
        return false;
    }

    // Partición de sesión
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

    // /users.txt -> inode
    int32_t usersInoIndex = -1;
    if (!resolvePathToInode(disk, sb, "/users.txt", usersInoIndex, err)) { outMsg = err; return false; }

    Inode usersIno{};
    if (!readInodeByIndex(disk, sb, usersInoIndex, usersIno, err)) { outMsg = err; return false; }
    if (usersIno.i_type != '1') { outMsg = "Error: /users.txt no es un archivo."; return false; }

    // leer contenido
    std::string content;
    if (!readFileContentDirect(disk, sb, usersIno, content, err)) { outMsg = err; return false; }

    // modificar línea del grupo (id -> 0)
    std::istringstream iss(content);
    std::vector<std::string> lines;
    std::string line;

    bool foundActive = false;

    while (std::getline(iss, line)) {
        std::string raw = line;            // sin \n
        std::string t = trimSpaces(raw);

        if (t.empty()) { lines.push_back(raw); continue; }

        auto cols = splitCSV(t);
        if (cols.size() >= 3) {
            // id, tipo, nombre
            int idNum = 0;
            try { idNum = std::stoi(cols[0]); } catch (...) { idNum = -1; }

            std::string tipo = cols[1];

            // solo grupos
            bool isGroup = (tipo == "G" || tipo == "g");
            if (isGroup) {
                std::string gname = cols[2];

                // solo si está activo (id>0) y nombre coincide (case-sensitive)
                if (idNum > 0 && gname == name) {
                    foundActive = true;
                    // reconstruir la línea con id=0 (formato del enunciado: 0,G,grupo)
                    raw = "0,G," + gname;
                }
            }
        }

        lines.push_back(raw);
    }

    if (!foundActive) {
        outMsg = "Error: el grupo '" + name + "' no existe (o ya fue eliminado).";
        return false;
    }

    // reconstruir contenido con saltos de línea
    std::string newContent;
    for (size_t i = 0; i < lines.size(); i++) {
        newContent += lines[i];
        newContent += "\n";
    }

    // escribir (puede crecer, pero normalmente no; igual soporta crecer)
    if (!writeFileContentDirectGrow(disk, sb, usersInoIndex, usersIno, newContent, err)) { outMsg = err; return false; }

    // escribir SB actualizado
    if (!writeAt(disk, partStart, &sb, sizeof(Superblock), err)) { outMsg = err; return false; }

    outMsg = "Grupo eliminado correctamente: " + name;
    return true;
}

} // namespace cmd