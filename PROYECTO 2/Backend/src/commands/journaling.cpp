#include "commands/journaling.hpp"
#include "commands/mount.hpp"
#include "Structures.hpp"

#include <fstream>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <vector>

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

static std::string cleanFixedString(const char* data, size_t size) {
    size_t len = 0;
    while (len < size && data[len] != '\0') len++;
    return std::string(data, data + len);
}

static std::string timeToStr(float t) {
    if (t <= 0) return "-";

    std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    localtime_r(&tt, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%d/%m/%Y %H:%M");
    return oss.str();
}

static std::string padRight(const std::string& s, size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

namespace cmd {

bool journaling(const std::string& id, std::string& outMsg) {
    if (id.empty()) {
        outMsg = "Error: journaling requiere -id.";
        return false;
    }

    std::string diskPath;
    int32_t partStart = 0;
    int32_t partSize = 0;
    std::string err;

    if (!getMountedById(id, diskPath, partStart, partSize, err)) {
        outMsg = err;
        return false;
    }

    Superblock sb{};
    if (!readAt(diskPath, partStart, &sb, sizeof(Superblock), err)) {
        outMsg = err;
        return false;
    }

    if (sb.s_magic != 0xEF53) {
        outMsg = "Error: la partición no contiene un sistema de archivos válido.";
        return false;
    }

    if (sb.s_filesystem_type != 3) {
        outMsg = "Error: journaling solo está disponible para particiones EXT3.";
        return false;
    }

    int32_t journalingStart = partStart + (int32_t)sizeof(Superblock);
    int32_t journalingBytes = sb.s_bm_inode_start - journalingStart;

    if (journalingBytes <= 0) {
        outMsg = "Error: el área de journaling no es válida.";
        return false;
    }

    int32_t maxEntries = journalingBytes / (int32_t)sizeof(Journal);
    if (maxEntries <= 0) {
        outMsg = "Error: no hay espacio de journaling disponible.";
        return false;
    }

    std::vector<Journal> entries;
    entries.reserve(maxEntries);

    for (int32_t i = 0; i < maxEntries; i++) {
        Journal j{};
        int32_t pos = journalingStart + i * (int32_t)sizeof(Journal);

        if (!readAt(diskPath, pos, &j, sizeof(Journal), err)) {
            outMsg = err;
            return false;
        }

        if (j.j_count == 0) break; 
        entries.push_back(j);
    }

    if (entries.empty()) {
        outMsg = "No hay transacciones registradas en el journaling.";
        return true;
    }

    size_t wOp   = std::string("Operacion").size();
    size_t wPath = std::string("Path").size();
    size_t wCont = std::string("Contenido").size();
    size_t wDate = std::string("Fecha").size();

    struct Row {
        std::string op;
        std::string path;
        std::string content;
        std::string date;
    };

    std::vector<Row> rows;
    rows.reserve(entries.size());

    for (const auto& j : entries) {
        Row r;
        r.op      = cleanFixedString(j.j_content.i_operation, sizeof(j.j_content.i_operation));
        r.path    = cleanFixedString(j.j_content.i_path, sizeof(j.j_content.i_path));
        r.content = cleanFixedString(j.j_content.i_content, sizeof(j.j_content.i_content));
        r.date    = timeToStr(j.j_content.i_date);

        if (r.op.empty()) r.op = "-";
        if (r.path.empty()) r.path = "-";
        if (r.content.empty()) r.content = "-";

        wOp   = std::max(wOp,   r.op.size());
        wPath = std::max(wPath, r.path.size());
        wCont = std::max(wCont, r.content.size());
        wDate = std::max(wDate, r.date.size());

        rows.push_back(r);
    }

    std::ostringstream oss;

    oss << padRight("Operacion", wOp) << " | "
        << padRight("Path", wPath) << " | "
        << padRight("Contenido", wCont) << " | "
        << padRight("Fecha", wDate) << "\n";

    oss << std::string(wOp, '-') << "-+-"
        << std::string(wPath, '-') << "-+-"
        << std::string(wCont, '-') << "-+-"
        << std::string(wDate, '-') << "\n";

    for (const auto& r : rows) {
        oss << padRight(r.op, wOp) << " | "
            << padRight(r.path, wPath) << " | "
            << padRight(r.content, wCont) << " | "
            << padRight(r.date, wDate) << "\n";
    }

    outMsg = oss.str();
    return true;
}

} 