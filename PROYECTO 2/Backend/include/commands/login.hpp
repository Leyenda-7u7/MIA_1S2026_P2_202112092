#pragma once
#include <string>
#include <cstdint>

namespace cmd {
    bool login(
        const std::string& user, 
        const std::string& pass, 
        const std::string& id, 
        std::string& outMsg
    );

    bool logout(
        std::string& outMsg
    );

    bool hasActiveSession();

    // NUEVO: para que CAT y demás comandos sepan dónde trabajar
    bool getSessionPartition(
        std::string& diskPath, 
        int32_t& start, 
        int32_t& size, 
        std::string& outMsg
    );

    // (opcional, pero recomendado para permisos)
    int32_t sessionUid();
    int32_t sessionGid();
}