#pragma once
#include <string>

namespace cmd {
    bool execScript(
        const std::string& path, 
        std::string& outMsg
    );
}