#pragma once
#include <string>

namespace reports {

bool buildMbrDot(
    const std::string& diskPath, 
    std::string& dotOut, 
    std::string& err 
);

}