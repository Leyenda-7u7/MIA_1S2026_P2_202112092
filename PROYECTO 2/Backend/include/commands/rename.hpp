#pragma once
#include <string>

namespace cmd {
    bool rename(
        const std::string& path,
        const std::string& newName,
        std::string& outMsg
    );
}