#include "commands/fdisk.hpp"
#include "Structures.hpp"

#include <fstream>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <vector>

static bool readMBR(const std::string& path, MBR& mbr, std::string& err) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) { err = "Error: no se pudo abrir el disco."; return false; }
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(&mbr), sizeof(MBR));
    if (!file) { err = "Error: no se pudo leer el MBR."; return false; }
    return true;
}

static bool writeMBR(const std::string& path, const MBR& mbr, std::string& err) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) { err = "Error: no se pudo abrir el disco para escritura."; return false; }
    file.seekp(0, std::ios::beg);
    file.write(reinterpret_cast<const char*>(&mbr), sizeof(MBR));
    if (!file) { err = "Error: no se pudo escribir el MBR."; return false; }
    file.flush();
    return true;
}

static bool readEBRAt(const std::string& path, int32_t offset, EBR& ebr, std::string& err) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) { err = "Error: no se pudo abrir el disco: " + path; return false; }
    file.seekg(offset, std::ios::beg);
    file.read(reinterpret_cast<char*>(&ebr), sizeof(EBR));
    if (!file) { err = "Error: no se pudo leer EBR en offset=" + std::to_string(offset); return false; }
    return true;
}

static bool writeEBRAt(const std::string& path, int32_t offset, const EBR& ebr, std::string& err) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) { err = "Error: no se pudo abrir el disco para escritura: " + path; return false; }
    file.seekp(offset, std::ios::beg);
    file.write(reinterpret_cast<const char*>(&ebr), sizeof(EBR));
    if (!file) { err = "Error: no se pudo escribir EBR en offset=" + std::to_string(offset); return false; }
    file.flush();
    return true;
}

static std::string cleanName16(const char name16[16]) {
    size_t len = 0;
    while (len < 16 && name16[len] != '\0') len++;
    return std::string(name16, name16 + len);
}

static long long toBytes(int32_t size, const std::string& unitStr) {
    if (size <= 0) return -1;
    std::string u = unitStr;
    if (u.empty()) u = "k";
    std::transform(u.begin(), u.end(), u.begin(), ::tolower);

    if (u == "k") return (long long)size * 1024LL;
    if (u == "m") return (long long)size * 1024LL * 1024LL;
    return -1;
}

static char normalizeType(const std::string& typeStr) {
    if (typeStr.empty()) return 'P';
    std::string t = typeStr;
    std::transform(t.begin(), t.end(), t.begin(), ::tolower);
    if (t == "p") return 'P';
    if (t == "e") return 'E';
    if (t == "l") return 'L';
    return '\0';
}

static char normalizeFit(const std::string& fitStr) {
    if (fitStr.empty()) return 'W';
    std::string f = fitStr;
    std::transform(f.begin(), f.end(), f.begin(), ::tolower);
    if (f == "bf") return 'B';
    if (f == "ff") return 'F';
    if (f == "wf") return 'W';
    return '\0';
}

static bool ebrPosInRange(int32_t pos, int32_t extStart, int32_t extSize) {
    int32_t extEnd = extStart + extSize;
    return (pos >= extStart) && (pos + (int32_t)sizeof(EBR) <= extEnd);
}

