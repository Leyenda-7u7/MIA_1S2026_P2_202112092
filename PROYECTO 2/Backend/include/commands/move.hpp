#pragma once
#include <string>

namespace cmd {
    bool move(
        const std::string& path,
        const std::string& destino,
        std::string& outMsg
    );
}