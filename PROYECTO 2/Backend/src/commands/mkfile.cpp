#include "commands/mkfile.hpp"

#include "commands/login.hpp"   // hasActiveSession(), getSessionPartition(), sessionUid(), sessionGid()
#include "Structures.hpp"       // Superblock, Inode, FolderBlock, Block64
#include "ext2/Bitmap.hpp"

#include <fstream>
#include <cstring>
#include <ctime>
#include <vector>
#include <algorithm>
#include <filesystem>

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
    // root TODO
    if (uid == 1) return true;

    int u = digitToInt(ino.i_perm[0]);
    int g = digitToInt(ino.i_perm[1]);
    int o = digitToInt(ino.i_perm[2]);

    // bit escritura = 2
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

static bool writeBlock64(const std::string& disk, const Superblock& sb, int32_t bidx, const Block64& b, std::string& err) {
    int32_t pos = sb.s_block_start + bidx * 64;
    return writeAt(disk, pos, &b, sizeof(Block64), err);
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
        err = "Error: mkfile requiere ruta absoluta que inicie con '/'.";
        return false;
    }
    std::string cur;
    for (size_t i = 1; i < path.size(); i++) {
        char c = path[i];
        if (c == '/') {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
        } else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);
    return true;
}

// Buscar entrada por nombre dentro de carpeta inode (solo directos)
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
    return true; // no encontrado
}

// Encontrar primer '0' en bitmap (simple)
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

