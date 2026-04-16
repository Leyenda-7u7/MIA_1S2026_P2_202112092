#pragma once
#include <string>
#include <cstdint>
#include "Structures.hpp"

namespace ext3 {
    bool writeJournal(
        const std::string& diskPath,
        int32_t journalingStart,
        int32_t indexHint,
        const std::string& operation,
        const std::string& path,
        const std::string& content
    );

    bool findNextJournalIndex(
        const std::string& diskPath,
        int32_t journalingStart,
        int32_t maxEntries,
        int32_t& outIndex
    );
}