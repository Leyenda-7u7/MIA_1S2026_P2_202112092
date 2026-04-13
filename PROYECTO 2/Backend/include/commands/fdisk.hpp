#pragma once
#include <string>
#include <cstdint>

namespace cmd {
    bool fdiskCreate(
        int32_t size,
        const std::string& unitStr,
        const std::string& path,
        const std::string& typeStr,
        const std::string& fitStr,
        const std::string& name,
        std::string& outMsg
    );
}