// Agregar entry name->inode dentro de un directorio (crea nuevo folder block si no hay espacio)
static bool addEntryToDir(const std::string& disk, Superblock& sb, int32_t partStart,
                          int32_t dirInoIdx, const std::string& name, int32_t childInoIdx,
                          std::string& err) {
    Inode dir{};
    if (!readInode(disk, sb, dirInoIdx, dir, err)) return false;

    // 1) buscar slot libre en bloques existentes
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

    // 2) no hay slot -> necesito nuevo FolderBlock
    // encontrar bloque libre
    int32_t freeBlock = -1;
    if (!findFirstFreeBit(disk, sb.s_bm_block_start, sb.s_blocks_count, freeBlock, err)) return false;
    if (freeBlock < 0) { err = "Error: no hay bloques libres."; return false; }

    // encontrar espacio i_block libre
    int freePtr = -1;
    for (int i = 0; i < 12; i++) {
        if (dir.i_block[i] < 0) { freePtr = i; break; }
    }
    if (freePtr < 0) { err = "Error: directorio sin espacio (i_block lleno)."; return false; }

    // marcar bitmap + actualizar sb
    if (!Bitmap::setOne(disk, sb.s_bm_block_start, freeBlock, err)) return false;
    sb.s_free_blocks_count--;
    sb.s_first_blo = freeBlock + 1;
    if (!writeSuperblock(disk, partStart, sb, err)) return false;

    // crear bloque carpeta vacío y colocar primera entrada
    FolderBlock fb{};
    for (int k = 0; k < 4; k++) {
        std::memset(fb.b_content[k].b_name, 0, 12);
        fb.b_content[k].b_inodo = -1;
    }
    setName12(fb.b_content[0].b_name, name);
    fb.b_content[0].b_inodo = childInoIdx;

    if (!writeFolderBlock(disk, sb, freeBlock, fb, err)) return false;

    // conectar en el directorio
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

static Inode makeFileInode(int32_t uid, int32_t gid, int32_t size, const char perm[3]) {
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

// Crear carpeta (inode + 1 folderblock con . y ..) y enlazarla al padre
static bool createDirUnder(const std::string& disk, Superblock& sb, int32_t partStart,
                           int32_t parentIdx, const std::string& name,
                           int32_t uid, int32_t gid,
                           int32_t& newDirIdx, std::string& err) {
    // inode libre
    int32_t freeIno = -1;
    if (!findFirstFreeBit(disk, sb.s_bm_inode_start, sb.s_inodes_count, freeIno, err)) return false;
    if (freeIno < 0) { err = "Error: no hay inodos libres."; return false; }

    // block libre
    int32_t freeBlk = -1;
    if (!findFirstFreeBit(disk, sb.s_bm_block_start, sb.s_blocks_count, freeBlk, err)) return false;
    if (freeBlk < 0) { err = "Error: no hay bloques libres."; return false; }

    // marcar bitmaps
    if (!Bitmap::setOne(disk, sb.s_bm_inode_start, freeIno, err)) return false;
    if (!Bitmap::setOne(disk, sb.s_bm_block_start, freeBlk, err)) return false;

    sb.s_free_inodes_count--;
    sb.s_free_blocks_count--;
    sb.s_first_ino = freeIno + 1;
    sb.s_first_blo = freeBlk + 1;
    if (!writeSuperblock(disk, partStart, sb, err)) return false;

    // inode dir
    Inode dino = makeDirInode(uid, gid, "664");
    dino.i_block[0] = freeBlk;

    // folderblock con . y ..
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

    // enlazar en padre
    if (!addEntryToDir(disk, sb, partStart, parentIdx, name, freeIno, err)) return false;

    newDirIdx = freeIno;
    return true;
}

// Asegura carpetas padres (mkdir -p) si recursive=true
static bool ensureParentDirs(const std::string& disk, Superblock& sb, int32_t partStart,
                            const std::vector<std::string>& dirParts,
                            bool recursive,
                            int32_t uid, int32_t gid,
                            int32_t& outParentInode,
                            std::string& err) {
    int32_t current = 0; // root inode
    for (const auto& part : dirParts) {
        int32_t next = -1;
        if (!findEntryInDir(disk, sb, current, part, next, err)) return false;

        if (next >= 0) {
            // existe: debe ser carpeta
            Inode ino{};
            if (!readInode(disk, sb, next, ino, err)) return false;
            if (ino.i_type != '0') { err = "Error: '" + part + "' no es carpeta."; return false; }
            current = next;
            continue;
        }

        // no existe
        if (!recursive) {
            err = "Error: no existen carpetas padres. Use -r para crearlas.";
            return false;
        }

        int32_t created = -1;
        if (!createDirUnder(disk, sb, partStart, current, part, uid, gid, created, err)) return false;
        current = created;
    }
    outParentInode = current;
    return true;
}

// Construir contenido
static bool buildContent(const std::string& contHostPath, int32_t size, std::string& out, std::string& err) {
    // prioridad cont
    if (!contHostPath.empty()) {
        if (!std::filesystem::exists(contHostPath)) {
            err = "Error: -cont no existe en el host: " + contHostPath;
            return false;
        }
        std::ifstream f(contHostPath, std::ios::binary);
        if (!f.is_open()) { err = "Error: no se pudo abrir -cont: " + contHostPath; return false; }
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    }

    if (size < 0) { err = "Error: -size no puede ser negativo."; return false; }
    if (size == 0) { out.clear(); return true; }

    out.clear();
    out.reserve((size_t)size);
    for (int32_t i = 0; i < size; i++) out.push_back(char('0' + (i % 10)));
    return true;
}

namespace cmd {

bool mkfile(const std::string& path,
            bool recursive,
            int32_t size,
            const std::string& contHostPath,
            std::string& outMsg) {

    // 1) sesión
    if (!cmd::hasActiveSession()) {
        outMsg = "Error: no existe una sesión activa. Use login.";
        return false;
    }

    if (path.empty()) {
        outMsg = "Error: mkfile requiere -path.";
        return false;
    }

    // 2) partición de sesión
    std::string disk;
    int32_t partStart = 0, partSize = 0;
    std::string err;
    if (!cmd::getSessionPartition(disk, partStart, partSize, err)) {
        outMsg = err;
        return false;
    }

    // 3) leer superblock
    Superblock sb{};
    if (!readSuperblock(disk, partStart, sb, err)) {
        outMsg = err;
        return false;
    }

    // 4) parsear path
    std::vector<std::string> parts;
    if (!splitAbsPath(path, parts, err)) { outMsg = err; return false; }
    if (parts.empty()) { outMsg = "Error: path inválido."; return false; }

    std::string filename = parts.back();
    parts.pop_back(); // dir parts

    int32_t uid = cmd::sessionUid();
    int32_t gid = cmd::sessionGid();

    // 5) asegurar carpetas padres
    int32_t parentIno = 0;
    if (!ensureParentDirs(disk, sb, partStart, parts, recursive, uid, gid, parentIno, err)) {
        outMsg = err;
        return false;
    }

    // 6) validar permiso escritura en carpeta padre
    Inode parent{};
    if (!readInode(disk, sb, parentIno, parent, err)) { outMsg = err; return false; }
    if (!canWriteDir(parent, uid, gid)) {
        outMsg = "Error: no tiene permiso de escritura en la carpeta padre.";
        return false;
    }

    // 7) si ya existe archivo con ese nombre -> error (no sobreescribimos por ahora)
    int32_t existing = -1;
    if (!findEntryInDir(disk, sb, parentIno, filename, existing, err)) { outMsg = err; return false; }
    if (existing >= 0) {
        outMsg = "Error: el archivo ya existe: " + path;
        return false;
    }

    // 8) construir contenido (cont tiene prioridad)
    std::string content;
    if (!buildContent(contHostPath, size, content, err)) { outMsg = err; return false; }

    // 9) reservar inode para archivo
    int32_t freeIno = -1;
    if (!findFirstFreeBit(disk, sb.s_bm_inode_start, sb.s_inodes_count, freeIno, err)) { outMsg = err; return false; }
    if (freeIno < 0) { outMsg = "Error: no hay inodos libres."; return false; }

    if (!Bitmap::setOne(disk, sb.s_bm_inode_start, freeIno, err)) { outMsg = err; return false; }
    sb.s_free_inodes_count--;
    sb.s_first_ino = freeIno + 1;

    // 10) reservar bloques necesarios para contenido (solo directos)
    int32_t neededBlocks = (int32_t)((content.size() + 63) / 64);
    if (neededBlocks > 12) {
        outMsg = "Error: archivo demasiado grande para esta fase (solo 12 bloques directos).";
        return false;
    }

    std::vector<int32_t> blocks;
    blocks.reserve((size_t)neededBlocks);

    for (int i = 0; i < neededBlocks; i++) {
        int32_t freeBlk = -1;
        if (!findFirstFreeBit(disk, sb.s_bm_block_start, sb.s_blocks_count, freeBlk, err)) { outMsg = err; return false; }
        if (freeBlk < 0) { outMsg = "Error: no hay bloques libres."; return false; }

        if (!Bitmap::setOne(disk, sb.s_bm_block_start, freeBlk, err)) { outMsg = err; return false; }
        sb.s_free_blocks_count--;
        sb.s_first_blo = freeBlk + 1;
        blocks.push_back(freeBlk);
    }

    // guardar SB actualizado
    if (!writeSuperblock(disk, partStart, sb, err)) { outMsg = err; return false; }

    // 11) escribir bloques de contenido
    for (int i = 0; i < neededBlocks; i++) {
        Block64 b{};
        std::memset(b.bytes, 0, 64);

        size_t off = (size_t)i * 64;
        size_t take = std::min<size_t>(64, content.size() - off);
        if (take > 0) std::memcpy(b.bytes, content.data() + off, take);

        if (!writeBlock64(disk, sb, blocks[(size_t)i], b, err)) { outMsg = err; return false; }
    }

    // 12) escribir inode archivo
    Inode fino = makeFileInode(uid, gid, (int32_t)content.size(), "664");
    for (int i = 0; i < neededBlocks; i++) fino.i_block[i] = blocks[(size_t)i];

    if (!writeInode(disk, sb, freeIno, fino, err)) { outMsg = err; return false; }

    // 13) enlazar archivo en carpeta padre
    if (!addEntryToDir(disk, sb, partStart, parentIno, filename, freeIno, err)) {
        outMsg = err;
        return false;
    }

    outMsg = "Archivo creado correctamente: " + path;
    return true;
}

} // namespace cmd