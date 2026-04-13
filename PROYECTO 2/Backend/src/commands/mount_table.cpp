#include "commands/mount_table.hpp"
#include <unordered_map>
#include <algorithm>

static std::vector<MountedEntry> g_mounts;
static std::unordered_map<std::string, char> g_diskLetter;
static std::unordered_map<std::string, int>  g_diskCounter;

namespace mount_table {

const std::vector<MountedEntry>& list() {
    return g_mounts;
}

void add(const MountedEntry& e) {
    g_mounts.push_back(e);
}

bool exists(const std::string& diskPath, const std::string& partName, std::string& outId) {
    for (auto& m : g_mounts) {
        if (m.diskPath == diskPath && m.partName == partName) {
            outId = m.id;
            return true;
        }
    }
    return false;
}

static char nextLetter() {
    char maxL = 'A' - 1;
    for (auto& kv : g_diskLetter) maxL = std::max(maxL, kv.second);
    return (maxL < 'A') ? 'A' : (char)(maxL + 1);
}

char getOrAssignLetter(const std::string& diskPath) {
    auto it = g_diskLetter.find(diskPath);
    if (it == g_diskLetter.end()) {
        char letter = nextLetter();
        g_diskLetter[diskPath] = letter;
        g_diskCounter[diskPath] = 0;
        return letter;
    }
    return it->second;
}

int nextNumber(const std::string& diskPath) {
    int n = g_diskCounter[diskPath] + 1;
    g_diskCounter[diskPath] = n;
    return n;
}

}