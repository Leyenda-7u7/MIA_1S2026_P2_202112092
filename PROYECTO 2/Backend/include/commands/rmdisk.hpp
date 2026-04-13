#pragma once
#include <string>

namespace cmd {
    bool rmdisk(
        const std::string& path, 
        std::string& outMsg
    );
}