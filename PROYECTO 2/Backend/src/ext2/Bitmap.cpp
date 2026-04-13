#include "ext2/Bitmap.hpp"
#include <fstream>
#include <cstring>

static bool writeValue(const std::string& diskPath, int32_t pos, char value, std::string& error) {
    std::fstream file(diskPath, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        error = "No se pudo abrir el disco: " + diskPath;
        return false;
    }
    file.seekp(pos, std::ios::beg);
    file.write(&value, 1);
    if (!file) {
        error = "Error escribiendo bitmap en posición: " + std::to_string(pos);
        file.close();
        return false;
    }
    file.close();
    return true;
}

bool Bitmap::initZeros(const std::string& diskPath, int32_t start, int32_t count, std::string& error) {
    if (count <= 0) return true;

    std::fstream file(diskPath, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        error = "No se pudo abrir el disco: " + diskPath;
        return false;
    }

    file.seekp(start, std::ios::beg);

    // buffer grande para velocidad
    char buffer[1024];
    std::memset(buffer, '0', sizeof(buffer)); // usamos '0' y '1' para visualizar fácil

    int32_t remaining = count;
    while (remaining > 0) {
        int32_t chunk = remaining > (int32_t)sizeof(buffer) ? (int32_t)sizeof(buffer) : remaining;
        file.write(buffer, chunk);
        if (!file) {
            error = "Error inicializando bitmap.";
            file.close();
            return false;
        }
        remaining -= chunk;
    }

    file.close();
    return true;
}

bool Bitmap::setOne(const std::string& diskPath, int32_t start, int32_t index, std::string& error) {
    if (index < 0) {
        error = "Índice inválido en bitmap.";
        return false;
    }
    return writeValue(diskPath, start + index, '1', error);
}

bool Bitmap::setZero(const std::string& diskPath, int32_t start, int32_t index, std::string& error) {
    if (index < 0) {
        error = "Índice inválido en bitmap.";
        return false;
    }
    return writeValue(diskPath, start + index, '0', error);
}