#pragma once
#include <string>
#include <unordered_map>

class DiskManager {
public:

    // MKDISK
    static bool mkdisk(int32_t size,
                       const std::string& unitStr,
                       const std::string& fitStr,
                       const std::string& path,
                       std::string& outMsg);

    // RMDISK
    static bool rmdisk(const std::string& path,
                       std::string& outMsg);

    //FDISK
    static bool fdisk(const std::unordered_map<std::string, std::string>& params,
                      std::string& outMsg);

    //FDISK CREATE
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

    static bool mkfs(const std::string& id, 
        const std::string& typeStr, 
        const std::string& fsStr, 
        std::string& outMsg);
};
