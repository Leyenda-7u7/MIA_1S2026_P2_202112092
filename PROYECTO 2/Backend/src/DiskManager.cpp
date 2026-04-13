#include "DiskManager.hpp"

#include "commands/mkdisk.hpp"
#include "commands/rmdisk.hpp"
#include "commands/fdisk.hpp"
#include "commands/mount.hpp"
#include "commands/mounted.hpp"
#include "commands/mkfs.hpp"

#include <unordered_map>

bool DiskManager::mkdisk(int32_t size,
                         const std::string& unitStr,
                         const std::string& fitStr,
                         const std::string& path,
                         std::string& outMsg) {
    return cmd::mkdisk(size, unitStr, fitStr, path, outMsg);
}

bool DiskManager::rmdisk(const std::string& path,
                         std::string& outMsg) {
    return cmd::rmdisk(path, outMsg);
}

// Wrapper que tu CommandParser busca: fdisk(params, outMsg)
bool DiskManager::fdisk(const std::unordered_map<std::string, std::string>& params,
                        std::string& outMsg) {

    // Lee params típicos (en tu CommandParser tú los guardas con key "-size", etc.)
    auto get = [&](const std::string& k) -> std::string {
        auto it = params.find(k);
        return (it == params.end()) ? "" : it->second;
    };

    std::string sizeStr = get("-size");
    std::string unitStr = get("-unit");
    std::string path    = get("-path");
    std::string typeStr = get("-type");
    std::string fitStr  = get("-fit");
    std::string name    = get("-name");

    if (path.empty() || name.empty()) {
        outMsg = "Error: fdisk requiere -path y -name.";
        return false;
    }
    if (sizeStr.empty()) {
        outMsg = "Error: fdisk requiere -size para crear particiones.";
        return false;
    }

    int32_t size = 0;
    try {
        size = std::stoi(sizeStr);
    } catch (...) {
        outMsg = "Error: fdisk -size debe ser numérico.";
        return false;
    }

    return DiskManager::fdiskCreate(size, unitStr, path, typeStr, fitStr, name, outMsg);
}

bool DiskManager::fdiskCreate(int32_t size,
                              const std::string& unitStr,
                              const std::string& path,
                              const std::string& typeStr,
                              const std::string& fitStr,
                              const std::string& name,
                              std::string& outMsg) {
    return cmd::fdiskCreate(size, unitStr, path, typeStr, fitStr, name, outMsg);
}

bool DiskManager::mountPartition(const std::string& path,
                                 const std::string& name,
                                 std::string& outMsg) {
    return cmd::mountPartition(path, name, outMsg);
}

bool DiskManager::mounted(std::string& outMsg) {
    return cmd::mounted(outMsg);
}

bool DiskManager::mkfs(const std::string& id,
                       const std::string& typeStr,
                       std::string& outMsg) {
    return cmd::mkfs(id, typeStr, outMsg);
}






