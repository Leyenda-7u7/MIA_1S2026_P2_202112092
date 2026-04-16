#include "commands/loss.hpp"
#include "commands/mount.hpp"
#include "Structures.hpp"

#include <fstream>
#include <cstring>
#include <algorithm>

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

static bool zeroFillAt(const std::string& path, int32_t offset, int32_t size, std::string& err) {
    if (size <= 0) return true;

    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) {
        err = "Error: no se pudo abrir el disco para escritura: " + path;
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
            err = "Error: no se pudo limpiar el área del disco.";
            return false;
        }
        remaining -= chunk;
    }

    file.flush();
    return true;
}

namespace cmd {

bool loss(const std::string& id, std::string& outMsg) {
    if (id.empty()) {
        outMsg = "Error: loss requiere -id.";
        return false;
    }

    std::string diskPath;
    int32_t partStart = 0;
    int32_t partSize = 0;
    std::string err;

    if (!getMountedById(id, diskPath, partStart, partSize, err)) {
        outMsg = err;
        return false;
    }

    Superblock sb{};
    if (!readAt(diskPath, partStart, &sb, sizeof(Superblock), err)) {
        outMsg = err;
        return false;
    }

    if (sb.s_magic != 0xEF53) {
        outMsg = "Error: la particion no contiene un sistema de archivos valido.";
        return false;
    }

    if (sb.s_filesystem_type != 3) {
        outMsg = "Error: loss solo puede ejecutarse sobre particiones EXT3.";
        return false;
    }

    int32_t bmInodeSize = sb.s_inodes_count;
    int32_t bmBlockSize = sb.s_blocks_count;
    int32_t inodeAreaSize = sb.s_inodes_count * (int32_t)sizeof(Inode);
    int32_t blockAreaSize = sb.s_blocks_count * 64;

    if (!zeroFillAt(diskPath, sb.s_bm_inode_start, bmInodeSize, err)) {
        outMsg = err;
        return false;
    }

    if (!zeroFillAt(diskPath, sb.s_bm_block_start, bmBlockSize, err)) {
        outMsg = err;
        return false;
    }

    if (!zeroFillAt(diskPath, sb.s_inode_start, inodeAreaSize, err)) {
        outMsg = err;
        return false;
    }

    if (!zeroFillAt(diskPath, sb.s_block_start, blockAreaSize, err)) {
        outMsg = err;
        return false;
    }

    outMsg = "Loss ejecutado correctamente sobre la particion: " + id;
    return true;
}

} 