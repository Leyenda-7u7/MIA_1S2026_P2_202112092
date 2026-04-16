#include "commands/chown.hpp"
#include "commands/login.hpp"
#include "commands/mount.hpp"
#include "ext3/journal.hpp"
#include "Structures.hpp"

#include <fstream>
#include <string>
#include <vector>
#include <cstring>
#include <sstream>

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

static bool writeInode(const std::string& disk, const Superblock& sb, int32_t idx, const Inode& ino, std::string& err) {
    int32_t pos = sb.s_inode_start + idx * (int32_t)sizeof(Inode);
    return writeAt(disk, pos, &ino, sizeof(Inode), err);
}

static bool readFolderBlock(const std::string& disk, const Superblock& sb, int32_t bidx, FolderBlock& fb, std::string& err) {
    int32_t pos = sb.s_block_start + bidx * 64;
    return readAt(disk, pos, &fb, sizeof(FolderBlock), err);
}

static bool readBlock64(const std::string& disk, const Superblock& sb, int32_t bidx, Block64& blk, std::string& err) {
    int32_t pos = sb.s_block_start + bidx * 64;
    return readAt(disk, pos, &blk, sizeof(Block64), err);
}

static std::string name12ToString(const char n[12]) {
    size_t len = 0;
    while (len < 12 && n[len] != '\0') len++;
    return std::string(n, n + len);
}

static bool splitAbsPath(const std::string& path, std::vector<std::string>& parts, std::string& err) {
    parts.clear();

    if (path.empty() || path[0] != '/') {
        err = "Error: chown requiere ruta absoluta que inicie con '/'.";
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

static bool readUsersTxt(const std::string& disk, const Superblock& sb, std::string& outContent, std::string& err) {
    outContent.clear();

    Inode usersIno{};
    if (!readInode(disk, sb, 1, usersIno, err)) return false;

    if (usersIno.i_type != '1') {
        err = "Error: users.txt no es un archivo válido.";
        return false;
    }

    for (int i = 0; i < 12; i++) {
        int32_t b = usersIno.i_block[i];
        if (b < 0) continue;

        Block64 blk{};
        if (!readBlock64(disk, sb, b, blk, err)) return false;
        outContent.append(blk.bytes, blk.bytes + 64);
    }

    size_t z = outContent.find('\0');
    if (z != std::string::npos) outContent.resize(z);

    return true;
}

static bool findUserUidByName(const std::string& usersContent, const std::string& username, int32_t& outUid) {
    outUid = -1;

    std::istringstream iss(usersContent);
    std::string line;

    while (std::getline(iss, line)) {
        if (line.empty()) continue;

        std::vector<std::string> cols;
        std::string cur;
        for (char c : line) {
            if (c == ',') {
                cols.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        cols.push_back(cur);

        if (cols.size() < 5) continue;
        if (cols[0] == "0") continue;      
        if (cols[1] != "U") continue;

        if (cols[3] == username) {
            try {
                outUid = std::stoi(cols[0]);
                return true;
            } catch (...) {
                return false;
            }
        }
    }

    return true;
}


// VALIDACIÓN DE PROPIEDAD

static bool validateChownAllowed(const std::string& disk, const Superblock& sb,
                                 int32_t inodeIdx, int32_t currentUid,
                                 bool recursive, std::string& err) {
    Inode ino{};
    if (!readInode(disk, sb, inodeIdx, ino, err)) return false;

    if (currentUid != 1 && ino.i_uid != currentUid) {
        err = "Error: solo puede cambiar propietario de sus propios archivos o carpetas.";
        return false;
    }

    if (!recursive || ino.i_type != '0') return true;

    std::vector<DirChild> children;
    if (!collectDirChildren(disk, sb, inodeIdx, children, err)) return false;

    for (const auto& ch : children) {
        if (!validateChownAllowed(disk, sb, ch.inode, currentUid, true, err)) return false;
    }

    return true;
}

static bool applyChownRecursive(const std::string& disk, const Superblock& sb,
                                int32_t inodeIdx, int32_t newUid,
                                bool recursive, std::string& err) {
    Inode ino{};
    if (!readInode(disk, sb, inodeIdx, ino, err)) return false;

    ino.i_uid = newUid;
    if (!writeInode(disk, sb, inodeIdx, ino, err)) return false;

    if (!recursive || ino.i_type != '0') return true;

    std::vector<DirChild> children;
    if (!collectDirChildren(disk, sb, inodeIdx, children, err)) return false;

    for (const auto& ch : children) {
        if (!applyChownRecursive(disk, sb, ch.inode, newUid, true, err)) return false;
    }

    return true;
}

static void writeChownJournal(const std::string& disk,
                              int32_t partStart,
                              const Superblock& sb,
                              const std::string& path,
                              const std::string& usuario) {
    if (sb.s_filesystem_type != 3) return;

    int32_t journalingStart = partStart + (int32_t)sizeof(Superblock);

    ext3::writeJournal(
        disk,
        journalingStart,
        0,
        "chown",
        path,
        usuario
    );
}


namespace cmd {

bool chown(const std::string& path, bool recursive, const std::string& usuario, std::string& outMsg) {
    if (path.empty() || usuario.empty()) {
        outMsg = "Error: chown requiere -path y -usuario.";
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

    int32_t targetIno = -1;
    if (!resolvePath(disk, sb, path, targetIno, err)) {
        outMsg = err;
        return false;
    }

    // buscar nuevo usuario en users.txt
    std::string usersContent;
    if (!readUsersTxt(disk, sb, usersContent, err)) {
        outMsg = err;
        return false;
    }

    int32_t newUid = -1;
    if (!findUserUidByName(usersContent, usuario, newUid)) {
        outMsg = "Error: no se pudo leer users.txt.";
        return false;
    }
    if (newUid < 0) {
        outMsg = "Error: el usuario indicado no existe.";
        return false;
    }

    int32_t currentUid = cmd::sessionUid();

    // validar permisos/reglas antes de modificar
    if (!validateChownAllowed(disk, sb, targetIno, currentUid, recursive, err)) {
        outMsg = err;
        return false;
    }

    // aplicar cambio
    if (!applyChownRecursive(disk, sb, targetIno, newUid, recursive, err)) {
        outMsg = err;
        return false;
    }

    // journal EXT3
    writeChownJournal(disk, partStart, sb, path, usuario);

    outMsg = "Chown realizado correctamente.";
    return true;
}

}