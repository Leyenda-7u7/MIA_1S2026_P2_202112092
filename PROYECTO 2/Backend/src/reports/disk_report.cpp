#include "reports/disk_report.hpp"
#include "Structures.hpp"

#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <cctype>

static bool readMBR(const std::string& path, MBR& mbr, std::string& err) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        err = "Error: no se pudo abrir el disco: " + path;
        return false;
    }
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(&mbr), sizeof(MBR));
    if (!file) {
        err = "Error: no se pudo leer el MBR.";
        return false;
    }
    return true;
}

static bool readEBRAt(const std::string& path, int32_t offset, EBR& ebr, std::string& err) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        err = "Error: no se pudo abrir el disco: " + path;
        return false;
    }
    file.seekg(offset, std::ios::beg);
    file.read(reinterpret_cast<char*>(&ebr), sizeof(EBR));
    if (!file) {
        err = "Error: no se pudo leer EBR en offset=" + std::to_string(offset);
        return false;
    }
    return true;
}

static std::string partName16(const char name16[16]) {
    size_t len = 0;
    while (len < 16 && name16[len] != '\0') len++;
    return std::string(name16, name16 + len);
}

static std::string fmtPct(double x) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << x;
    return oss.str();
}

static double pctOfDisk(int64_t bytes, int64_t total) {
    if (total <= 0) return 0.0;
    return (double)bytes * 100.0 / (double)total;
}

struct Seg {
    std::string label;
    int32_t start;
    int32_t size;
};

static void pushFreeIfAny(std::vector<Seg>& segs, int32_t start, int32_t end, const std::string& label) {
    if (end > start) {
        segs.push_back(Seg{label, start, end - start});
    }
}

static std::string escHtml(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;";  break;
            case '>': out += "&gt;";  break;
            case '"': out += "&quot;";break;
            default:  out += c; break;
        }
    }
    return out;
}

// Construye segmentos internos de una extendida: EBR / Lógica / Libres (todo como % del disco)
static bool buildExtendedInnerHtml(const std::string& diskPath,
                                  const Partition& ext,
                                  int64_t diskSize,
                                  std::string& innerHtml,
                                  std::string& err) {

    int32_t extStart = ext.part_start;
    int32_t extEnd   = ext.part_start + ext.part_s;

    // si no hay espacio válido
    if (ext.part_s <= 0 || extStart < 0) {
        innerHtml = "<table border='0' cellborder='1' cellspacing='0'><tr><td>Extendida vacía</td></tr></table>";
        return true;
    }

    // Recorremos cadena de EBRs empezando en extStart
    // OJO: esto asume el primer EBR está en extStart (lo típico).
    // Si tu implementación luego lo maneja distinto, solo ajustás aquí.
    std::vector<Seg> inner;

    int32_t pos = extStart;
    int loops = 0;

    // Si no hay EBR válido, igual mostramos todo libre
    // (pero intentamos leer al menos uno)
    bool anyEbr = false;

    while (pos != -1 && pos < extEnd) {
        EBR ebr{};
        if (!readEBRAt(diskPath, pos, ebr, err)) {
            // si no se pudo leer el primer EBR, asumimos todo libre
            if (!anyEbr) {
                inner.clear();
                inner.push_back(Seg{"Libre", extStart, extEnd - extStart});
                break;
            }
            return false;
        }

        anyEbr = true;

        // Segmento EBR (estructura)
        int32_t ebrStart = pos;
        int32_t ebrEnd   = pos + (int32_t)sizeof(EBR);
        if (ebrEnd > extEnd) ebrEnd = extEnd;

        inner.push_back(Seg{"EBR", ebrStart, std::max(0, ebrEnd - ebrStart)});

        // Segmento "Lógica" (datos de la partición lógica)
        // En tu struct, ebr.part_start debería apuntar al inicio de la lógica.
        int32_t logStart = ebr.part_start;
        int32_t logEnd   = ebr.part_start + ebr.part_s;

        // Validaciones básicas para evitar cosas raras
        if (ebr.part_s > 0 && logStart >= extStart && logStart < extEnd) {
            if (logEnd > extEnd) logEnd = extEnd;

            // si hay hueco entre fin EBR y inicio lógica, es libre
            pushFreeIfAny(inner, ebrEnd, logStart, "Libre");

            inner.push_back(Seg{
                "Lógica",
                logStart,
                std::max(0, logEnd - logStart)
            });

            // libre hasta el siguiente EBR (si existe) o hasta fin extendida
            int32_t next = ebr.part_next;
            int32_t freeFrom = logEnd;

            if (next == -1) {
                pushFreeIfAny(inner, freeFrom, extEnd, "Libre");
                break;
            } else {
                // puede haber libre entre fin lógica y next EBR
                pushFreeIfAny(inner, freeFrom, std::min(next, extEnd), "Libre");
                pos = next;
            }
        } else {
            // si no hay lógica válida, asumimos libre hasta next o fin
            int32_t next = ebr.part_next;
            if (next == -1) {
                pushFreeIfAny(inner, ebrEnd, extEnd, "Libre");
                break;
            } else {
                pushFreeIfAny(inner, ebrEnd, std::min(next, extEnd), "Libre");
                pos = next;
            }
        }

        loops++;
        if (loops > 2000) {
            err = "Error: demasiados EBRs (posible loop/corrupción).";
            return false;
        }
    }

    // Ordenar segmentos internos por start y fusionar si hay overlaps mínimos
    std::sort(inner.begin(), inner.end(), [](const Seg& a, const Seg& b){
        return a.start < b.start;
    });

    // Construir HTML: fila con celdas
    std::ostringstream html;
    html << "<table border='0' cellborder='1' cellspacing='0' cellpadding='6'>";

    // header de extendida
    double pExt = pctOfDisk(ext.part_s, diskSize);
    html << "<tr><td bgcolor='#d8d8ff' colspan='" << std::max<size_t>(1, inner.size()) << "'>"
         << "<b>Extendida</b><br/>"
         << escHtml(partName16(ext.part_name)) << "<br/>"
         << fmtPct(pExt) << "% del disco"
         << "</td></tr>";

    // fila segments
    html << "<tr>";
    if (inner.empty()) {
        html << "<td>Libre</td>";
    } else {
        for (const auto& s : inner) {
            double p = pctOfDisk(s.size, diskSize);
            html << "<td>";
            html << "<b>" << escHtml(s.label) << "</b><br/>"
                 << fmtPct(p) << "%";
            html << "</td>";
        }
    }
    html << "</tr>";

    html << "</table>";
    innerHtml = html.str();
    return true;
}

