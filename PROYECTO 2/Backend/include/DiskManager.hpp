#pragma once
#include <string>
#include <unordered_map>

class DiskManager {
public:
    // Comandos
    static bool mkdisk(int32_t size,
                       const std::string& unitStr,
                       const std::string& fitStr,
                       const std::string& path,
                       std::string& outMsg);

    static bool rmdisk(const std::string& path,
                       std::string& outMsg);

    // fdisk "alto nivel" (el que tu CommandParser está intentando usar)
    static bool fdisk(const std::unordered_map<std::string, std::string>& params,
                      std::string& outMsg);

    // fdisk "create" (tu implementación actual)
    static bool fdiskCreate(int32_t size,
                            const std::string& unitStr,
                            const std::string& path,
                            const std::string& typeStr,
                            const std::string& fitStr,
                            const std::string& name,
                            std::string& outMsg);

    static bool mountPartition(const std::string& path,
                               const std::string& name,
                               std::string& outMsg);

    static bool mounted(std::string& outMsg);

    // MKFS (ya lo estás compilando: mkfs.cpp está en CMakeLists)
    static bool mkfs(const std::string& id,
                     const std::string& typeStr,
                     std::string& outMsg);
};
