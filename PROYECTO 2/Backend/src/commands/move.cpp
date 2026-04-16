#include "commands/move.hpp"
#include "commands/login.hpp"
#include "commands/mount.hpp"
#include "ext3/journal.hpp"
#include "Structures.hpp"

#include <fstream>
#include <string>
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

static bool writeAt(const std::string& path, int32_t offset, const void* data, size_t sz, std::string& err) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) {
        err = "Error: no se pudo abrir el disco: " + path;
        return false;
    }
    file.seekp(offset, std::ios::beg);
    file.write(reinterpret_cast<const char*>(data), (std::streamsize)sz);
    if (!file) {
        err = "Error: no se pudo escribir en el disco (offset=" + std::to_string(offset) + ").";
        return false;
    }
    file.flush();
    return true;
}

static bool readSuperblock(const std::string& disk, int32_t partStart, Superblock& sb, std::string& err) {
    if (!readAt(disk, partStart, &sb, sizeof(Superblock), err)) return false;
    if (sb.s_magic != 0xEF53) {
        err = "Error: la partición no parece EXT2/EXT3 (magic inválido).";
        return false;
    }
    return true;
}

static bool readInode(const std::string& disk, const Superblock& sb, int32_t idx, Inode& ino, std::string& err) {
    int32_t pos = sb.s_inode_start + idx * (int32_t)sizeof(Inode);
    return readAt(disk, pos, &ino, sizeof(Inode), err);
}

static bool readFolderBlock(const std::string& disk, const Superblock& sb, int32_t bidx, FolderBlock& fb, std::string& err) {
    int32_t pos = sb.s_block_start + bidx * 64;
    return readAt(disk, pos, &fb, sizeof(FolderBlock), err);
}

static bool writeFolderBlock(const std::string& disk, const Superblock& sb, int32_t bidx, const FolderBlock& fb, std::string& err) {
    int32_t pos = sb.s_block_start + bidx * 64;
    return writeAt(disk, pos, &fb, sizeof(FolderBlock), err);
}

static std::string name12ToString(const char n[12]) {
    size_t len = 0;
    while (len < 12 && n[len] != '\0') len++;
    return std::string(n, n + len);
}

static void setName12(char out[12], const std::string& s) {
    std::memset(out, 0, 12);
    std::strncpy(out, s.c_str(), 11);
}

