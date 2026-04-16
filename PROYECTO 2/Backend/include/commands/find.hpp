#pragma once
#include <string>

namespace cmd {
    bool find(
        const std::string& path,
        const std::string& name,
        std::string& outMsg
    );
}