#include "reports/mbr_report.hpp"
#include "Structures.hpp"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>

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

static std::string timeToStr(std::time_t t) {
    std::tm* tm = std::localtime(&t);
    if (!tm) return "";
    std::ostringstream oss;
    oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

static bool ebrPosInRange(int32_t pos, int32_t extStart, int32_t extSize) {
    int32_t extEnd = extStart + extSize;
    // debe caber un EBR completo
    return (pos >= extStart) && (pos + (int32_t)sizeof(EBR) <= extEnd);
}

namespace reports {

bool buildMbrDot(const std::string& diskPath, std::string& dotOut, std::string& err) {
    MBR mbr{};
    if (!readMBR(diskPath, mbr, err)) return false;

    std::ostringstream dot;
    dot << "digraph G {\n";
    dot << "  rankdir=TB;\n";
    dot << "  node [shape=plaintext];\n";

    // ====== TABLA MBR ======
    dot << "  mbr [label=<\n";
    dot << "    <table border='1' cellborder='1' cellspacing='0' cellpadding='6'>\n";
    dot << "      <tr><td bgcolor='#3b1f5b' colspan='2'><font color='white'><b>REPORTE DE MBR</b></font></td></tr>\n";
    dot << "      <tr><td><b>mbr_tamano</b></td><td>" << mbr.mbr_tamano << "</td></tr>\n";
    dot << "      <tr><td><b>mbr_fecha_creacion</b></td><td>" << timeToStr(mbr.mbr_fecha_creacion) << "</td></tr>\n";
    dot << "      <tr><td><b>mbr_dsk_signature</b></td><td>" << mbr.mbr_dsk_signature << "</td></tr>\n";
    dot << "      <tr><td><b>dsk_fit</b></td><td>" << mbr.dsk_fit << "</td></tr>\n";
    dot << "    </table>\n";
    dot << "  >];\n\n";

    // ====== PARTICIONES ======
    for (int i = 0; i < 4; i++) {
        const Partition& p = mbr.mbr_partitions[i];

        // Si no está usada
        if (p.part_s <= 0 || p.part_start < 0) continue;

        dot << "  p" << i << " [label=<\n";
        dot << "    <table border='1' cellborder='1' cellspacing='0' cellpadding='6'>\n";
        dot << "      <tr><td bgcolor='#3b1f5b' colspan='2'><font color='white'><b>Particion " << (i+1) << "</b></font></td></tr>\n";
        dot << "      <tr><td><b>part_status</b></td><td>" << p.part_status << "</td></tr>\n";
        dot << "      <tr><td><b>part_type</b></td><td>" << p.part_type << "</td></tr>\n";
        dot << "      <tr><td><b>part_fit</b></td><td>" << p.part_fit << "</td></tr>\n";
        dot << "      <tr><td><b>part_start</b></td><td>" << p.part_start << "</td></tr>\n";
        dot << "      <tr><td><b>part_size</b></td><td>" << p.part_s << "</td></tr>\n";
        dot << "      <tr><td><b>part_name</b></td><td>" << partName16(p.part_name) << "</td></tr>\n";
        dot << "    </table>\n";
        dot << "  >];\n";

        dot << "  mbr -> p" << i << ";\n";

    // ====== EBRs si es extendida ======
    if (p.part_type == 'E') {
        const int32_t extStart = p.part_start;
        const int32_t extSize  = p.part_s;
        const int32_t extEnd   = extStart + extSize;

        int32_t ebrPos = extStart;
        int ecount = 0;

        // Validación inicial: el EBR inicial debe caber dentro de la extendida
        if (!ebrPosInRange(ebrPos, extStart, extSize)) {
            // No abortamos todo el reporte, solo avisamos en DOT
            dot << "  ebrwarn" << i << " [label=<"
                << "<table border='1' cellborder='1' cellspacing='0' cellpadding='6'>"
                << "<tr><td bgcolor='#b00020'><font color='white'><b>EBR ERROR</b></font></td></tr>"
                << "<tr><td>Inicio de EBR fuera de la particion extendida</td></tr>"
                << "<tr><td>extStart=" << extStart << " extEnd=" << extEnd << " ebrPos=" << ebrPos << "</td></tr>"
                << "</table>"
                << ">];\n";
            dot << "  p" << i << " -> ebrwarn" << i << ";\n";
        } else {
            while (ebrPos != -1) {
                // Validar rango ANTES de leer
                if (!ebrPosInRange(ebrPos, extStart, extSize)) {
                    err = "Error: EBR fuera de rango. extendida=[" + std::to_string(extStart) + "," +
                        std::to_string(extEnd) + ") ebrPos=" + std::to_string(ebrPos);
                    return false;
                }

                EBR ebr{};
                if (!readEBRAt(diskPath, ebrPos, ebr, err)) return false;

                // Validación básica: coherencia de start (opcional pero útil)
                // Si tu diseño guarda part_start como "posición del EBR", debería coincidir.
                // Si en tu diseño es "inicio de data", ajusta esta validación.
                if (ebr.part_start != ebrPos) {
                    // No lo hacemos fatal por si tu diseño difiere, pero sí lo podrías mostrar/registrar
                    // (si quieres, lo volvemos fatal luego)
                }

                dot << "  e" << i << "_" << ecount << " [label=<\n";
                dot << "    <table border='1' cellborder='1' cellspacing='0' cellpadding='6'>\n";
                dot << "      <tr><td bgcolor='#ff6b6b' colspan='2'><font color='white'><b>Particion Logica (EBR)</b></font></td></tr>\n";
                dot << "      <tr><td><b>part_mount</b></td><td>" << ebr.part_mount << "</td></tr>\n";
                dot << "      <tr><td><b>part_fit</b></td><td>" << ebr.part_fit << "</td></tr>\n";
                dot << "      <tr><td><b>part_start</b></td><td>" << ebr.part_start << "</td></tr>\n";
                dot << "      <tr><td><b>part_size</b></td><td>" << ebr.part_s << "</td></tr>\n";
                dot << "      <tr><td><b>part_next</b></td><td>" << ebr.part_next << "</td></tr>\n";
                dot << "      <tr><td><b>part_name</b></td><td>" << partName16(ebr.part_name) << "</td></tr>\n";
                dot << "    </table>\n";
                dot << "  >];\n";

                if (ecount == 0) dot << "  p" << i << " -> e" << i << "_" << ecount << ";\n";
                else dot << "  e" << i << "_" << (ecount-1) << " -> e" << i << "_" << ecount << ";\n";

                ecount++;

                // cortar por loop
                if (ecount > 1000) {
                    err = "Error: demasiados EBRs (posible loop/corrupción).";
                    return false;
                }

                // Validar next
                if (ebr.part_next == -1) break;
                if (!ebrPosInRange(ebr.part_next, extStart, extSize)) {
                    err = "Error: EBR part_next fuera de rango. next=" + std::to_string(ebr.part_next) +
                        " extendida=[" + std::to_string(extStart) + "," + std::to_string(extEnd) + ")";
                    return false;
                }

                ebrPos = ebr.part_next;
            }
        }
    }

        dot << "\n";
    }

    dot << "}\n";
    dotOut = dot.str();
    return true;
}

} // namespace reports