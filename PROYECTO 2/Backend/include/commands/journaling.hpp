#pragma once
#include <string>

namespace cmd {
    bool journaling(
        const std::string& id,
        std::string& outMsg
    );
}