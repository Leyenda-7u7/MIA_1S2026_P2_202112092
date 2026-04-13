#include "reports/bm_inode_report.hpp"
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

bool buildBmInodeText(const std::string& diskPath, int32_t partStart, std::string& textOut, std::string& err) {
    // 1) leer superblock
    Superblock sb{};
    if (!readAt(diskPath, partStart, &sb, sizeof(Superblock), err)) return false;

    if (sb.s_magic != 0xEF53) {
        err = "Error: la partición no parece EXT2 (magic inválido).";
        return false;
    }

    const int32_t n = sb.s_inodes_count;
    if (n <= 0) {
        textOut.clear();
        return true;
    }

    // 2) leer bitmap completo (bytes '0'/'1' según tu implementación)
    std::ifstream file(diskPath, std::ios::binary);
    if (!file.is_open()) {
        err = "Error: no se pudo abrir el disco: " + diskPath;
        return false;
    }

    file.seekg(sb.s_bm_inode_start, std::ios::beg);

    std::ostringstream out;
    for (int32_t i = 0; i < n; i++) {
        char bit = '0';
        file.read(&bit, 1);
        if (!file) {
            err = "Error: no se pudo leer el bitmap de inodos.";
            return false;
        }

        // Normaliza por si en algún momento guardas 0/1 binario
        if (bit != '0' && bit != '1') bit = (bit == 0) ? '0' : '1';

        out << bit;

        // espacio entre bits (como imagen)
        if ((i + 1) % 20 != 0) out << ' ';

        // salto cada 20
        if ((i + 1) % 20 == 0) out << "\n";
    }

    // si no cae exacto, termina con \n opcional (no obligatorio)
    textOut = out.str();
    return true;
}

} // namespace reports