namespace reports {

bool buildDiskDot(const std::string& diskPath, std::string& dotOut, std::string& err) {
    MBR mbr{};
    if (!readMBR(diskPath, mbr, err)) return false;

    const int64_t diskSize = (int64_t)mbr.mbr_tamano;
    if (diskSize <= 0) {
        err = "Error: tamaño de disco inválido en MBR.";
        return false;
    }

    // Particiones usadas
    std::vector<Partition> parts;
    parts.reserve(4);
    for (int i = 0; i < 4; i++) {
        const Partition& p = mbr.mbr_partitions[i];
        if (p.part_s > 0 && p.part_start >= 0) parts.push_back(p);
    }

    std::sort(parts.begin(), parts.end(), [](const Partition& a, const Partition& b){
        return a.part_start < b.part_start;
    });

    // Construir segmentos a nivel DISCO: MBR, libres, primarias, extendida (celda compuesta)
    std::vector<Seg> segs;

    // MBR ocupa sizeof(MBR) desde 0
    int32_t mbrBytes = (int32_t)sizeof(MBR);
    segs.push_back(Seg{"MBR", 0, mbrBytes});

    int32_t cursor = mbrBytes;

    for (const auto& p : parts) {
        // libre antes de la partición
        pushFreeIfAny(segs, cursor, p.part_start, "Libre");

        // partición
        std::string label;
        if (p.part_type == 'P') label = "Primaria";
        else if (p.part_type == 'E') label = "Extendida";
        else label = "Partición";

        segs.push_back(Seg{label, p.part_start, p.part_s});

        cursor = p.part_start + p.part_s;
    }

    // libre final hasta tamaño del disco
    pushFreeIfAny(segs, cursor, (int32_t)diskSize, "Libre");

    // Ahora generar DOT con una tabla horizontal
    std::ostringstream dot;
    dot << "digraph G {\n";
    dot << "  rankdir=LR;\n";
    dot << "  node [shape=plaintext];\n";

    dot << "  disk [label=<\n";
    dot << "    <table border='1' cellborder='1' cellspacing='0' cellpadding='8'>\n";
    dot << "      <tr><td bgcolor='#3b1f5b' colspan='" << std::max<size_t>(1, segs.size())
        << "'><font color='white'><b>DISK</b></font><br/><font color='white'>"
        << escHtml(diskPath) << "</font></td></tr>\n";

    dot << "      <tr>\n";

    for (const auto& s : segs) {
        double p = pctOfDisk(s.size, diskSize);

        // buscar si este segmento corresponde exactamente a una partición extendida para hacer celda compuesta
        bool isExt = false;
        Partition ext{};
        for (const auto& pp : parts) {
            if (pp.part_type == 'E' && pp.part_start == s.start && pp.part_s == s.size) {
                isExt = true;
                ext = pp;
                break;
            }
        }

        if (!isExt) {
            dot << "        <td>";
            dot << "<b>" << escHtml(s.label) << "</b><br/>";
            dot << fmtPct(p) << "% del disco";
            dot << "</td>\n";
        } else {
            std::string innerHtml;
            if (!buildExtendedInnerHtml(diskPath, ext, diskSize, innerHtml, err)) return false;

            dot << "        <td>";
            dot << innerHtml;
            dot << "</td>\n";
        }
    }

    dot << "      </tr>\n";
    dot << "    </table>\n";
    dot << "  >];\n";

    dot << "}\n";

    dotOut = dot.str();
    return true;
}

} // namespace reports