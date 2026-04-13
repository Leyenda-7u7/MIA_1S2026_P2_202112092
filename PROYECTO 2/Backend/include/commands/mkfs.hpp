#pragma once
#include <string>

namespace cmd {
    bool mkfs(
        const std::string& id, 
        const std::string& type, 
        std::string& outMsg
    );
}