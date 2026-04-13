#pragma once
#include <string>
#include <cstdint>

namespace cmd {
    bool mkdisk(
        int32_t size, 
        const std::string& unitStr, 
        const std::string& fitStr,
        const std::string& path, 
        std::string& outMsg
    );
}