namespace cmd {

static bool createPrimaryOrExtended(int32_t bytes,
                                   const std::string& path,
                                   char type,
                                   char fit,
                                   const std::string& name,
                                   std::string& outMsg) {
    MBR mbr{};
    std::string err;
    if (!readMBR(path, mbr, err)) { outMsg = err; return false; }

    // Validar nombre duplicado en MBR
    for (int i = 0; i < 4; i++) {
        const Partition& p = mbr.mbr_partitions[i];
        if (p.part_s > 0) {
            if (cleanName16(p.part_name) == name) {
                outMsg = "Error: ya existe una partición con ese nombre.";
                return false;
            }
        }
    }

    // Validar solo una extendida
    if (type == 'E') {
        for (int i = 0; i < 4; i++) {
            const Partition& p = mbr.mbr_partitions[i];
            if (p.part_s > 0 && p.part_type == 'E') {
                outMsg = "Error: ya existe una partición extendida.";
                return false;
            }
        }
    }

    // Buscar slot libre en MBR
    int freeIndex = -1;
    for (int i = 0; i < 4; i++) {
        if (mbr.mbr_partitions[i].part_s == 0) { freeIndex = i; break; }
    }
    if (freeIndex == -1) {
        outMsg = "Error: no hay espacio para más particiones.";
        return false;
    }

    // Calcular inicio por huecos (best-fit simple por orden)
    int32_t start = (int32_t)sizeof(MBR);
    std::vector<std::pair<int32_t,int32_t>> used;
    for (int i = 0; i < 4; i++) {
        auto& p = mbr.mbr_partitions[i];
        if (p.part_s > 0) used.push_back({p.part_start, p.part_s});
    }
    std::sort(used.begin(), used.end());
    for (auto& seg : used) {
        if (start + bytes <= seg.first) break;
        start = seg.first + seg.second;
    }

    if ((int64_t)start + (int64_t)bytes > (int64_t)mbr.mbr_tamano) {
        outMsg = "Error: no hay espacio suficiente en el disco.";
        return false;
    }

    // Crear partición en MBR
    auto& part = mbr.mbr_partitions[freeIndex];
    part.part_status = '0';
    part.part_type = type;     // P o E
    part.part_fit = fit;
    part.part_start = start;
    part.part_s = bytes;
    part.part_correlative = -1;
    std::memset(part.part_name, 0, sizeof(part.part_name));
    std::strncpy(part.part_name, name.c_str(), sizeof(part.part_name)-1);
    std::memset(part.part_id, 0, sizeof(part.part_id));

    // Escribir MBR
    if (!writeMBR(path, mbr, err)) { outMsg = err; return false; }

    // Si es extendida, inicializar EBR vacío al inicio de la extendida
    if (type == 'E') {
        EBR e{};
        e.part_mount = '0';
        e.part_fit   = fit;
        e.part_start = start;      // en este diseño: el start del EBR = donde está el EBR
        e.part_s     = 0;          // vacío (sin lógica aún)
        e.part_next  = -1;
        std::memset(e.part_name, 0, sizeof(e.part_name));

        if (!writeEBRAt(path, start, e, err)) {
            outMsg = "Error: no se pudo inicializar EBR de extendida. " + err;
            return false;
        }
    }

    outMsg = "Partición creada correctamente: " + name;
    return true;
}

static bool createLogical(int32_t bytes,
                          const std::string& path,
                          char fit,
                          const std::string& name,
                          std::string& outMsg) {
    MBR mbr{};
    std::string err;
    if (!readMBR(path, mbr, err)) { outMsg = err; return false; }

    // Encontrar la extendida
    int extIdx = -1;
    for (int i = 0; i < 4; i++) {
        const Partition& p = mbr.mbr_partitions[i];
        if (p.part_s > 0 && p.part_type == 'E') { extIdx = i; break; }
    }
    if (extIdx == -1) {
        outMsg = "Error: no existe partición extendida para crear lógicas.";
        return false;
    }

    const Partition& ext = mbr.mbr_partitions[extIdx];
    const int32_t extStart = ext.part_start;
    const int32_t extSize  = ext.part_s;
    const int32_t extEnd   = extStart + extSize;

    // Validación: EBR inicial debe existir y caber
    if (!ebrPosInRange(extStart, extStart, extSize)) {
        outMsg = "Error: extendida inválida (EBR inicial fuera de rango).";
        return false;
    }

    // Recorrer EBRs
    int32_t ebrPos = extStart;
    int safeguard = 0;

    EBR cur{};
    if (!readEBRAt(path, ebrPos, cur, err)) { outMsg = err; return false; }

    // Validar duplicado en lógicas (y también permitir si aún está vacío)
    {
        int32_t scanPos = extStart;
        int scanSafe = 0;
        while (scanPos != -1) {
            if (!ebrPosInRange(scanPos, extStart, extSize)) break;

            EBR e{};
            if (!readEBRAt(path, scanPos, e, err)) break;

            std::string ename = cleanName16(e.part_name);
            if (e.part_s > 0 && !ename.empty() && ename == name) {
                outMsg = "Error: ya existe una partición lógica con ese nombre.";
                return false;
            }

            if (e.part_next == -1) break;
            scanPos = e.part_next;
            if (++scanSafe > 1000) break;
        }
    }

    // Caso 1: primera EBR vacía => primera lógica se escribe aquí mismo
    if (cur.part_s == 0 && cur.part_next == -1 && cleanName16(cur.part_name).empty()) {
        // Debe caber: EBR + data dentro de la extendida
        int64_t needEnd = (int64_t)extStart + (int64_t)sizeof(EBR) + (int64_t)bytes;
        if (needEnd > (int64_t)extEnd) {
            outMsg = "Error: no hay espacio suficiente dentro de la extendida.";
            return false;
        }

        cur.part_mount = '0';
        cur.part_fit   = fit;
        cur.part_start = extStart;
        cur.part_s     = bytes;
        cur.part_next  = -1;
        std::memset(cur.part_name, 0, sizeof(cur.part_name));
        std::strncpy(cur.part_name, name.c_str(), sizeof(cur.part_name)-1);

        if (!writeEBRAt(path, extStart, cur, err)) { outMsg = err; return false; }

        outMsg = "Partición creada correctamente: " + name;
        return true;
    }

    // Caso 2: ya hay al menos una lógica => insertar al final
    int32_t lastPos = extStart;
    EBR last{};
    while (true) {
        if (!ebrPosInRange(lastPos, extStart, extSize)) {
            outMsg = "Error: cadena EBR corrupta (posición fuera de rango).";
            return false;
        }

        if (!readEBRAt(path, lastPos, last, err)) { outMsg = err; return false; }

        if (last.part_next == -1) break;

        if (!ebrPosInRange(last.part_next, extStart, extSize)) {
            outMsg = "Error: cadena EBR corrupta (next fuera de rango).";
            return false;
        }

        lastPos = last.part_next;
        if (++safeguard > 1000) { outMsg = "Error: demasiados EBRs (posible loop)."; return false; }
    }

    // Nuevo EBR va después del “EBR + data” del último
    int32_t newPos = last.part_start + (int32_t)sizeof(EBR) + last.part_s;

    // Debe caber: newEBR + data
    int64_t endNeed = (int64_t)newPos + (int64_t)sizeof(EBR) + (int64_t)bytes;
    if (endNeed > (int64_t)extEnd) {
        outMsg = "Error: no hay espacio suficiente dentro de la extendida.";
        return false;
    }

    // Actualizar last.next
    last.part_next = newPos;
    if (!writeEBRAt(path, last.part_start, last, err)) { outMsg = err; return false; }

    // Escribir nuevo EBR
    EBR neu{};
    neu.part_mount = '0';
    neu.part_fit   = fit;
    neu.part_start = newPos;
    neu.part_s     = bytes;
    neu.part_next  = -1;
    std::memset(neu.part_name, 0, sizeof(neu.part_name));
    std::strncpy(neu.part_name, name.c_str(), sizeof(neu.part_name)-1);

    if (!writeEBRAt(path, newPos, neu, err)) { outMsg = err; return false; }

    outMsg = "Partición creada correctamente: " + name;
    return true;
}

bool fdiskCreate(int32_t size,
                 const std::string& unitStr,
                 const std::string& path,
                 const std::string& typeStr,
                 const std::string& fitStr,
                 const std::string& name,
                 std::string& outMsg) {

    if (size <= 0 || path.empty() || name.empty()) {
        outMsg = "Error: parámetros inválidos para fdisk.";
        return false;
    }

    if (!std::filesystem::exists(path)) {
        outMsg = "Error: el disco no existe.";
        return false;
    }

    long long bytesLL = toBytes(size, unitStr);
    if (bytesLL <= 0) { outMsg = "Error: tamaño inválido."; return false; }

    char type = normalizeType(typeStr);
    if (type == '\0') { outMsg = "Error: tipo inválido (use P, E o L)."; return false; }

    char fit = normalizeFit(fitStr);
    if (fit == '\0') { outMsg = "Error: fit inválido (BF, FF, WF)."; return false; }

    int32_t bytes = (int32_t)bytesLL;

    // ✅ LÓGICA: va a EBR, NO al MBR
    if (type == 'L') {
        return createLogical(bytes, path, fit, name, outMsg);
    }

    // ✅ PRIMARIA / EXTENDIDA: van al MBR
    return createPrimaryOrExtended(bytes, path, type, fit, name, outMsg);
}

} // namespace cmd