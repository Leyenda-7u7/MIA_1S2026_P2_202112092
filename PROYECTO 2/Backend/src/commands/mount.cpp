#include "commands/mount.hpp"
#include "Structures.hpp"

#include <fstream>
#include <filesystem>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <algorithm>

// ====== CONFIG ======
static const std::string CARNET_SUFFIX = "92"; // 202112092 -> "92"

// ====== RAM TABLE ======
struct MountedEntry {
    std::string diskPath;
    std::string partName;
    char letter;      // A, B, C...
    int number;       // 1,2,3...
    std::string id;   // e.g. "921A"
    int32_t start;    // byte inicio (para mkfs)
    int32_t size;     // bytes (para mkfs)
    char type;        // 'P' por ahora
};

// Lista global en RAM
static std::vector<MountedEntry> g_mounts;

// Mapa disco->letra asignada
static std::unordered_map<std::string, char> g_diskLetter;

// Contador por disco (para número correlativo)
static std::unordered_map<std::string, int> g_diskCounter;

// ====== helpers ======
static bool readMBR(const std::string& path, MBR& mbr, std::string& err) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        err = "Error: no se pudo abrir el disco: " + path;
        return false;
    }
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(&mbr), sizeof(MBR));
    if (!file) {
        err = "Error: no se pudo leer el MBR del disco.";
        return false;
    }
    return true;
}

static bool writeMBR(const std::string& path, const MBR& mbr, std::string& err) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) {
        err = "Error: no se pudo abrir el disco para escribir: " + path;
        return false;
    }
    file.seekp(0, std::ios::beg);
    file.write(reinterpret_cast<const char*>(&mbr), sizeof(MBR));
    if (!file) {
        err = "Error: no se pudo escribir el MBR en el disco.";
        return false;
    }
    file.flush();
    return true;
}

static std::string cleanName16(const char name16[16]) {
    // corta en el primer '\0'
    size_t len = 0;
    while (len < 16 && name16[len] != '\0') len++;
    return std::string(name16, name16 + len);
}

static bool sameMountExists(const std::string& disk, const std::string& part, std::string& existingId) {
    for (auto& m : g_mounts) {
        if (m.diskPath == disk && m.partName == part) {
            existingId = m.id;
            return true;
        }
    }
    return false;
}

static char nextLetter() {
    // letras A..Z según discos descubiertos
    char maxL = char('A' - 1);
    for (auto& kv : g_diskLetter) maxL = std::max(maxL, kv.second);
    return (maxL < 'A') ? 'A' : (char)(maxL + 1);
}

static std::string makeId(int number, char letter) {
    // Formato: 92 + number + letter  (ej 921A, 922A, 921B)
    return CARNET_SUFFIX + std::to_string(number) + std::string(1, letter);
}

namespace cmd {

// -------------------------------------------------
// MOUNT
// -------------------------------------------------
bool mountPartition(const std::string& path,
                    const std::string& name,
                    std::string& outMsg) {

    if (path.empty() || name.empty()) {
        outMsg = "Error: mount requiere parámetros obligatorios -path y -name.";
        return false;
    }

    if (!std::filesystem::exists(path)) {
        outMsg = "Error: el disco no existe en la ruta indicada: " + path;
        return false;
    }

    // Si ya está montada en RAM, devolver mismo ID
    std::string existingId;
    if (sameMountExists(path, name, existingId)) {
        outMsg = "La partición ya estaba montada. ID=" + existingId;
        return true;
    }

    // Leer MBR
    MBR mbr{};
    std::string err;
    if (!readMBR(path, mbr, err)) {
        outMsg = err;
        return false;
    }

    // Buscar partición por nombre
    int idx = -1;
    for (int i = 0; i < 4; i++) {
        auto& p = mbr.mbr_partitions[i];
        if (p.part_start != -1 && p.part_s > 0) {
            if (cleanName16(p.part_name) == name) {
                idx = i;
                break;
            }
        }
    }

    if (idx == -1) {
        outMsg = "Error: no existe una partición con nombre '" + name + "' en el disco.";
        return false;
    }

    auto& part = mbr.mbr_partitions[idx];

    // Solo primarias (según Observación #3)
    if (part.part_type != 'P') {
        outMsg = "Error: solo se permite montar particiones primarias (P).";
        return false;
    }

    // Asignar letra por disco (A,B,C...)
    char letter;
    auto itL = g_diskLetter.find(path);
    if (itL == g_diskLetter.end()) {
        letter = nextLetter();
        g_diskLetter[path] = letter;
        g_diskCounter[path] = 0; // contador inicia en 0, luego +1
    } else {
        letter = itL->second;
    }

    // Incrementar número para ese disco
    int number = g_diskCounter[path] + 1;
    g_diskCounter[path] = number;

    std::string id = makeId(number, letter);

    // Actualizar partición en MBR (status, correlative, id)
    part.part_status = '1';
    part.part_correlative = number;

    // IMPORTANTE:
    // Si cambiaste part_id a char[16], aquí sí cabe bien y termina en '\0'
    std::memset(part.part_id, 0, sizeof(part.part_id));
    std::strncpy(part.part_id, id.c_str(), sizeof(part.part_id) - 1);

    // Escribir MBR actualizado al disco
    if (!writeMBR(path, mbr, err)) {
        outMsg = err;
        return false;
    }

    // Guardar en RAM (incluye start/size)
    g_mounts.push_back(MountedEntry{
        path,
        name,
        letter,
        number,
        id,
        part.part_start,
        part.part_s,
        part.part_type
    });

    outMsg = "Partición montada correctamente. ID=" + id;
    return true;
}

// -------------------------------------------------
// MOUNTED
// -------------------------------------------------
bool mounted(std::string& outMsg) {
    if (g_mounts.empty()) {
        outMsg = "No hay particiones montadas.";
        return true;
    }

    outMsg = "Particiones montadas (RAM):\n";
    for (const auto& m : g_mounts) {
        outMsg += "  " + m.id + " -> " + m.diskPath + " (" + m.partName + ")\n";
    }
    return true;
}

// -------------------------------------------------
// Para MKFS: obtener montada por ID
// -------------------------------------------------
bool getMountedById(const std::string& id, std::string& diskPath, int32_t& start, int32_t& size, std::string& outMsg) {
    for (const auto& m : g_mounts) {
        if (m.id == id) {
            diskPath = m.diskPath;
            start = m.start;
            size = m.size;
            return true;
        }
    }
    outMsg = "Error: no existe una partición montada con id=" + id;
    return false;
}

} // namespace cmd