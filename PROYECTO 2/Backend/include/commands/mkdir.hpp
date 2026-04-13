#pragma once
#include <string>

namespace cmd {
    bool mkdir(
        const std::string& path, 
        bool parents, 
        std::string& outMsg
    );
}