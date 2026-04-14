#pragma once
#include <string>

namespace cmd {
    bool unmount(
        const std::string& id,
        std::string& outMsg
    );
}