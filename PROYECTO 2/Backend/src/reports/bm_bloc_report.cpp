#include "reports/bm_bloc_report.hpp"
#include "Structures.hpp"

#include <fstream>
#include <sstream>

static bool readAt(const std::string& path, int32_t offset, void* data, size_t sz, std::string& err) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        err = "Error: no se pudo abrir el disco: " + path;
        return false;
    }
    file.seekg(offset, std::ios::beg);
    file.read(reinterpret_cast<char*>(data), (std::streamsize)sz);
    if (!file) {
        err = "Error: no se pudo leer del disco (offset=" + std::to_string(offset) + ").";
        return false;
    }
    return true;
}

namespace reports {

bool buildBmBlockText(const std::string& diskPath,
                      int32_t partStart,
                      std::string& outText,
                      std::string& err) {

    // 1) Leer Superblock
    Superblock sb{};
    if (!readAt(diskPath, partStart, &sb, sizeof(Superblock), err)) return false;
    if (sb.s_magic != 0xEF53) {
        err = "Error: la partición no parece EXT2 (magic inválido).";
        return false;
    }

    // 2) Leer bitmap de bloques completo
    const int32_t count = sb.s_blocks_count;
    if (count <= 0) {
        outText.clear();
        return true;
    }

    std::string bits;
    bits.resize((size_t)count);

    if (!readAt(diskPath, sb.s_bm_block_start, bits.data(), (size_t)count, err)) return false;

    // 3) Formatear 20 por línea
    std::ostringstream oss;
    int line = 1;
    int col = 0;

    // (opcional) índice de línea como en la imagen
    oss << line << " ";
    for (int i = 0; i < count; i++) {
        char b = bits[(size_t)i];
        if (b != '0' && b != '1') b = '0'; // por si hubiera basura

        oss << b << " ";
        col++;

        if (col == 20 && i + 1 < count) {
            oss << "\n";
            line++;
            oss << line << " ";
            col = 0;
        }
    }

    outText = oss.str();
    return true;
}

} // namespace reports