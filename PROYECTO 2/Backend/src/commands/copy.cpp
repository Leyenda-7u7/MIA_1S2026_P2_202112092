#include "commands/copy.hpp"
#include "commands/login.hpp"
#include "commands/mount.hpp"
#include "ext2/Bitmap.hpp"
#include "ext3/journal.hpp"
#include "Structures.hpp"

#include <string>
#include <vector>
#include <cstring>
#include <fstream>
#include <algorithm>

// =======================================
// IO HELPERS
// =======================================

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

static bool writeSuperblock(const std::string& disk, int32_t partStart, const Superblock& sb, std::string& err) {
    return writeAt(disk, partStart, &sb, sizeof(Superblock), err);
}

static bool readInode(const std::string& disk, const Superblock& sb, int32_t idx, Inode& ino, std::string& err) {
    int32_t pos = sb.s_inode_start + idx * (int32_t)sizeof(Inode);
    return readAt(disk, pos, &ino, sizeof(Inode), err);
}

static bool writeInode(const std::string& disk, const Superblock& sb, int32_t idx, const Inode& ino, std::string& err) {
    int32_t pos = sb.s_inode_start + idx * (int32_t)sizeof(Inode);
    return writeAt(disk, pos, &ino, sizeof(Inode), err);
}

static bool readFolderBlock(const std::string& disk, const Superblock& sb, int32_t bidx, FolderBlock& fb, std::string& err) {
    int32_t pos = sb.s_block_start + bidx * 64;
    return readAt(disk, pos, &fb, sizeof(FolderBlock), err);
}

static bool writeFolderBlock(const std::string& disk, const Superblock& sb, int32_t bidx, const FolderBlock& fb, std::string& err) {
    int32_t pos = sb.s_block_start + bidx * 64;
    return writeAt(disk, pos, &fb, sizeof(FolderBlock), err);
}

static bool readBlock64(const std::string& disk, const Superblock& sb, int32_t bidx, Block64& blk, std::string& err) {
    int32_t pos = sb.s_block_start + bidx * 64;
    return readAt(disk, pos, &blk, sizeof(Block64), err);
}

static bool writeBlock64(const std::string& disk, const Superblock& sb, int32_t bidx, const Block64& blk, std::string& err) {
    int32_t pos = sb.s_block_start + bidx * 64;
    return writeAt(disk, pos, &blk, sizeof(Block64), err);
}

static bool findFirstFreeBit(const std::string& disk, int32_t bmStart, int32_t count, int32_t& outIndex, std::string& err) {
    std::ifstream file(disk, std::ios::binary);
    if (!file.is_open()) {
        err = "Error: no se pudo abrir disco para leer bitmap.";
        return false;
    }

    file.seekg(bmStart, std::ios::beg);

    for (int32_t i = 0; i < count; i++) {
        char c;
        file.read(&c, 1);
        if (!file) {
            err = "Error leyendo bitmap.";
            return false;
        }
        if (c == '0') {
            outIndex = i;
            return true;
        }
    }

    outIndex = -1;
    return true;
}

// =======================================
// PATH / NAMES
// =======================================

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
        err = "Error: copy requiere ruta absoluta que inicie con '/'.";
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

// =======================================
// PERMISOS
// =======================================

static int digitToInt(char c) {
    return (c >= '0' && c <= '7') ? (c - '0') : 0;
}

static bool canRead(const Inode& ino, int uid, int gid) {
    if (uid == 1) return true;

    int u = digitToInt(ino.i_perm[0]);
    int g = digitToInt(ino.i_perm[1]);
    int o = digitToInt(ino.i_perm[2]);

    if (uid == ino.i_uid) return (u & 4) != 0;
    if (gid == ino.i_gid) return (g & 4) != 0;
    return (o & 4) != 0;
}

static bool canWrite(const Inode& ino, int uid, int gid) {
    if (uid == 1) return true;

    int u = digitToInt(ino.i_perm[0]);
    int g = digitToInt(ino.i_perm[1]);
    int o = digitToInt(ino.i_perm[2]);

    if (uid == ino.i_uid) return (u & 2) != 0;
    if (gid == ino.i_gid) return (g & 2) != 0;
    return (o & 2) != 0;
}

