#pragma once
#include <string>

namespace cmd {
    // mkfile -path="..." [-r] [-size=N] [-cont="/ruta/host.txt"]
    bool mkfile(const std::string& path,
                bool recursive,
                int32_t size,
                const std::string& contHostPath,
                std::string& outMsg
            );
}