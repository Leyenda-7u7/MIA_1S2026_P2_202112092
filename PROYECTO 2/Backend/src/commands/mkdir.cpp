#include "commands/mkdir.hpp"

#include "commands/login.hpp"
#include "Structures.hpp"
#include "ext2/Bitmap.hpp"

#include <fstream>
#include <cstring>
#include <ctime>
#include <vector>
#include <algorithm>

// ----------------- IO helpers -----------------
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

// ----------------- Permisos UGO -----------------
static int digitToInt(char c) { return (c >= '0' && c <= '7') ? (c - '0') : 0; }

static bool canWriteDir(const Inode& ino, int32_t uid, int32_t gid) {
    if (uid == 1) return true; // root bypass

    int u = digitToInt(ino.i_perm[0]);
    int g = digitToInt(ino.i_perm[1]);
    int o = digitToInt(ino.i_perm[2]);

    if (uid == ino.i_uid) return (u & 2) != 0;
    if (gid == ino.i_gid) return (g & 2) != 0;
    return (o & 2) != 0;
}

// ----------------- EXT2 helpers -----------------
static bool readSuperblock(const std::string& disk, int32_t partStart, Superblock& sb, std::string& err) {
    if (!readAt(disk, partStart, &sb, sizeof(Superblock), err)) return false;
    if (sb.s_magic != 0xEF53) { err = "Error: la partición no parece EXT2 (magic inválido)."; return false; }
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

static void setName12(char out[12], const std::string& s) {
    std::memset(out, 0, 12);
    std::strncpy(out, s.c_str(), 11);
}

static bool splitAbsPath(const std::string& path, std::vector<std::string>& parts, std::string& err) {
    parts.clear();
    if (path.empty() || path[0] != '/') { err = "Error: mkdir requiere ruta absoluta que inicie con '/'."; return false; }
    std::string cur;
    for (size_t i = 1; i < path.size(); i++) {
        char c = path[i];
        if (c == '/') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);
    return true;
}

// buscar entry dentro de dir inode (solo directos)
static bool findEntryInDir(const std::string& disk, const Superblock& sb, int32_t dirIno,
                           const std::string& name, int32_t& outInode, std::string& err) {
    Inode dino{};
    if (!readInode(disk, sb, dirIno, dino, err)) return false;
    if (dino.i_type != '0') { err = "Error: no es carpeta."; return false; }

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

// primer '0' en bitmap
static bool findFirstFreeBit(const std::string& disk, int32_t bmStart, int32_t count, int32_t& outIndex, std::string& err) {
    std::ifstream file(disk, std::ios::binary);
    if (!file.is_open()) { err = "Error: no se pudo abrir disco para leer bitmap."; return false; }
    file.seekg(bmStart, std::ios::beg);

    for (int32_t i = 0; i < count; i++) {
        char c;
        file.read(&c, 1);
        if (!file) { err = "Error leyendo bitmap."; return false; }
        if (c == '0') { outIndex = i; return true; }
    }
    outIndex = -1;
    return true;
}

// agrega entry al dir, crea nuevo folderblock si hace falta
static bool addEntryToDir(const std::string& disk, Superblock& sb, int32_t partStart,
                          int32_t dirInoIdx, const std::string& name, int32_t childInoIdx,
                          std::string& err) {
    Inode dir{};
    if (!readInode(disk, sb, dirInoIdx, dir, err)) return false;

    // slot libre en bloques existentes
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

    // necesito nuevo folderblock
    int32_t freeBlock = -1;
    if (!findFirstFreeBit(disk, sb.s_bm_block_start, sb.s_blocks_count, freeBlock, err)) return false;
    if (freeBlock < 0) { err = "Error: no hay bloques libres."; return false; }

    int freePtr = -1;
    for (int i = 0; i < 12; i++) {
        if (dir.i_block[i] < 0) { freePtr = i; break; }
    }
    if (freePtr < 0) { err = "Error: directorio sin espacio (i_block lleno)."; return false; }

    if (!Bitmap::setOne(disk, sb.s_bm_block_start, freeBlock, err)) return false;
    sb.s_free_blocks_count--;
    sb.s_first_blo = freeBlock + 1;
    if (!writeSuperblock(disk, partStart, sb, err)) return false;

    FolderBlock fb{};
    for (int k = 0; k < 4; k++) {
        std::memset(fb.b_content[k].b_name, 0, 12);
        fb.b_content[k].b_inodo = -1;
    }
    setName12(fb.b_content[0].b_name, name);
    fb.b_content[0].b_inodo = childInoIdx;

    if (!writeFolderBlock(disk, sb, freeBlock, fb, err)) return false;

    dir.i_block[freePtr] = freeBlock;
    if (!writeInode(disk, sb, dirInoIdx, dir, err)) return false;

    return true;
}

static Inode makeDirInode(int32_t uid, int32_t gid, const char perm[3]) {
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

static bool createDirUnder(const std::string& disk, Superblock& sb, int32_t partStart,
                           int32_t parentIdx, const std::string& name,
                           int32_t uid, int32_t gid,
                           int32_t& newDirIdx, std::string& err) {
    int32_t freeIno = -1;
    if (!findFirstFreeBit(disk, sb.s_bm_inode_start, sb.s_inodes_count, freeIno, err)) return false;
    if (freeIno < 0) { err = "Error: no hay inodos libres."; return false; }

    int32_t freeBlk = -1;
    if (!findFirstFreeBit(disk, sb.s_bm_block_start, sb.s_blocks_count, freeBlk, err)) return false;
    if (freeBlk < 0) { err = "Error: no hay bloques libres."; return false; }

    if (!Bitmap::setOne(disk, sb.s_bm_inode_start, freeIno, err)) return false;
    if (!Bitmap::setOne(disk, sb.s_bm_block_start, freeBlk, err)) return false;

    sb.s_free_inodes_count--;
    sb.s_free_blocks_count--;
    sb.s_first_ino = freeIno + 1;
    sb.s_first_blo = freeBlk + 1;
    if (!writeSuperblock(disk, partStart, sb, err)) return false;

    Inode dino = makeDirInode(uid, gid, "664");
    dino.i_block[0] = freeBlk;

    FolderBlock fb{};
    for (int k = 0; k < 4; k++) {
        std::memset(fb.b_content[k].b_name, 0, 12);
        fb.b_content[k].b_inodo = -1;
    }
    setName12(fb.b_content[0].b_name, ".");
    fb.b_content[0].b_inodo = freeIno;
    setName12(fb.b_content[1].b_name, "..");
    fb.b_content[1].b_inodo = parentIdx;

    if (!writeInode(disk, sb, freeIno, dino, err)) return false;
    if (!writeFolderBlock(disk, sb, freeBlk, fb, err)) return false;

    if (!addEntryToDir(disk, sb, partStart, parentIdx, name, freeIno, err)) return false;

    newDirIdx = freeIno;
    return true;
}

static bool ensureParents(const std::string& disk, Superblock& sb, int32_t partStart,
                          const std::vector<std::string>& dirParts,
                          bool parents,
                          int32_t uid, int32_t gid,
                          int32_t& outParentInode,
                          std::string& err) {
    int32_t current = 0; // root
    for (const auto& part : dirParts) {
        int32_t next = -1;
        if (!findEntryInDir(disk, sb, current, part, next, err)) return false;

        if (next >= 0) {
            Inode ino{};
            if (!readInode(disk, sb, next, ino, err)) return false;
            if (ino.i_type != '0') { err = "Error: '" + part + "' no es carpeta."; return false; }
            current = next;
            continue;
        }

        if (!parents) {
            err = "Error: no existen carpetas padres. Use -p para crearlas.";
            return false;
        }

        int32_t created = -1;
        if (!createDirUnder(disk, sb, partStart, current, part, uid, gid, created, err)) return false;
        current = created;
    }

    outParentInode = current;
    return true;
}

namespace cmd {

bool mkdir(const std::string& path, bool parents, std::string& outMsg) {
    if (!cmd::hasActiveSession()) {
        outMsg = "Error: no existe una sesión activa. Use login.";
        return false;
    }

    if (path.empty()) {
        outMsg = "Error: mkdir requiere -path.";
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
    if (!splitAbsPath(path, parts, err)) { outMsg = err; return false; }
    if (parts.empty()) { outMsg = "Error: path inválido."; return false; }

    std::string newDirName = parts.back();
    parts.pop_back();

    int32_t uid = cmd::sessionUid();
    int32_t gid = cmd::sessionGid();

    // asegurar padres
    int32_t parentIno = 0;
    if (!ensureParents(disk, sb, partStart, parts, parents, uid, gid, parentIno, err)) {
        outMsg = err;
        return false;
    }

    // permiso escritura en padre
    Inode parent{};
    if (!readInode(disk, sb, parentIno, parent, err)) { outMsg = err; return false; }
    if (!canWriteDir(parent, uid, gid)) {
        outMsg = "Error: no tiene permiso de escritura en la carpeta padre.";
        return false;
    }

    // ¿ya existe?
    int32_t existing = -1;
    if (!findEntryInDir(disk, sb, parentIno, newDirName, existing, err)) { outMsg = err; return false; }
    if (existing >= 0) {
        if (parents) { outMsg = "La carpeta ya existe: " + path; return true; }
        outMsg = "Error: la carpeta ya existe: " + path;
        return false;
    }

    // crear carpeta final
    int32_t created = -1;
    if (!createDirUnder(disk, sb, partStart, parentIno, newDirName, uid, gid, created, err)) {
        outMsg = err;
        return false;
    }

    outMsg = "Carpeta creada correctamente: " + path;
    return true;
}

} // namespace cmd