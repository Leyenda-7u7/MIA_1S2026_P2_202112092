#pragma once
#include <string>

namespace cmd {
    bool remove(
        const std::string& path,
        std::string& outMsg
    );
}