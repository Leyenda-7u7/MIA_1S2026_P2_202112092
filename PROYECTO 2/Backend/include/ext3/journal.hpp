#pragma once
#include <string>
#include "Structures.hpp"

namespace ext3 {
    bool writeJournal(
        const std::string& diskPath,
        int32_t journalingStart,
        int32_t index,
        const std::string& operation,
        const std::string& path,
        const std::string& content
    );
}