#include "ext3/journal.hpp"

#include <fstream>
#include <cstring>
#include <ctime>
#include <algorithm>

namespace ext3 {

bool findNextJournalIndex(
    const std::string& diskPath,
    int32_t journalingStart,
    int32_t maxEntries,
    int32_t& outIndex
) {
    outIndex = -1;

    std::ifstream file(diskPath, std::ios::binary);
    if (!file.is_open()) return false;

    for (int32_t i = 0; i < maxEntries; i++) {
        Journal j{};
        int32_t pos = journalingStart + i * (int32_t)sizeof(Journal);

        file.seekg(pos, std::ios::beg);
        file.read(reinterpret_cast<char*>(&j), sizeof(Journal));
        if (!file) return false;

        if (j.j_content.i_date == 0) {
            outIndex = i;
            return true;
        }
    }

    return true;
}

bool writeJournal(
    const std::string& diskPath,
    int32_t journalingStart,
    int32_t indexHint,
    const std::string& operation,
    const std::string& path,
    const std::string& content
) {
    (void)indexHint;

    std::fstream file(diskPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) return false;

    int32_t realIndex = -1;

    {
        std::ifstream in(diskPath, std::ios::binary);
        if (!in.is_open()) return false;

        for (int32_t i = 0; i < 4096; i++) {
            Journal tmp{};
            int32_t pos = journalingStart + i * (int32_t)sizeof(Journal);

            in.seekg(pos, std::ios::beg);
            in.read(reinterpret_cast<char*>(&tmp), sizeof(Journal));
            if (!in) break;

            if (tmp.j_content.i_date == 0) {
                realIndex = i;
                break;
            }
        }
    }

    if (realIndex < 0) {
        return false;
    }

    Journal j{};
    j.j_count = realIndex + 1;

    std::memset(j.j_content.i_operation, 0, sizeof(j.j_content.i_operation));
    std::memset(j.j_content.i_path, 0, sizeof(j.j_content.i_path));
    std::memset(j.j_content.i_content, 0, sizeof(j.j_content.i_content));

    std::strncpy(j.j_content.i_operation, operation.c_str(), sizeof(j.j_content.i_operation) - 1);
    std::strncpy(j.j_content.i_path, path.c_str(), sizeof(j.j_content.i_path) - 1);
    std::strncpy(j.j_content.i_content, content.c_str(), sizeof(j.j_content.i_content) - 1);

    j.j_content.i_date = static_cast<float>(std::time(nullptr));

    int32_t pos = journalingStart + realIndex * (int32_t)sizeof(Journal);

    file.seekp(pos, std::ios::beg);
    file.write(reinterpret_cast<const char*>(&j), sizeof(Journal));

    return file.good();
}

} 