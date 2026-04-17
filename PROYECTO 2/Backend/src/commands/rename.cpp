#include "commands/rename.hpp"
#include "commands/login.hpp"
#include "commands/mount.hpp"
#include "ext3/journal.hpp"
#include "Structures.hpp"

#include <string>
#include <vector>
#include <fstream>
#include <cstring>

static bool readAt(const std::string& path, int32_t offset, void* data, size_t sz, std::string& err) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        err = "Error: no se pudo abrir el disco.";
        return false;
    }
    file.seekg(offset);
    file.read(reinterpret_cast<char*>(data), sz);
    return true;
}

static bool writeAt(const std::string& path, int32_t offset, const void* data, size_t sz, std::string& err) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) {
        err = "Error: no se pudo abrir el disco.";
        return false;
    }
    file.seekp(offset);
    file.write(reinterpret_cast<const char*>(data), sz);
    return true;
}

static bool readSuperblock(const std::string& disk, int32_t start, Superblock& sb, std::string& err) {
    return readAt(disk, start, &sb, sizeof(Superblock), err);
}

static bool readInode(const std::string& disk, const Superblock& sb, int32_t idx, Inode& ino, std::string& err) {
    return readAt(disk, sb.s_inode_start + idx * sizeof(Inode), &ino, sizeof(Inode), err);
}

static bool readFolderBlock(const std::string& disk, const Superblock& sb, int32_t b, FolderBlock& fb, std::string& err) {
    return readAt(disk, sb.s_block_start + b * 64, &fb, sizeof(FolderBlock), err);
}

static bool writeFolderBlock(const std::string& disk, const Superblock& sb, int32_t b, const FolderBlock& fb, std::string& err) {
    return writeAt(disk, sb.s_block_start + b * 64, &fb, sizeof(FolderBlock), err);
}

static std::string name12(const char n[12]) {
    return std::string(n);
}

static bool splitPath(const std::string& path, std::vector<std::string>& parts) {
    std::string tmp;
    for (char c : path) {
        if (c == '/') {
            if (!tmp.empty()) {
                parts.push_back(tmp);
                tmp.clear();
            }
        } else tmp += c;
    }
    if (!tmp.empty()) parts.push_back(tmp);
    return true;
}

static void tryWriteRenameJournal(const std::string& disk,
                                  int32_t partStart,
                                  const Superblock& sb,
                                  const std::string& path,
                                  const std::string& newName) {
    if (sb.s_filesystem_type != 3) return;

    int32_t journalingStart = partStart + (int32_t)sizeof(Superblock);

    // Por ahora usamos índice 0 igual que en mkdir/remove.
    // Luego se mejora para buscar el siguiente journal libre.
    ext3::writeJournal(
        disk,
        journalingStart,
        0,
        "rename",
        path,
        newName
    );
}

namespace cmd {

bool rename(const std::string& path, const std::string& newName, std::string& outMsg) {

    if (path.empty() || newName.empty()) {
        outMsg = "Error: rename requiere -path y -name.";
        return false;
    }

    if (!cmd::hasActiveSession()) {
        outMsg = "Error: no hay sesión activa.";
        return false;
    }

    std::string disk;
    int32_t partStart, partSize;
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
    splitPath(path, parts);

    int32_t current = 0; 

    // navegar hasta padre
    for (size_t i = 0; i < parts.size() - 1; i++) {
        Inode ino{};
        readInode(disk, sb, current, ino, err);

        bool found = false;

        for (int j = 0; j < 12; j++) {
            if (ino.i_block[j] < 0) continue;

            FolderBlock fb{};
            readFolderBlock(disk, sb, ino.i_block[j], fb, err);

            for (int k = 0; k < 4; k++) {
                if (name12(fb.b_content[k].b_name) == parts[i]) {
                    current = fb.b_content[k].b_inodo;
                    found = true;
                }
            }
        }

        if (!found) {
            outMsg = "Error: ruta no existe.";
            return false;
        }
    }

    // buscar archivo objetivo
    Inode parent{};
    readInode(disk, sb, current, parent, err);

    for (int j = 0; j < 12; j++) {
        if (parent.i_block[j] < 0) continue;

        FolderBlock fb{};
        readFolderBlock(disk, sb, parent.i_block[j], fb, err);

        // validar que NO exista con el nuevo nombre
        for (int k = 0; k < 4; k++) {
            if (name12(fb.b_content[k].b_name) == newName) {
                outMsg = "Error: ya existe un archivo con ese nombre.";
                return false;
            }
        }

        for (int k = 0; k < 4; k++) {
            if (name12(fb.b_content[k].b_name) == parts.back()) {

                // cambiar nombre
                std::memset(fb.b_content[k].b_name, 0, 12);
                std::strncpy(fb.b_content[k].b_name, newName.c_str(), 11);

                if (!writeFolderBlock(disk, sb, parent.i_block[j], fb, err)) {
                    outMsg = err;
                    return false;
                }

                // JOURNAL (EXT3)

                tryWriteRenameJournal(disk, partStart, sb, path, newName);

                outMsg = "Rename exitoso: " + path + " -> " + newName;
                return true;
            }
        }
    }

    outMsg = "Error: archivo no encontrado.";
    return false;
}

}