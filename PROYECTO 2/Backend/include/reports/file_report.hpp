#pragma once
#include <string>
#include <cstdint>

namespace reports {

bool buildFileText(
    const std::string& diskPath,
    int32_t partStart,
    const std::string& absPathInFs,
    std::string& outText,
    std::string& err
);

}