// =======================================
// DIRECTORY HELPERS
// =======================================

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

struct DirChild {
    std::string name;
    int32_t inode;
};

static bool collectDirChildren(const std::string& disk, const Superblock& sb,
                               int32_t dirInoIdx,
                               std::vector<DirChild>& children,
                               std::string& err) {
    children.clear();

    Inode dir{};
    if (!readInode(disk, sb, dirInoIdx, dir, err)) return false;
    if (dir.i_type != '0') {
        err = "Error: el inodo no es una carpeta.";
        return false;
    }

    for (int i = 0; i < 12; i++) {
        int32_t b = dir.i_block[i];
        if (b < 0) continue;

        FolderBlock fb{};
        if (!readFolderBlock(disk, sb, b, fb, err)) return false;

        for (int k = 0; k < 4; k++) {
            if (fb.b_content[k].b_inodo < 0) continue;

            std::string nm = name12ToString(fb.b_content[k].b_name);
            if (nm == "." || nm == ".." || nm.empty()) continue;

            children.push_back({nm, fb.b_content[k].b_inodo});
        }
    }

    return true;
}

// =======================================
// JOURNAL
// =======================================

static void writeCopyJournal(const std::string& disk,
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
        "copy",
        path,
        destino
    );
}

// =======================================
// CREATE NEW DIR IN DEST
// =======================================

static bool createDirClone(const std::string& disk, Superblock& sb, int32_t partStart,
                           const Inode& srcDir, int32_t parentDestIno,
                           const std::string& dirName, int32_t& newDirIno,
                           std::string& err) {
    int32_t freeIno = -1;
    if (!findFirstFreeBit(disk, sb.s_bm_inode_start, sb.s_inodes_count, freeIno, err)) return false;
    if (freeIno < 0) {
        err = "Error: no hay inodos libres.";
        return false;
    }

    int32_t freeBlk = -1;
    if (!findFirstFreeBit(disk, sb.s_bm_block_start, sb.s_blocks_count, freeBlk, err)) return false;
    if (freeBlk < 0) {
        err = "Error: no hay bloques libres.";
        return false;
    }

    if (!Bitmap::setOne(disk, sb.s_bm_inode_start, freeIno, err)) return false;
    if (!Bitmap::setOne(disk, sb.s_bm_block_start, freeBlk, err)) return false;

    sb.s_free_inodes_count--;
    sb.s_free_blocks_count--;
    if (freeIno < sb.s_first_ino) sb.s_first_ino = freeIno;
    if (freeBlk < sb.s_first_blo) sb.s_first_blo = freeBlk;

    Inode newDir = srcDir;
    for (int i = 0; i < 15; i++) newDir.i_block[i] = -1;
    newDir.i_block[0] = freeBlk;

    FolderBlock fb{};
    for (int k = 0; k < 4; k++) {
        std::memset(fb.b_content[k].b_name, 0, 12);
        fb.b_content[k].b_inodo = -1;
    }
    setName12(fb.b_content[0].b_name, ".");
    fb.b_content[0].b_inodo = freeIno;
    setName12(fb.b_content[1].b_name, "..");
    fb.b_content[1].b_inodo = parentDestIno;

    if (!writeInode(disk, sb, freeIno, newDir, err)) return false;
    if (!writeFolderBlock(disk, sb, freeBlk, fb, err)) return false;
    if (!addEntryToDir(disk, sb, parentDestIno, dirName, freeIno, err)) return false;
    if (!writeSuperblock(disk, partStart, sb, err)) return false;

    newDirIno = freeIno;
    return true;
}

// =======================================
// COPY FILE
// =======================================

