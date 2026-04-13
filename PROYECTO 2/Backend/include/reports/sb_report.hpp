#pragma once
#include <string>
#include <cstdint>

namespace reports {

bool buildSbDot(
    const std::string& diskPath,
    int32_t partStart,
    std::string& outDot,
    std::string& err
);

}