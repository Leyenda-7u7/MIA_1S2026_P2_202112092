#pragma once
#include <string>
#include <cstdint>

namespace cmd {
    bool mountPartition(
        const std::string& path, 
        const std::string& name, 
        std::string& outMsg
    );
    bool mounted(
        std::string& outMsg
    );

    // para mkfs
    bool getMountedById(
        const std::string& id, 
        std::string& diskPath, 
        int32_t& start, 
        int32_t& size, 
        std::string& outMsg
    );
}