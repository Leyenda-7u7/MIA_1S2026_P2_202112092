#pragma once
#include <string>

namespace cmd {
    bool chmod(
        const std::string& path,
        bool recursive,
        const std::string& ugo,
        std::string& outMsg
    );
}