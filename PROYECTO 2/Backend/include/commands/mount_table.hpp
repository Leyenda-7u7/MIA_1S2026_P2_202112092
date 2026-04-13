#pragma once
#include <string>
#include <vector>

struct MountedEntry {
    std::string diskPath;
    std::string partName;
    char letter;
    int number;
    std::string id;
};

namespace mount_table {
    const std::vector<MountedEntry>& list();
    void add(const MountedEntry& e);
    bool exists(const std::string& diskPath, const std::string& partName, std::string& outId);
    char getOrAssignLetter(const std::string& diskPath);
    int nextNumber(const std::string& diskPath); // incrementa y devuelve
}