#include "commands/exec.hpp"

#include <fstream>
#include <filesystem>
#include <string>

#include "CommandParser.hpp" // executeLine(...)

namespace cmd {

bool execScript(const std::string& path, std::string& outMsg) {
    if (path.empty()) {
        outMsg = "Error: exec requiere -path.";
        return false;
    }

    if (!std::filesystem::exists(path)) {
        outMsg = "Error: no existe el archivo de script: " + path;
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        outMsg = "Error: no se pudo abrir el script: " + path;
        return false;
    }

    std::string line;
    std::string result;

    while (std::getline(file, line)) {
        // Preservar líneas en blanco
        if (line.empty()) {
            result += "\n";
            continue;
        }

        // Comentarios: se muestran tal cual
        if (!line.empty() && line[0] == '#') {
            result += line + "\n";
            continue;
        }

        // Ejecutar comando normal
        std::string out = executeLine(line);

        if (out == "EXIT") {
            // Si en script ponen exit, se corta la ejecución del script.
            result += "EXIT\n";
            break;
        }

        if (!out.empty()) result += out;
        result += "\n";
    }

    outMsg = result;
    return true;
}

} // namespace cmd