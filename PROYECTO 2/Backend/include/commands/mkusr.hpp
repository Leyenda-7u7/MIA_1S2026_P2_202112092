#pragma once
#include <string>

namespace cmd {
    bool mkusr(const std::string& user,
               const std::string& pass,
               const std::string& grp,
               std::string& outMsg
            );
}