static bool copyFileToDir(const std::string& disk, Superblock& sb,
                          int32_t partStart,
                          int32_t srcInoIdx,
                          int32_t destDirIno,
                          const std::string& newName,
                          std::string& err) {
    Inode src{};
    if (!readInode(disk, sb, srcInoIdx, src, err)) return false;

    if (src.i_type != '1') {
        err = "Error: el origen no es un archivo.";
        return false;
    }

    int32_t newIno = -1;
    if (!findFirstFreeBit(disk, sb.s_bm_inode_start, sb.s_inodes_count, newIno, err)) return false;
    if (newIno < 0) {
        err = "Error: no hay inodos libres.";
        return false;
    }

    if (!Bitmap::setOne(disk, sb.s_bm_inode_start, newIno, err)) return false;
    sb.s_free_inodes_count--;
    if (newIno < sb.s_first_ino) sb.s_first_ino = newIno;

    Inode newNode = src;
    for (int i = 0; i < 15; i++) newNode.i_block[i] = -1;

    for (int i = 0; i < 12; i++) {
        if (src.i_block[i] < 0) continue;

        int32_t newBlock = -1;
        if (!findFirstFreeBit(disk, sb.s_bm_block_start, sb.s_blocks_count, newBlock, err)) return false;
        if (newBlock < 0) {
            err = "Error: no hay bloques libres.";
            return false;
        }

        if (!Bitmap::setOne(disk, sb.s_bm_block_start, newBlock, err)) return false;
        sb.s_free_blocks_count--;
        if (newBlock < sb.s_first_blo) sb.s_first_blo = newBlock;

        Block64 blk{};
        if (!readBlock64(disk, sb, src.i_block[i], blk, err)) return false;
        if (!writeBlock64(disk, sb, newBlock, blk, err)) return false;

        newNode.i_block[i] = newBlock;
    }

    if (!writeInode(disk, sb, newIno, newNode, err)) return false;
    if (!addEntryToDir(disk, sb, destDirIno, newName, newIno, err)) return false;
    if (!writeSuperblock(disk, partStart, sb, err)) return false;

    return true;
}

// =======================================
// RECURSIVE COPY
// =======================================

static bool copyTree(const std::string& disk, Superblock& sb, int32_t partStart,
                     int32_t srcInoIdx, int32_t destDirIno,
                     const std::string& entryName,
                     int uid, int gid,
                     std::string& err) {
    Inode src{};
    if (!readInode(disk, sb, srcInoIdx, src, err)) return false;

    if (!canRead(src, uid, gid)) {
        // Se ignora si no tiene permiso de lectura
        return true;
    }

    if (src.i_type == '1') {
        return copyFileToDir(disk, sb, partStart, srcInoIdx, destDirIno, entryName, err);
    }

    // Es carpeta: crear clon y copiar hijos
    int32_t newDirIno = -1;
    if (!createDirClone(disk, sb, partStart, src, destDirIno, entryName, newDirIno, err)) return false;

    std::vector<DirChild> children;
    if (!collectDirChildren(disk, sb, srcInoIdx, children, err)) return false;

    for (const auto& ch : children) {
        if (!copyTree(disk, sb, partStart, ch.inode, newDirIno, ch.name, uid, gid, err)) return false;
    }

    return true;
}

// =======================================
// MAIN
// =======================================

namespace cmd {

bool copy(const std::string& path, const std::string& destino, std::string& outMsg) {
    if (path.empty() || destino.empty()) {
        outMsg = "Error: copy requiere -path y -destino.";
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

    int uid = cmd::sessionUid();
    int gid = cmd::sessionGid();

    int32_t srcIno = -1;
    if (!resolvePath(disk, sb, path, srcIno, err)) {
        outMsg = err;
        return false;
    }

    int32_t destIno = -1;
    if (!resolvePath(disk, sb, destino, destIno, err)) {
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

    if (!canWrite(dest, uid, gid)) {
        outMsg = "Error: no tiene permiso de escritura en la carpeta destino.";
        return false;
    }

    std::vector<std::string> srcParts;
    if (!splitAbsPath(path, srcParts, err)) {
        outMsg = err;
        return false;
    }
    if (srcParts.empty()) {
        outMsg = "Error: path inválido.";
        return false;
    }

    std::string copyName = srcParts.back();

    int32_t existing = -1;
    if (!findEntryInDir(disk, sb, destIno, copyName, existing, err)) {
        outMsg = err;
        return false;
    }
    if (existing >= 0) {
        outMsg = "Error: ya existe un archivo o carpeta con ese nombre en el destino.";
        return false;
    }

    if (!copyTree(disk, sb, partStart, srcIno, destIno, copyName, uid, gid, err)) {
        outMsg = err;
        return false;
    }

    writeCopyJournal(disk, partStart, sb, path, destino);

    outMsg = "Copy realizado correctamente.";
    return true;
}

} // namespace cmd