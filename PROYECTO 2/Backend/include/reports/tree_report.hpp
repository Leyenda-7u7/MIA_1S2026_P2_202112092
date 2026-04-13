#pragma once
#include <string>
#include <cstdint>

namespace reports {

bool buildTreeDot(
    const std::string& diskPath,
    int32_t partStart,
    std::string& outDot,
    std::string& err
);

}