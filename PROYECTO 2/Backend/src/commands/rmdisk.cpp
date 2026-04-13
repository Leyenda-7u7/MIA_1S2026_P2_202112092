#include "commands/rmdisk.hpp"
#include <filesystem>

namespace cmd {
bool rmdisk(const std::string& path, std::string& outMsg) {
    if (path.empty()) {
        outMsg = "Error: rmdisk requiere el parámetro obligatorio -path.";
        return false;
    }

    try {
        std::filesystem::path p(path);

        if (!std::filesystem::exists(p)) {
            outMsg = "Error: el archivo no existe: " + path;
            return false;
        }
        if (!std::filesystem::is_regular_file(p)) {
            outMsg = "Error: la ruta no es un archivo válido: " + path;
            return false;
        }

        std::filesystem::remove(p);
        outMsg = "Disco eliminado correctamente: " + path;
        return true;

    } catch (const std::exception& e) {
        outMsg = std::string("Error eliminando el disco: ") + e.what();
        return false;
    }
}
}