#pragma once
#include <string>
#include <vector>

namespace cmd {
    // files: lista de rutas absolutas tipo /users.txt
    bool cat(
        const std::vector<std::string>& files, 
        std::string& outMsg
    );
}