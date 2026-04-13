#include "reports/sb_report.hpp"
#include "Structures.hpp"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <string>
#include <filesystem>

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

static std::string escHtml(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;";  break;
            case '>': out += "&gt;";  break;
            case '"': out += "&quot;";break;
            default:  out += c;
        }
    }
    return out;
}

static std::string timeToStr(std::time_t t) {
    if (t <= 0) return "-";
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream o;
    o << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return o.str();
}

static void addRow(std::ostringstream& o, const std::string& k, const std::string& v, bool alt) {
    const char* bg = alt ? "#EAF6EF" : "#FFFFFF";
    o << "<TR>";
    o << "<TD BGCOLOR=\"" << bg << "\" ALIGN=\"LEFT\"><B>" << escHtml(k) << "</B></TD>";
    o << "<TD BGCOLOR=\"" << bg << "\" ALIGN=\"LEFT\">" << escHtml(v) << "</TD>";
    o << "</TR>";
}

namespace reports {

bool buildSbDot(const std::string& diskPath,
                int32_t partStart,
                std::string& outDot,
                std::string& err) {

    Superblock sb{};
    if (!readAt(diskPath, partStart, &sb, sizeof(Superblock), err)) return false;

    if (sb.s_magic != 0xEF53) {
        err = "Error: la partición no parece EXT2 (magic inválido).";
        return false;
    }

    // ✅ sb_nombre_hd: solo el nombre del archivo del disco
    std::string sbNombreHd = std::filesystem::path(diskPath).filename().string();
    if (sbNombreHd.empty()) sbNombreHd = diskPath; // fallback

    std::ostringstream h;
    h << "<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"6\">";
    h << "<TR><TD COLSPAN=\"2\" BGCOLOR=\"#145A32\" ALIGN=\"CENTER\">"
      << "<FONT COLOR=\"white\"><B>REPORTE DE SUPERBLOQUE</B></FONT>"
      << "</TD></TR>";

    bool alt = false;

    // ✅ Primero el nombre del disco (como pide el enunciado)
    addRow(h, "sb_nombre_hd", sbNombreHd, alt = !alt);

    addRow(h, "s_filesystem_type", std::to_string(sb.s_filesystem_type), alt=!alt);
    addRow(h, "s_inodes_count", std::to_string(sb.s_inodes_count), alt=!alt);
    addRow(h, "s_blocks_count", std::to_string(sb.s_blocks_count), alt=!alt);
    addRow(h, "s_free_blocks_count", std::to_string(sb.s_free_blocks_count), alt=!alt);
    addRow(h, "s_free_inodes_count", std::to_string(sb.s_free_inodes_count), alt=!alt);
    addRow(h, "s_mtime", timeToStr(sb.s_mtime), alt=!alt);
    addRow(h, "s_umtime", timeToStr(sb.s_umtime), alt=!alt);
    addRow(h, "s_mnt_count", std::to_string(sb.s_mnt_count), alt=!alt);

    {
        std::ostringstream m;
        m << "0x" << std::hex << std::uppercase << sb.s_magic;
        addRow(h, "s_magic", m.str(), alt=!alt);
    }

    addRow(h, "s_inode_s", std::to_string(sb.s_inode_s), alt=!alt);
    addRow(h, "s_block_s", std::to_string(sb.s_block_s), alt=!alt);
    addRow(h, "s_first_ino", std::to_string(sb.s_first_ino), alt=!alt);
    addRow(h, "s_first_blo", std::to_string(sb.s_first_blo), alt=!alt);
    addRow(h, "s_bm_inode_start", std::to_string(sb.s_bm_inode_start), alt=!alt);
    addRow(h, "s_bm_block_start", std::to_string(sb.s_bm_block_start), alt=!alt);
    addRow(h, "s_inode_start", std::to_string(sb.s_inode_start), alt=!alt);
    addRow(h, "s_block_start", std::to_string(sb.s_block_start), alt=!alt);

    h << "</TABLE>";

    std::ostringstream dot;
    dot << "digraph G {\n";
    dot << "rankdir=TB;\n";
    dot << "node [shape=plaintext fontname=\"Helvetica\"];\n";
    dot << "sb [label=<" << h.str() << ">];\n";
    dot << "}\n";

    outDot = dot.str();
    return true;
}

} // namespace reports