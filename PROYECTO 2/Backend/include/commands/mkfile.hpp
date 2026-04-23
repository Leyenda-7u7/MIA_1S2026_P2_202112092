#pragma once
#include <string>

namespace cmd {
    bool mkfile(const std::string& path,
                bool recursive,
                int32_t size,
                const std::string& contHostPath,
                std::string& outMsg
            );
}