static bool splitAbsPath(const std::string& path, std::vector<std::string>& parts, std::string& err) {
    parts.clear();
    if (path.empty() || path[0] != '/') {
        err = "Error: move requiere ruta absoluta que inicie con '/'.";
        return false;
    }

    std::string cur;
    for (size_t i = 1; i < path.size(); i++) {
        char c = path[i];
        if (c == '/') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return true;
}

static int digitToInt(char c) {
    return (c >= '0' && c <= '7') ? (c - '0') : 0;
}

static bool canWriteInode(const Inode& ino, int32_t uid, int32_t gid) {
    if (uid == 1) return true; 

    int u = digitToInt(ino.i_perm[0]);
    int g = digitToInt(ino.i_perm[1]);
    int o = digitToInt(ino.i_perm[2]);

    if (uid == ino.i_uid) return (u & 2) != 0;
    if (gid == ino.i_gid) return (g & 2) != 0;
    return (o & 2) != 0;
}

static bool findEntryInDir(const std::string& disk, const Superblock& sb, int32_t dirIno,
                           const std::string& name, int32_t& outInode, std::string& err) {
    Inode dino{};
    if (!readInode(disk, sb, dirIno, dino, err)) return false;
    if (dino.i_type != '0') {
        err = "Error: no es carpeta.";
        return false;
    }

    for (int i = 0; i < 12; i++) {
        int32_t b = dino.i_block[i];
        if (b < 0) continue;

        FolderBlock fb{};
        if (!readFolderBlock(disk, sb, b, fb, err)) return false;

        for (int k = 0; k < 4; k++) {
            if (fb.b_content[k].b_inodo < 0) continue;
            if (name12ToString(fb.b_content[k].b_name) == name) {
                outInode = fb.b_content[k].b_inodo;
                return true;
            }
        }
    }

    outInode = -1;
    return true;
}

static bool resolvePath(const std::string& disk, const Superblock& sb,
                        const std::string& absPath, int32_t& outInode, std::string& err) {
    std::vector<std::string> parts;
    if (!splitAbsPath(absPath, parts, err)) return false;

    if (parts.empty()) {
        outInode = 0;
        return true;
    }

    int32_t current = 0;
    for (const auto& p : parts) {
        int32_t next = -1;
        if (!findEntryInDir(disk, sb, current, p, next, err)) return false;
        if (next < 0) {
            err = "Error: no existe la ruta: " + absPath;
            return false;
        }
        current = next;
    }

    outInode = current;
    return true;
}

static bool resolveParentAndTarget(const std::string& disk, const Superblock& sb,
                                   const std::string& absPath,
                                   int32_t& parentIno, int32_t& targetIno,
                                   std::string& targetName,
                                   std::string& err) {
    std::vector<std::string> parts;
    if (!splitAbsPath(absPath, parts, err)) return false;
    if (parts.empty()) {
        err = "Error: path inválido.";
        return false;
    }

    targetName = parts.back();
    parentIno = 0;

    for (size_t i = 0; i + 1 < parts.size(); i++) {
        int32_t next = -1;
        if (!findEntryInDir(disk, sb, parentIno, parts[i], next, err)) return false;
        if (next < 0) {
            err = "Error: no existe la carpeta padre en la ruta.";
            return false;
        }

        Inode ino{};
        if (!readInode(disk, sb, next, ino, err)) return false;
        if (ino.i_type != '0') {
            err = "Error: un componente padre no es carpeta.";
            return false;
        }

        parentIno = next;
    }

    if (!findEntryInDir(disk, sb, parentIno, targetName, targetIno, err)) return false;
    if (targetIno < 0) {
        err = "Error: no existe el archivo o carpeta origen.";
        return false;
    }

    return true;
}

static bool addEntryToDir(const std::string& disk, const Superblock& sb,
                          int32_t dirInoIdx, const std::string& name, int32_t childInoIdx,
                          std::string& err) {
    Inode dir{};
    if (!readInode(disk, sb, dirInoIdx, dir, err)) return false;

    for (int i = 0; i < 12; i++) {
        int32_t b = dir.i_block[i];
        if (b < 0) continue;

        FolderBlock fb{};
        if (!readFolderBlock(disk, sb, b, fb, err)) return false;

        for (int k = 0; k < 4; k++) {
            if (fb.b_content[k].b_inodo < 0) {
                setName12(fb.b_content[k].b_name, name);
                fb.b_content[k].b_inodo = childInoIdx;
                if (!writeFolderBlock(disk, sb, b, fb, err)) return false;
                return true;
            }
        }
    }

    err = "Error: no hay espacio en carpeta destino.";
    return false;
}

static bool removeEntryFromParent(const std::string& disk, const Superblock& sb,
                                  int32_t parentIno, const std::string& targetName,
                                  std::string& err) {
    Inode parent{};
    if (!readInode(disk, sb, parentIno, parent, err)) return false;

    for (int i = 0; i < 12; i++) {
        int32_t b = parent.i_block[i];
        if (b < 0) continue;

        FolderBlock fb{};
        if (!readFolderBlock(disk, sb, b, fb, err)) return false;

        for (int k = 0; k < 4; k++) {
            if (fb.b_content[k].b_inodo < 0) continue;
            if (name12ToString(fb.b_content[k].b_name) == targetName) {
                std::memset(fb.b_content[k].b_name, 0, 12);
                fb.b_content[k].b_inodo = -1;
                if (!writeFolderBlock(disk, sb, b, fb, err)) return false;
                return true;
            }
        }
    }

    err = "Error: no se encontró la entrada en la carpeta padre.";
    return false;
}

static bool updateDotDotIfDirectory(const std::string& disk, const Superblock& sb,
                                    int32_t movedInoIdx, int32_t newParentIno,
                                    std::string& err) {
    Inode ino{};
    if (!readInode(disk, sb, movedInoIdx, ino, err)) return false;

    if (ino.i_type != '0') return true; 

    int32_t b = ino.i_block[0];
    if (b < 0) {
        err = "Error: carpeta sin bloque inicial.";
        return false;
    }

    FolderBlock fb{};
    if (!readFolderBlock(disk, sb, b, fb, err)) return false;

    for (int k = 0; k < 4; k++) {
        std::string nm = name12ToString(fb.b_content[k].b_name);
        if (nm == "..") {
            fb.b_content[k].b_inodo = newParentIno;
            if (!writeFolderBlock(disk, sb, b, fb, err)) return false;
            return true;
        }
    }

    err = "Error: no se encontró la referencia '..' en la carpeta.";
    return false;
}

// =======================================
// JOURNAL
// =======================================

static void writeMoveJournal(const std::string& disk,
                             int32_t partStart,
                             const Superblock& sb,
                             const std::string& path,
                             const std::string& destino) {
    if (sb.s_filesystem_type != 3) return;

    int32_t journalingStart = partStart + (int32_t)sizeof(Superblock);

    ext3::writeJournal(
        disk,
        journalingStart,
        0,
        "move",
        path,
        destino
    );
}

namespace cmd {

bool move(const std::string& path, const std::string& destino, std::string& outMsg) {
    if (path.empty() || destino.empty()) {
        outMsg = "Error: move requiere -path y -destino.";
        return false;
    }

    if (path == "/") {
        outMsg = "Error: no se permite mover la raíz del sistema.";
        return false;
    }

    if (!cmd::hasActiveSession()) {
        outMsg = "Error: no existe una sesión activa. Use login.";
        return false;
    }

    std::string disk;
    int32_t partStart = 0, partSize = 0;
    std::string err;

    if (!cmd::getSessionPartition(disk, partStart, partSize, err)) {
        outMsg = err;
        return false;
    }

    Superblock sb{};
    if (!readSuperblock(disk, partStart, sb, err)) {
        outMsg = err;
        return false;
    }

    int32_t srcParentIno = -1;
    int32_t srcIno = -1;
    std::string moveName;

    if (!resolveParentAndTarget(disk, sb, path, srcParentIno, srcIno, moveName, err)) {
        outMsg = err;
        return false;
    }

    int32_t destIno = -1;
    if (!resolvePath(disk, sb, destino, destIno, err)) {
        outMsg = err;
        return false;
    }

    Inode src{};
    if (!readInode(disk, sb, srcIno, src, err)) {
        outMsg = err;
        return false;
    }

    Inode dest{};
    if (!readInode(disk, sb, destIno, dest, err)) {
        outMsg = err;
        return false;
    }

    if (dest.i_type != '0') {
        outMsg = "Error: el destino debe ser una carpeta.";
        return false;
    }

    int uid = cmd::sessionUid();
    int gid = cmd::sessionGid();

    // se verifica escritura sobre el origen
    if (!canWriteInode(src, uid, gid)) {
        outMsg = "Error: no tiene permiso de escritura sobre el origen.";
        return false;
    }

    // y también escritura sobre la carpeta destino
    if (!canWriteInode(dest, uid, gid)) {
        outMsg = "Error: no tiene permiso de escritura en la carpeta destino.";
        return false;
    }

    // evitar mover a una carpeta donde ya exista el mismo nombre
    int32_t existing = -1;
    if (!findEntryInDir(disk, sb, destIno, moveName, existing, err)) {
        outMsg = err;
        return false;
    }
    if (existing >= 0) {
        outMsg = "Error: ya existe un archivo o carpeta con ese nombre en el destino.";
        return false;
    }

    // evitar mover dentro del mismo padre
    if (srcParentIno == destIno) {
        outMsg = "Error: el origen ya pertenece a esa carpeta destino.";
        return false;
    }

    // agregar referencia en destino
    if (!addEntryToDir(disk, sb, destIno, moveName, srcIno, err)) {
        outMsg = err;
        return false;
    }

    // quitar referencia del padre origen
    if (!removeEntryFromParent(disk, sb, srcParentIno, moveName, err)) {
        outMsg = err;
        return false;
    }

    // si es carpeta, actualizar '..'
    if (!updateDotDotIfDirectory(disk, sb, srcIno, destIno, err)) {
        outMsg = err;
        return false;
    }

    // journal EXT3
    writeMoveJournal(disk, partStart, sb, path, destino);

    outMsg = "Move realizado correctamente.";
    return true;
}

}