#include "commands/cat.hpp"

#include "commands/login.hpp"
#include "Structures.hpp"

#include <fstream>
#include <cstring>
#include <algorithm>


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

static bool canRead(const Inode& ino, int32_t uid, int32_t gid) {
    // permisos tipo "664" en char[3] (NO null-terminated)
    auto digitToInt = [](char c)->int { return (c >= '0' && c <= '7') ? (c - '0') : 0; };

    // root siempre lee
    if (uid == 1) return true;

    int u = digitToInt(ino.i_perm[0]);
    int g = digitToInt(ino.i_perm[1]);
    int o = digitToInt(ino.i_perm[2]);

    // bit lectura = 4
    if (uid == ino.i_uid) return (u & 4) != 0;
    if (gid == ino.i_gid) return (g & 4) != 0;
    return (o & 4) != 0;
}

static bool readSuperblockSession(const std::string& disk, int32_t partStart, Superblock& sb, std::string& err) {
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
        err = "Error: CAT solo acepta rutas absolutas que inicien con '/'.";
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

// Resuelve /a/b/c desde root inode 0 (solo usando directos)
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
            err = "Error: la ruta contiene un componente que no es carpeta: " + parts[pi];
            return false;
        }

        bool found = false;
        int32_t next = -1;

        // directos 0..11
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

    // directos 0..11
    for (int d = 0; d < 12 && remaining > 0; d++) {
        int32_t bidx = ino.i_block[d];
        if (bidx < 0) continue;

        Block64 b{};
        if (!readBlock64At(disk, sb, bidx, b, err)) return false;

        int32_t take = std::min<int32_t>(64, remaining);
        out.append(b.bytes, b.bytes + take);
        remaining -= take;
    }

    // (por ahora NO indirectos; luego lo extendemos con PointerBlock)
    return true;
}

namespace cmd {

bool cat(const std::vector<std::string>& files, std::string& outMsg) {
    // 1) Requiere sesión activa
    if (!cmd::hasActiveSession()) {
        outMsg = "Error: no existe una sesión activa. Use login.";
        return false;
    }

    if (files.empty()) {
        outMsg = "Error: cat requiere al menos -file1.";
        return false;
    }

    // 2) Traer partición de la sesión
    std::string disk;
    int32_t partStart = 0;
    int32_t partSize = 0;
    std::string err;

    if (!cmd::getSessionPartition(disk, partStart, partSize, err)) {
        outMsg = err;
        return false;
    }

    // 3) Leer superblock
    Superblock sb{};
    if (!readSuperblockSession(disk, partStart, sb, err)) {
        outMsg = err;
        return false;
    }

    int32_t uid = cmd::sessionUid();
    int32_t gid = cmd::sessionGid();

    // 4) Por cada archivo: resolver path -> inode -> permisos -> contenido
    std::string finalOut;

    for (size_t i = 0; i < files.size(); i++) {
        const std::string& p = files[i];

        int32_t inoIndex = -1;
        if (!resolvePathToInode(disk, sb, p, inoIndex, err)) {
            outMsg = err;
            return false;
        }

        Inode ino{};
        if (!readInodeAt(disk, sb, inoIndex, ino, err)) {
            outMsg = err;
            return false;
        }

        if (!canRead(ino, uid, gid)) {
            outMsg = "Error: no tiene permisos de lectura para: " + p;
            return false;
        }

        std::string content;
        if (!readFileContent(disk, sb, inoIndex, content, err)) {
            outMsg = err;
            return false;
        }

        finalOut += content;
        if (i + 1 < files.size()) finalOut += "\n";
    }

    outMsg = finalOut;
    return true;
}

} // namespace cmd