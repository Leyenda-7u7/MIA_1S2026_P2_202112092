#include "commands/mkdisk.hpp"
#include "Structures.hpp"

#include <fstream>
#include <filesystem>
#include <cstring>
#include <random>
#include <ctime>

static int32_t randomSignature() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int32_t> dist(1, 2000000000);
    return dist(gen);
}

static char fitToDiskFitChar(const std::string& fitStr) {
    // enunciado: BF, FF, WF  -> disco: B, F, W
    if (fitStr.empty()) return 'F'; // default First Fit

    std::string f = fitStr;
    for (auto &c : f) c = (char)std::tolower((unsigned char)c);

    if (f == "bf") return 'B';
    if (f == "ff") return 'F';
    if (f == "wf") return 'W';
    return '\0';
}

static long long sizeToBytes(int32_t size, const std::string& unitStr) {
    if (size <= 0) return -1;

    // unit default: M
    std::string u = unitStr;
    if (u.empty()) u = "m";

    for (auto &c : u) c = (char)std::tolower((unsigned char)c);

    if (u == "k") return (long long)size * 1024LL;
    if (u == "m") return (long long)size * 1024LL * 1024LL;

    return -1;
}

static void initEmptyMBR(MBR& mbr, int32_t diskBytes, char diskFit) {
    mbr.mbr_tamano = diskBytes;
    mbr.mbr_fecha_creacion = std::time(nullptr);
    mbr.mbr_dsk_signature = randomSignature();
    mbr.dsk_fit = diskFit;

    for (int i = 0; i < 4; i++) {
        auto &p = mbr.mbr_partitions[i];
        p.part_status = '0';
        p.part_type = 'P';
        p.part_fit = diskFit;
        p.part_start = -1;
        p.part_s = 0;
        std::memset(p.part_name, 0, sizeof(p.part_name));
        p.part_correlative = -1;
        std::memset(p.part_id, 0, sizeof(p.part_id));
    }
}

namespace cmd {

bool mkdisk(int32_t size,
            const std::string& unitStr,
            const std::string& fitStr,
            const std::string& path,
            std::string& outMsg) {

    if (path.empty()) {
        outMsg = "Error: mkdisk requiere -path.";
        return false;
    }

    long long bytes = sizeToBytes(size, unitStr);
    if (bytes <= 0) {
        outMsg = "Error: tamaño inválido o unit inválida (use K o M).";
        return false;
    }

    char diskFit = fitToDiskFitChar(fitStr);
    if (diskFit == '\0') {
        outMsg = "Error: fit inválido (use BF, FF o WF).";
        return false;
    }

    // Ruta absoluta real (para debug y cero confusión)
    std::filesystem::path p(path);
    std::filesystem::path abs = std::filesystem::absolute(p);

    // Crear directorios padre si existen
    try {
        if (abs.has_parent_path()) {
            std::filesystem::create_directories(abs.parent_path());
        }
    } catch (...) {
        outMsg = "Error: no se pudieron crear directorios para: " + abs.string();
        return false;
    }

    // Crear archivo y llenarlo de ceros
    std::ofstream file(abs, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        outMsg = "Error: no se pudo crear/abrir el disco: " + abs.string();
        return false;
    }

    char buffer[1024];
    std::memset(buffer, 0, sizeof(buffer));

    long long remaining = bytes;
    while (remaining > 0) {
        long long chunk = (remaining >= (long long)sizeof(buffer)) ? (long long)sizeof(buffer) : remaining;
        file.write(buffer, (std::streamsize)chunk);
        if (!file) {
            file.close();
            outMsg = "Error: fallo escribiendo ceros en el disco (permiso/espacio).";
            return false;
        }
        remaining -= chunk;
    }
    file.flush();

    // Escribir MBR al inicio
    if (bytes > INT32_MAX) {
        file.close();
        outMsg = "Error: el tamaño excede el límite soportado por int32_t.";
        return false;
    }

    MBR mbr{};
    initEmptyMBR(mbr, (int32_t)bytes, diskFit);

    file.seekp(0, std::ios::beg);
    file.write(reinterpret_cast<const char*>(&mbr), sizeof(MBR));
    if (!file) {
        file.close();
        outMsg = "Error: no se pudo escribir el MBR al inicio del disco.";
        return false;
    }

    file.flush();
    file.close();

    // Validación final (CLAVE)
    try {
        if (!std::filesystem::exists(abs)) {
            outMsg = "Error: mkdisk terminó pero el archivo NO existe: " + abs.string();
            return false;
        }
        auto realSize = (long long)std::filesystem::file_size(abs);
        if (realSize != bytes) {
            outMsg = "Error: tamaño real del archivo no coincide. Esperado=" + std::to_string(bytes) +
                     " real=" + std::to_string(realSize) + " path=" + abs.string();
            return false;
        }
    } catch (...) {
        outMsg = "Error: no se pudo verificar existencia/tamaño del disco: " + abs.string();
        return false;
    }

    outMsg = "Disco creado correctamente: " + abs.string() + " (" + std::to_string(bytes) + " bytes)";
    return true;
}

} // namespace cmd