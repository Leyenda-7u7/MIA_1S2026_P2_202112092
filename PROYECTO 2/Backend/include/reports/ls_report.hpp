#pragma once
#include <string>
#include <cstdint>

namespace reports {

bool buildLsDot(
    const std::string& diskPath,
    int32_t partStart,
    const std::string& absPath,
    std::string& outDot,
    std::string& err
);

}