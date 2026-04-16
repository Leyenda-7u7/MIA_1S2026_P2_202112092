#pragma once
#include <string>

namespace cmd {
    bool copy(
        const std::string& path,
        const std::string& destino,
        std::string& outMsg
    );
}