#pragma once
#include <string>

namespace cmd {
    bool chgrp(
        const std::string& user, 
        const std::string& grp, 
        std::string& outMsg
    );
}