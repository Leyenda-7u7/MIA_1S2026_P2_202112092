#pragma once
#include <string>

namespace reports {

bool buildInodeDot(
    const std::string& diskPath, 
    int32_t partStart, 
    std::string& dotOut, 
    std::string& err
);

}