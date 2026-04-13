#include "reports/file_report.hpp"
#include "Structures.hpp"
#include "commands/login.hpp" // opcional: si hay sesión, podemos validar permisos

#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <cstring>

static bool readAt(const std::string& path, int32_t offset, void* data, size_t sz, std::string& err) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        err = "Error: no se pudo abrir el disco: " + path;
        return false;
    }
    file.seekg(offset, std::ios::beg);
    file.read(reinterpret_cast<char*>(data), (std::streamsize)sz);
    if (!file) {
        err = "Error: no se pudo leer del disco (offset=" + std::to_string(offset) + ").";
        return false;
    }
    return true;
}

static std::string name12ToString(const char n[12]) {
    size_t len = 0;
    while (len < 12 && n[len] != '\0') len++;
    return std::string(n, n + len);
}

static bool readSuperblock(const std::string& disk, int32_t partStart, Superblock& sb, std::string& err) {
    if (!readAt(disk, partStart, &sb, sizeof(Superblock), err)) return false;
    if (sb.s_magic != 0xEF53) {
        err = "Error: la partición no parece EXT2 (magic inválido).";
        return false;
    }
    return true;
}

static bool readInodeAt(const std::string& disk, const Superblock& sb, int32_t inodeIndex, Inode& ino, std::string& err) {
    int32_t pos = sb.s_inode_start + inodeIndex * (int32_t)sizeof(Inode);
    return readAt(disk, pos, &ino, sizeof(Inode), err);
}

static bool readFolderBlockAt(const std::string& disk, const Superblock& sb, int32_t blockIndex, FolderBlock& fb, std::string& err) {
    int32_t pos = sb.s_block_start + blockIndex * 64;
    return readAt(disk, pos, &fb, sizeof(FolderBlock), err);
}

static bool readBlock64At(const std::string& disk, const Superblock& sb, int32_t blockIndex, Block64& b, std::string& err) {
    int32_t pos = sb.s_block_start + blockIndex * 64;
    return readAt(disk, pos, &b, sizeof(Block64), err);
}

static bool splitPath(const std::string& path, std::vector<std::string>& parts, std::string& err) {
    parts.clear();
    if (path.empty() || path[0] != '/') {
        err = "Error: path_file_ls debe ser una ruta absoluta (inicia con '/').";
        return false;
    }
    std::string cur;
    for (size_t i = 1; i < path.size(); i++) {
        char c = path[i];
        if (c == '/') {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return true;
}

// resuelve /a/b/c desde inode 0 (root) usando SOLO apuntadores directos
static bool resolvePathToInode(const std::string& disk, const Superblock& sb,
                              const std::string& absPath, int32_t& outInode,
                              std::string& err) {
    std::vector<std::string> parts;
    if (!splitPath(absPath, parts, err)) return false;

    int32_t current = 0; // root
    if (parts.empty()) { outInode = 0; return true; }

    for (size_t pi = 0; pi < parts.size(); pi++) {
        Inode curIno{};
        if (!readInodeAt(disk, sb, current, curIno, err)) return false;

        if (curIno.i_type != '0') {
            err = "Error: componente no es carpeta: " + parts[pi];
            return false;
        }

        bool found = false;
        int32_t next = -1;

        for (int d = 0; d < 12 && !found; d++) {
            int32_t b = curIno.i_block[d];
            if (b < 0) continue;

            FolderBlock fb{};
            if (!readFolderBlockAt(disk, sb, b, fb, err)) return false;

            for (int k = 0; k < 4; k++) {
                if (fb.b_content[k].b_inodo < 0) continue;
                std::string nm = name12ToString(fb.b_content[k].b_name);
                if (nm == parts[pi]) {
                    next = fb.b_content[k].b_inodo;
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            err = "Error: no existe el path: " + absPath;
            return false;
        }
        current = next;
    }

    outInode = current;
    return true;
}

static bool canReadUGO(const Inode& ino, int32_t uid) {
    auto digitToInt = [](char c)->int { return (c >= '0' && c <= '7') ? (c - '0') : 0; };

    // root (uid=1) siempre
    if (uid == 1) return true;

    int owner = digitToInt(ino.i_perm[0]);
    int other = digitToInt(ino.i_perm[2]);

    if (uid == ino.i_uid) return (owner & 4) != 0; // read bit
    return (other & 4) != 0;
}

static bool readFileContent(const std::string& disk, const Superblock& sb,
                            int32_t inodeIndex, std::string& out, std::string& err) {
    Inode ino{};
    if (!readInodeAt(disk, sb, inodeIndex, ino, err)) return false;

    if (ino.i_type != '1') {
        err = "Error: el path no es un archivo.";
        return false;
    }

    int32_t remaining = ino.i_s;
    out.clear();

    for (int d = 0; d < 12 && remaining > 0; d++) {
        int32_t bidx = ino.i_block[d];
        if (bidx < 0) continue;

        Block64 b{};
        if (!readBlock64At(disk, sb, bidx, b, err)) return false;

        int32_t take = std::min<int32_t>(64, remaining);
        out.append(b.bytes, b.bytes + take);
        remaining -= take;
    }

    // (por ahora: sin indirectos)
    return true;
}

namespace reports {

bool buildFileText(const std::string& diskPath,
                   int32_t partStart,
                   const std::string& absPathInFs,
                   std::string& outText,
                   std::string& err) {

    Superblock sb{};
    if (!readSuperblock(diskPath, partStart, sb, err)) return false;

    int32_t inodeIndex = -1;
    if (!resolvePathToInode(diskPath, sb, absPathInFs, inodeIndex, err)) return false;

    Inode ino{};
    if (!readInodeAt(diskPath, sb, inodeIndex, ino, err)) return false;

    // Si hay sesión activa, validamos permisos con el uid logueado.
    // Si NO hay sesión, NO bloqueamos (rep no exige login en el enunciado).
    if (cmd::hasActiveSession()) {
        int32_t uid = cmd::sessionUid();
        if (!canReadUGO(ino, uid)) {
            err = "Error: no tiene permisos de lectura para: " + absPathInFs;
            return false;
        }
    }

    std::string content;
    if (!readFileContent(diskPath, sb, inodeIndex, content, err)) return false;

    std::ostringstream o;
    o << "Archivo: " << absPathInFs << "\n";
    o << "----------------------------------------\n";
    o << content;

    outText = o.str();
    return true;
}

} // namespace reports