#include "commands/mkfs.hpp"
#include "commands/mount.hpp"
#include "Structures.hpp"
#include "ext2/Ext2Layout.hpp"
#include "ext2/Bitmap.hpp"
#include "ext3/Ext3Layout.hpp"
#include "ext3/journal.hpp"

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

static bool zeroFillAt(const std::string& path, int32_t offset, int32_t size, std::string& err) {
    if (size <= 0) return true;

    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) {
        err = "Error: no se pudo abrir el disco: " + path;
        return false;
    }

    file.seekp(offset, std::ios::beg);

    char buffer[1024];
    std::memset(buffer, 0, sizeof(buffer));

    int32_t remaining = size;
    while (remaining > 0) {
        int32_t chunk = std::min<int32_t>(remaining, (int32_t)sizeof(buffer));
        file.write(buffer, chunk);
        if (!file) {
            err = "Error: no se pudo limpiar la partición.";
            return false;
        }
        remaining -= chunk;
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

static Journal makeEmptyJournal() {
    Journal j{};
    j.j_count = 0;

    std::memset(j.j_content.i_operation, 0, sizeof(j.j_content.i_operation));
    std::memset(j.j_content.i_path, 0, sizeof(j.j_content.i_path));
    std::memset(j.j_content.i_content, 0, sizeof(j.j_content.i_content));

    j.j_content.i_date = 0.0f;
    return j;
}

static void tryWriteMkfsJournal(const std::string& disk,
                                int32_t partStart,
                                const Superblock& sb,
                                const std::string& id,
                                const std::string& fs) {
    if (sb.s_filesystem_type != 3) return;

    int32_t journalingStart = partStart + (int32_t)sizeof(Superblock);

    ext3::writeJournal(
        disk,
        journalingStart,
        0,
        "mkfs",
        id,
        fs
    );
}

namespace cmd {

bool mkfs(const std::string& id, const std::string& type, const std::string& fs, std::string& outMsg) {
    std::string t = type.empty() ? "full" : type;
    std::string f = fs.empty() ? "2fs" : fs;

    std::transform(t.begin(), t.end(), t.begin(), ::tolower);
    std::transform(f.begin(), f.end(), f.begin(), ::tolower);

    if (t != "full") {
        outMsg = "Error: mkfs -type solo admite 'full'.";
        return false;
    }

    if (f != "2fs" && f != "3fs") {
        outMsg = "Error: mkfs -fs solo admite '2fs' o '3fs'.";
        return false;
    }

    // 1) Buscar partición montada por ID
    std::string diskPath;
    int32_t partStart = 0;
    int32_t partSize  = 0;
    std::string err;

    if (!cmd::getMountedById(id, diskPath, partStart, partSize, err)) {
        outMsg = err;
        return false;
    }

    // 2) Formateo full: limpiar toda la partición
    if (!zeroFillAt(diskPath, partStart, partSize, err)) {
        outMsg = err;
        return false;
    }

    // Contenido base común
    std::string users =
        "1,G,root\n"
        "1,U,root,root,123\n";

    // =========================================================
    // EXT2
    // =========================================================
    if (f == "2fs") {
        auto L = build_layout<Superblock, Inode, Block64>(partStart, partSize);

        if (L.n_inodes <= 0 || L.n_blocks <= 0) {
            outMsg = "Error: la partición es demasiado pequeña para EXT2.";
            return false;
        }

        if (!Bitmap::initZeros(diskPath, L.bm_inodes_start, L.n_inodes, err)) {
            outMsg = "Error init bitmap inodos: " + err;
            return false;
        }
        if (!Bitmap::initZeros(diskPath, L.bm_blocks_start, L.n_blocks, err)) {
            outMsg = "Error init bitmap bloques: " + err;
            return false;
        }

        if (!Bitmap::setOne(diskPath, L.bm_inodes_start, 0, err) ||
            !Bitmap::setOne(diskPath, L.bm_inodes_start, 1, err) ||
            !Bitmap::setOne(diskPath, L.bm_blocks_start, 0, err) ||
            !Bitmap::setOne(diskPath, L.bm_blocks_start, 1, err)) {
            outMsg = "Error marcando bitmaps: " + err;
            return false;
        }

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

        Inode root = makeInodeDir(1, 1, "664");
        root.i_block[0] = 0;

        Inode usersIno = makeInodeFile(1, 1, (int)users.size(), "664");
        usersIno.i_block[0] = 1;

        int32_t inode0Pos = L.inodes_start + 0 * (int32_t)sizeof(Inode);
        int32_t inode1Pos = L.inodes_start + 1 * (int32_t)sizeof(Inode);

        if (!writeAt(diskPath, inode0Pos, &root, sizeof(Inode), err)) { outMsg = err; return false; }
        if (!writeAt(diskPath, inode1Pos, &usersIno, sizeof(Inode), err)) { outMsg = err; return false; }

        FolderBlock fb = makeRootFolderBlock();
        int32_t block0Pos = L.blocks_start + 0 * 64;
        if (!writeAt(diskPath, block0Pos, &fb, sizeof(FolderBlock), err)) { outMsg = err; return false; }

        Block64 b1 = makeFileBlock64(users);
        int32_t block1Pos = L.blocks_start + 1 * 64;
        if (!writeAt(diskPath, block1Pos, &b1, sizeof(Block64), err)) { outMsg = err; return false; }

        outMsg = "MKFS exitoso (EXT2). Partición " + id + " formateada. Se creó /users.txt.";
        return true;
    }

    // =========================================================
    // EXT3
    // =========================================================
    auto L3 = build_layout_ext3<Superblock, Journal, Inode, Block64>(partStart, partSize);

    if (L3.n_inodes <= 0 || L3.n_blocks <= 0 || L3.n_journaling <= 0) {
        outMsg = "Error: la partición es demasiado pequeña para EXT3.";
        return false;
    }

    // 1) Inicializar journals 
    for (int32_t i = 0; i < L3.n_journaling; i++) {
        Journal j = makeEmptyJournal();
        int32_t pos = L3.journaling_start + i * (int32_t)sizeof(Journal);
        if (!writeAt(diskPath, pos, &j, sizeof(Journal), err)) {
            outMsg = "Error escribiendo journaling: " + err;
            return false;
        }
    }

    // 2) Inicializar bitmaps
    if (!Bitmap::initZeros(diskPath, L3.bm_inodes_start, L3.n_inodes, err)) {
        outMsg = "Error init bitmap inodos: " + err;
        return false;
    }
    if (!Bitmap::initZeros(diskPath, L3.bm_blocks_start, L3.n_blocks, err)) {
        outMsg = "Error init bitmap bloques: " + err;
        return false;
    }

    // 3) Reservar root y users
    if (!Bitmap::setOne(diskPath, L3.bm_inodes_start, 0, err) ||
        !Bitmap::setOne(diskPath, L3.bm_inodes_start, 1, err) ||
        !Bitmap::setOne(diskPath, L3.bm_blocks_start, 0, err) ||
        !Bitmap::setOne(diskPath, L3.bm_blocks_start, 1, err)) {
        outMsg = "Error marcando bitmaps: " + err;
        return false;
    }

    // 4) Superblock EXT3
    Superblock sb{};
    sb.s_filesystem_type = 3;
    sb.s_inodes_count = L3.n_inodes;
    sb.s_blocks_count = L3.n_blocks;
    sb.s_free_inodes_count = L3.n_inodes - 2;
    sb.s_free_blocks_count = L3.n_blocks - 2;
    sb.s_mtime = std::time(nullptr);
    sb.s_umtime = std::time(nullptr);
    sb.s_mnt_count = 1;
    sb.s_magic = 0xEF53;
    sb.s_inode_s = (int32_t)sizeof(Inode);
    sb.s_block_s = 64;
    sb.s_first_ino = 2;
    sb.s_first_blo = 2;
    sb.s_bm_inode_start = L3.bm_inodes_start;
    sb.s_bm_block_start = L3.bm_blocks_start;
    sb.s_inode_start = L3.inodes_start;
    sb.s_block_start = L3.blocks_start;

    if (!writeAt(diskPath, L3.sb_start, &sb, sizeof(Superblock), err)) {
        outMsg = err;
        return false;
    }

    // 5) Inodos
    Inode root = makeInodeDir(1, 1, "664");
    root.i_block[0] = 0;

    Inode usersIno = makeInodeFile(1, 1, (int)users.size(), "664");
    usersIno.i_block[0] = 1;

    int32_t inode0Pos = L3.inodes_start + 0 * (int32_t)sizeof(Inode);
    int32_t inode1Pos = L3.inodes_start + 1 * (int32_t)sizeof(Inode);

    if (!writeAt(diskPath, inode0Pos, &root, sizeof(Inode), err)) { outMsg = err; return false; }
    if (!writeAt(diskPath, inode1Pos, &usersIno, sizeof(Inode), err)) { outMsg = err; return false; }

    // 6) Bloques
    FolderBlock fb = makeRootFolderBlock();
    int32_t block0Pos = L3.blocks_start + 0 * 64;
    if (!writeAt(diskPath, block0Pos, &fb, sizeof(FolderBlock), err)) { outMsg = err; return false; }

    Block64 b1 = makeFileBlock64(users);
    int32_t block1Pos = L3.blocks_start + 1 * 64;
    if (!writeAt(diskPath, block1Pos, &b1, sizeof(Block64), err)) { outMsg = err; return false; }

    // 7) Escribir journal de mkfs
    tryWriteMkfsJournal(diskPath, partStart, sb, id, fs);

    outMsg = "MKFS exitoso (EXT3). Partición " + id + " formateada. Se creó /users.txt y journaling.";
    return true;
}

}