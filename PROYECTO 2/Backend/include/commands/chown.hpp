#pragma once
#include <string>

namespace cmd {
    bool chown(
        const std::string& path,
        bool recursive,
        const std::string& usuario,
        std::string& outMsg
    );
}