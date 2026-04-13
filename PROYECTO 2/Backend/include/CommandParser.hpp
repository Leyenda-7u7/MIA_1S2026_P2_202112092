#pragma once
#include <string>
#include <vector>
#include <unordered_map>

struct ParsedCommand {
    std::string name;
    std::unordered_map<std::string, std::string> params;
    std::vector<std::string> unknown;
};

std::vector<std::string> tokenizeRespectQuotes(const std::string& line);

ParsedCommand parseCommand(
    const std::string& line,
    const std::vector<std::string>& allowedParamsLower
);

std::string executeLine(
    const std::string& line

);
