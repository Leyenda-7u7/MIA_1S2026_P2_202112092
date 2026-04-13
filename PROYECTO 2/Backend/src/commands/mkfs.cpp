#include "commands/mkfs.hpp"
#include "commands/mount.hpp"      
#include "Structures.hpp"          
#include "ext2/Ext2Layout.hpp"
#include "ext2/Bitmap.hpp"

#include <fstream>
#include <cstring>
#include <ctime>
#include <algorithm>

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

static Inode makeInodeDir(int uid, int gid, const char perm[3]) {
    Inode ino{};
    ino.i_uid = uid;
    ino.i_gid = gid;
    ino.i_s = 0;
    ino.i_atime = std::time(nullptr);
    ino.i_ctime = std::time(nullptr);
    ino.i_mtime = std::time(nullptr);
    for (int i = 0; i < 15; i++) ino.i_block[i] = -1;
    ino.i_type = '0';                 
    std::memcpy(ino.i_perm, perm, 3);
    return ino;
}

static Inode makeInodeFile(int uid, int gid, int size, const char perm[3]) {
    Inode ino{};
    ino.i_uid = uid;
    ino.i_gid = gid;
    ino.i_s = size;
    ino.i_atime = std::time(nullptr);
    ino.i_ctime = std::time(nullptr);
    ino.i_mtime = std::time(nullptr);
    for (int i = 0; i < 15; i++) ino.i_block[i] = -1;
    ino.i_type = '1';                
    std::memcpy(ino.i_perm, perm, 3);
    return ino;
}

static FolderBlock makeRootFolderBlock() {
    FolderBlock fb{};

    std::memset(fb.b_content[0].b_name, 0, 12);
    std::strncpy(fb.b_content[0].b_name, ".", 11);
    fb.b_content[0].b_inodo = 0;

    std::memset(fb.b_content[1].b_name, 0, 12);
    std::strncpy(fb.b_content[1].b_name, "..", 11);
    fb.b_content[1].b_inodo = 0;

    std::memset(fb.b_content[2].b_name, 0, 12);
    std::strncpy(fb.b_content[2].b_name, "users.txt", 11);
    fb.b_content[2].b_inodo = 1;

    std::memset(fb.b_content[3].b_name, 0, 12);
    fb.b_content[3].b_inodo = -1;

    return fb;
}

static Block64 makeFileBlock64(const std::string& content) {
    Block64 b{};
    std::memset(b.bytes, 0, 64);
    std::memcpy(b.bytes, content.data(), std::min<size_t>(64, content.size()));
    return b;
}

namespace cmd {

bool mkfs(const std::string& id, const std::string& type, std::string& outMsg) {
    std::string t = type;
    if (t.empty()) t = "full";
    // Por ahora solo full
    if (t != "full") {
        outMsg = "Error: mkfs -type solo admite 'full'.";
        return false;
    }

    // 1) Buscar montada por ID (RAM)
    std::string diskPath;
    int32_t partStart = 0;
    int32_t partSize  = 0;
    std::string err;

    if (!cmd::getMountedById(id, diskPath, partStart, partSize, err)) {
        outMsg = err;
        return false;
    }

    // 2) Layout EXT2
    auto L = build_layout<Superblock, Inode, Block64>(partStart, partSize);

    if (L.n_inodes <= 0 || L.n_blocks <= 0) {
        outMsg = "Error: la partición es demasiado pequeña para EXT2.";
        return false;
    }

    // 3) Inicializar bitmaps
    if (!Bitmap::initZeros(diskPath, L.bm_inodes_start, L.n_inodes, err)) {
        outMsg = "Error init bitmap inodos: " + err;
        return false;
    }
    if (!Bitmap::initZeros(diskPath, L.bm_blocks_start, L.n_blocks, err)) {
        outMsg = "Error init bitmap bloques: " + err;
        return false;
    }

    // 4) Reservar: inode0(root), inode1(users), block0(folderroot), block1(file users)
    if (!Bitmap::setOne(diskPath, L.bm_inodes_start, 0, err) ||
        !Bitmap::setOne(diskPath, L.bm_inodes_start, 1, err) ||
        !Bitmap::setOne(diskPath, L.bm_blocks_start, 0, err) ||
        !Bitmap::setOne(diskPath, L.bm_blocks_start, 1, err)) {
        outMsg = "Error marcando bitmaps: " + err;
        return false;
    }

    // 5) Superblock
    Superblock sb{};
    sb.s_filesystem_type = 2;
    sb.s_inodes_count = L.n_inodes;
    sb.s_blocks_count = L.n_blocks;
    sb.s_free_inodes_count = L.n_inodes - 2;
    sb.s_free_blocks_count = L.n_blocks - 2;
    sb.s_mtime = std::time(nullptr);
    sb.s_umtime = std::time(nullptr);
    sb.s_mnt_count = 1;
    sb.s_magic = 0xEF53;
    sb.s_inode_s = (int32_t)sizeof(Inode);
    sb.s_block_s = 64;
    sb.s_first_ino = 2;
    sb.s_first_blo = 2;
    sb.s_bm_inode_start = L.bm_inodes_start;
    sb.s_bm_block_start = L.bm_blocks_start;
    sb.s_inode_start = L.inodes_start;
    sb.s_block_start = L.blocks_start;

    if (!writeAt(diskPath, L.sb_start, &sb, sizeof(Superblock), err)) {
        outMsg = err;
        return false;
    }

    // 6) Inodo raíz
    Inode root = makeInodeDir(1, 1, "664");
    root.i_block[0] = 0;

    // 7) users.txt
    std::string users =
        "1,G,root\n"
        "1,U,root,root,123\n";

    Inode usersIno = makeInodeFile(1, 1, (int)users.size(), "664");
    usersIno.i_block[0] = 1;

    int32_t inode0Pos = L.inodes_start + 0 * (int32_t)sizeof(Inode);
    int32_t inode1Pos = L.inodes_start + 1 * (int32_t)sizeof(Inode);

    if (!writeAt(diskPath, inode0Pos, &root, sizeof(Inode), err)) { outMsg = err; return false; }
    if (!writeAt(diskPath, inode1Pos, &usersIno, sizeof(Inode), err)) { outMsg = err; return false; }

    // 8) Bloque carpeta raíz
    FolderBlock fb = makeRootFolderBlock();
    int32_t block0Pos = L.blocks_start + 0 * 64;
    if (!writeAt(diskPath, block0Pos, &fb, sizeof(FolderBlock), err)) { outMsg = err; return false; }

    // 9) Bloque users.txt (64)
    Block64 b1 = makeFileBlock64(users);
    int32_t block1Pos = L.blocks_start + 1 * 64;
    if (!writeAt(diskPath, block1Pos, &b1, sizeof(Block64), err)) { outMsg = err; return false; }

    outMsg = "MKFS exitoso (EXT2). Partición " + id + " formateada. Se creó /users.txt.";
    return true;
}

} // namespace cmd