#pragma once
#include <string>

namespace reports {

bool buildDiskDot(
    const std::string& diskPath, 
    std::string& dotOut, 
    std::string& err
);

}