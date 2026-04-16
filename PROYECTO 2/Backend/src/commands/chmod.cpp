#include "commands/chmod.hpp"
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

static bool writeInode(const std::string& disk, const Superblock& sb, int32_t idx, const Inode& ino, std::string& err) {
    int32_t pos = sb.s_inode_start + idx * (int32_t)sizeof(Inode);
    return writeAt(disk, pos, &ino, sizeof(Inode), err);
}

static bool readFolderBlock(const std::string& disk, const Superblock& sb, int32_t bidx, FolderBlock& fb, std::string& err) {
    int32_t pos = sb.s_block_start + bidx * 64;
    return readAt(disk, pos, &fb, sizeof(FolderBlock), err);
}

static std::string name12ToString(const char n[12]) {
    size_t len = 0;
    while (len < 12 && n[len] != '\0') len++;
    return std::string(n, n + len);
}

static bool splitAbsPath(const std::string& path, std::vector<std::string>& parts, std::string& err) {
    parts.clear();

    if (path.empty() || path[0] != '/') {
        err = "Error: chmod requiere ruta absoluta que inicie con '/'.";
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

static bool validUGO(const std::string& ugo) {
    if (ugo.size() != 3) return false;
    for (char c : ugo) {
        if (c < '0' || c > '7') return false;
    }
    return true;
}

static bool applyChmodRecursive(const std::string& disk, const Superblock& sb,
                                int32_t inodeIdx, const std::string& ugo,
                                bool recursive, std::string& err) {
    Inode ino{};
    if (!readInode(disk, sb, inodeIdx, ino, err)) return false;

    ino.i_perm[0] = ugo[0];
    ino.i_perm[1] = ugo[1];
    ino.i_perm[2] = ugo[2];

    if (!writeInode(disk, sb, inodeIdx, ino, err)) return false;

    if (!recursive || ino.i_type != '0') return true;

    std::vector<DirChild> children;
    if (!collectDirChildren(disk, sb, inodeIdx, children, err)) return false;

    for (const auto& ch : children) {
        if (!applyChmodRecursive(disk, sb, ch.inode, ugo, true, err)) return false;
    }

    return true;
}

static void writeChmodJournal(const std::string& disk,
                              int32_t partStart,
                              const Superblock& sb,
                              const std::string& path,
                              const std::string& ugo) {
    if (sb.s_filesystem_type != 3) return;

    int32_t journalingStart = partStart + (int32_t)sizeof(Superblock);

    ext3::writeJournal(
        disk,
        journalingStart,
        0,
        "chmod",
        path,
        ugo
    );
}

namespace cmd {

bool chmod(const std::string& path, bool recursive, const std::string& ugo, std::string& outMsg) {
    if (path.empty() || ugo.empty()) {
        outMsg = "Error: chmod requiere -path y -ugo.";
        return false;
    }

    if (!cmd::hasActiveSession()) {
        outMsg = "Error: no existe una sesión activa. Use login.";
        return false;
    }

    // solo root
    if (cmd::sessionUid() != 1) {
        outMsg = "Error: solo el usuario root puede ejecutar chmod.";
        return false;
    }

    if (!validUGO(ugo)) {
        outMsg = "Error: -ugo debe contener exactamente 3 dígitos entre 0 y 7.";
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

    if (!applyChmodRecursive(disk, sb, targetIno, ugo, recursive, err)) {
        outMsg = err;
        return false;
    }

    writeChmodJournal(disk, partStart, sb, path, ugo);

    outMsg = "Chmod realizado correctamente.";
    return true;
}

} 