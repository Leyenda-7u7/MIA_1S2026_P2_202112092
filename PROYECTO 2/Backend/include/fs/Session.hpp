#pragma once
#include <string>
#include <cstdint>

namespace session {

struct SessionInfo {
    bool active = false;
    std::string user;     // ej: "root"
    int32_t uid = -1;     // ej: 1
    int32_t gid = -1;     // ej: 1
    std::string mountedId; // id de partición montada donde trabaja (ej: "921A")
};

bool isActive();
SessionInfo get();
void start(const SessionInfo& s);
void end();
}