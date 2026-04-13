#pragma once
#include <cstdint>
#include <string>

class Bitmap {
public:
    // Escribe count entradas en 0 (libres) a partir de start
    static bool initZeros(const std::string& diskPath, int32_t start, int32_t count, std::string& error);

    // Marca una entrada del bitmap como ocupada (valor '1')
    // index: 0..count-1
    static bool setOne(const std::string& diskPath, int32_t start, int32_t index, std::string& error);

    // Marca una entrada como libre (valor '0')
    static bool setZero(const std::string& diskPath, int32_t start, int32_t index, std::string& error);
};