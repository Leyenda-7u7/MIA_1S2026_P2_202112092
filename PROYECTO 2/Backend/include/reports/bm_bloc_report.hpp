#pragma once
#include <string>
#include <cstdint>

namespace reports {

bool buildBmBlockText(
    const std::string& diskPath,
    int32_t partStart,
    std::string& outText,
    std::string& err
);

}