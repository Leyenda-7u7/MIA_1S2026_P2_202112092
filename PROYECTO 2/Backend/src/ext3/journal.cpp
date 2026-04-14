#include "ext3/journal.hpp"

#include <fstream>
#include <cstring>
#include <ctime>

namespace ext3 {

bool writeJournal(
    const std::string& diskPath,
    int32_t journalingStart,
    int32_t index,
    const std::string& operation,
    const std::string& path,
    const std::string& content
) {
    std::fstream file(diskPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) return false;

    Journal j{};
    j.j_count = index + 1;

    std::memset(j.j_content.i_operation, 0, sizeof(j.j_content.i_operation));
    std::memset(j.j_content.i_path, 0, sizeof(j.j_content.i_path));
    std::memset(j.j_content.i_content, 0, sizeof(j.j_content.i_content));

    std::strncpy(j.j_content.i_operation, operation.c_str(), sizeof(j.j_content.i_operation) - 1);
    std::strncpy(j.j_content.i_path, path.c_str(), sizeof(j.j_content.i_path) - 1);
    std::strncpy(j.j_content.i_content, content.c_str(), sizeof(j.j_content.i_content) - 1);

    j.j_content.i_date = static_cast<float>(std::time(nullptr));

    int32_t pos = journalingStart + index * sizeof(Journal);

    file.seekp(pos);
    file.write(reinterpret_cast<char*>(&j), sizeof(Journal));

    return file.good();
}

}