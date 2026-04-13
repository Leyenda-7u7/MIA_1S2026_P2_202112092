#pragma once
#include <string>

namespace reports {

bool buildBmInodeText(
    const std::string& diskPath, 
    int32_t partStart, 
    std::string& textOut, 
    std::string& err
);

}