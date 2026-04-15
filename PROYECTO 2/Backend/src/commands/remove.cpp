#include "commands/remove.hpp"
#include "commands/login.hpp"
#include "commands/mount.hpp"
#include "Structures.hpp"
#include "ext2/Bitmap.hpp"
#include "ext3/journal.hpp"

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

static std::string name12ToString(const char n[12]) {
    size_t len = 0;
    while (len < 12 && n[len] != '\0') len++;
    return std::string(n, n + len);
}

static bool splitAbsPath(const std::string& path, std::vector<std::string>& parts, std::string& err) {
    parts.clear();
    if (path.empty() || path[0] != '/') {
        err = "Error: remove requiere ruta absoluta que inicie con '/'.";
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
    if (uid == 1) return true; // root bypass

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

static bool resolveParentAndTarget(const std::string& disk, const Superblock& sb,
                                   const std::vector<std::string>& parts,
                                   int32_t& parentIno, int32_t& targetIno,
                                   std::string& targetName,
                                   std::string& err) {
    if (parts.empty()) {
        err = "Error: path inválido.";
        return false;
    }

    targetName = parts.back();
    parentIno = 0; // root

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
        err = "Error: no existe el archivo o carpeta: " + targetName;
        return false;
    }

    return true;
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

static bool releaseFileInodeAndBlocks(const std::string& disk, Superblock& sb, int32_t partStart,
                                      int32_t inodeIdx, std::string& err) {
    Inode ino{};
    if (!readInode(disk, sb, inodeIdx, ino, err)) return false;

    if (ino.i_type != '1') {
        err = "Error: el inodo no corresponde a un archivo.";
        return false;
    }

    for (int i = 0; i < 12; i++) {
        int32_t b = ino.i_block[i];
        if (b < 0) continue;

        if (!Bitmap::setZero(disk, sb.s_bm_block_start, b, err)) return false;
        sb.s_free_blocks_count++;
        if (b < sb.s_first_blo) sb.s_first_blo = b;
    }

    if (!Bitmap::setZero(disk, sb.s_bm_inode_start, inodeIdx, err)) return false;
    sb.s_free_inodes_count++;
    if (inodeIdx < sb.s_first_ino) sb.s_first_ino = inodeIdx;

    Inode empty{};
    if (!writeInode(disk, sb, inodeIdx, empty, err)) return false;

    if (!writeSuperblock(disk, partStart, sb, err)) return false;
    return true;
}

static bool releaseDirInodeAndBlocks(const std::string& disk, Superblock& sb, int32_t partStart,
                                     int32_t inodeIdx, std::string& err) {
    Inode ino{};
    if (!readInode(disk, sb, inodeIdx, ino, err)) return false;

    if (ino.i_type != '0') {
        err = "Error: el inodo no corresponde a una carpeta.";
        return false;
    }

    for (int i = 0; i < 12; i++) {
        int32_t b = ino.i_block[i];
        if (b < 0) continue;

        if (!Bitmap::setZero(disk, sb.s_bm_block_start, b, err)) return false;
        sb.s_free_blocks_count++;
        if (b < sb.s_first_blo) sb.s_first_blo = b;
    }

    if (!Bitmap::setZero(disk, sb.s_bm_inode_start, inodeIdx, err)) return false;
    sb.s_free_inodes_count++;
    if (inodeIdx < sb.s_first_ino) sb.s_first_ino = inodeIdx;

    Inode empty{};
    if (!writeInode(disk, sb, inodeIdx, empty, err)) return false;

    if (!writeSuperblock(disk, partStart, sb, err)) return false;
    return true;
}

static bool validateRemovableTree(const std::string& disk, const Superblock& sb,
                                  int32_t inodeIdx, int32_t uid, int32_t gid,
                                  std::string& err) {
    Inode ino{};
    if (!readInode(disk, sb, inodeIdx, ino, err)) return false;

    if (!canWriteInode(ino, uid, gid)) {
        err = "Error: no tiene permisos suficientes para eliminar el contenido.";
        return false;
    }

    if (ino.i_type == '1') return true;

    std::vector<DirChild> children;
    if (!collectDirChildren(disk, sb, inodeIdx, children, err)) return false;

    for (const auto& ch : children) {
        if (!validateRemovableTree(disk, sb, ch.inode, uid, gid, err)) return false;
    }

    return true;
}

static bool deleteTree(const std::string& disk, Superblock& sb, int32_t partStart,
                       int32_t inodeIdx, std::string& err) {
    Inode ino{};
    if (!readInode(disk, sb, inodeIdx, ino, err)) return false;

    if (ino.i_type == '1') {
        return releaseFileInodeAndBlocks(disk, sb, partStart, inodeIdx, err);
    }

    std::vector<DirChild> children;
    if (!collectDirChildren(disk, sb, inodeIdx, children, err)) return false;

    for (const auto& ch : children) {
        if (!deleteTree(disk, sb, partStart, ch.inode, err)) return false;
        if (!removeEntryFromParent(disk, sb, inodeIdx, ch.name, err)) return false;
    }

    return releaseDirInodeAndBlocks(disk, sb, partStart, inodeIdx, err);
}

static void tryWriteRemoveJournal(const std::string& disk,
                                  int32_t partStart,
                                  const Superblock& sb,
                                  const std::string& path) {
    if (sb.s_filesystem_type != 3) return;

    int32_t journalingStart = partStart + (int32_t)sizeof(Superblock);

    ext3::writeJournal(
        disk,
        journalingStart,
        0,
        "remove",
        path,
        "-"
    );
}

namespace cmd {

bool remove(const std::string& path, std::string& outMsg) {
    if (path.empty()) {
        outMsg = "Error: remove requiere -path.";
        return false;
    }

    if (path == "/") {
        outMsg = "Error: no se permite eliminar la raíz del sistema.";
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

    std::vector<std::string> parts;
    if (!splitAbsPath(path, parts, err)) {
        outMsg = err;
        return false;
    }

    int32_t parentIno = -1;
    int32_t targetIno = -1;
    std::string targetName;

    if (!resolveParentAndTarget(disk, sb, parts, parentIno, targetIno, targetName, err)) {
        outMsg = err;
        return false;
    }

    int32_t uid = cmd::sessionUid();
    int32_t gid = cmd::sessionGid();

    // validar permiso también sobre la carpeta padre
    Inode parent{};
    if (!readInode(disk, sb, parentIno, parent, err)) {
        outMsg = err;
        return false;
    }
    if (!canWriteInode(parent, uid, gid)) {
        outMsg = "Error: no tiene permiso de escritura en la carpeta padre.";
        return false;
    }

    // validar todo el árbol antes de eliminar
    if (!validateRemovableTree(disk, sb, targetIno, uid, gid, err)) {
        outMsg = err;
        return false;
    }

    // eliminar árbol
    if (!deleteTree(disk, sb, partStart, targetIno, err)) {
        outMsg = err;
        return false;
    }

    // quitar entrada en padre
    if (!removeEntryFromParent(disk, sb, parentIno, targetName, err)) {
        outMsg = err;
        return false;
    }

    // =====================
    // JOURNAL (EXT3)
    // =====================
    tryWriteRemoveJournal(disk, partStart, sb, path);

    outMsg = "Elemento eliminado correctamente: " + path;
    return true;
}

} // namespace cmd