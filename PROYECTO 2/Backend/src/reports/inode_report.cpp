#include "reports/inode_report.hpp"
#include "Structures.hpp"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <string>
#include <algorithm>

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
            default:  out += c; break;
        }
    }
    return out;
}

static std::string fmtTime(std::time_t t) {
    if (t <= 0) return "-";
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    std::tm* p = std::localtime(&t);
    if (!p) return "-";
    tm = *p;
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &tm);
    return std::string(buf);
}

static bool readSuperblock(const std::string& diskPath, int32_t partStart, Superblock& sb, std::string& err) {
    if (!readAt(diskPath, partStart, &sb, sizeof(Superblock), err)) return false;
    if (sb.s_magic != 0xEF53) {
        err = "Error: la partición no parece EXT2 (magic inválido).";
        return false;
    }
    return true;
}

static bool readInode(const std::string& diskPath, const Superblock& sb, int32_t inodeIndex, Inode& ino, std::string& err) {
    int32_t pos = sb.s_inode_start + inodeIndex * (int32_t)sizeof(Inode);
    return readAt(diskPath, pos, &ino, sizeof(Inode), err);
}

static bool readBitmapValue(const std::string& diskPath, int32_t bmStart, int32_t index, char& out, std::string& err) {
    return readAt(diskPath, bmStart + index, &out, 1, err);
}

static std::string permToString3(const char perm[3]) {
    std::string s;
    s.push_back(perm[0]);
    s.push_back(perm[1]);
    s.push_back(perm[2]);
    return s;
}

namespace reports {

bool buildInodeDot(const std::string& diskPath, int32_t partStart, std::string& dotOut, std::string& err) {
    Superblock sb{};
    if (!readSuperblock(diskPath, partStart, sb, err)) return false;

    std::ostringstream dot;
    dot << "digraph G {\n";
    dot << "  rankdir=LR;\n";
    dot << "  node [shape=plaintext];\n";

    // Título
    dot << "  title [label=<\n";
    dot << "    <table border='1' cellborder='0' cellspacing='0' cellpadding='8'>\n";
    dot << "      <tr><td bgcolor='#3b1f5b'><font color='white'><b>REPORTE INODE</b></font></td></tr>\n";
    dot << "      <tr><td>" << escHtml(diskPath) << "</td></tr>\n";
    dot << "    </table>\n";
    dot << "  >];\n";

    int shown = 0;

    // Recorremos bitmap de inodos: si es '1' => usado => se muestra
    for (int32_t i = 0; i < sb.s_inodes_count; i++) {
        char v = '0';
        if (!readBitmapValue(diskPath, sb.s_bm_inode_start, i, v, err)) return false;

        if (v != '1') continue;

        Inode ino{};
        if (!readInode(diskPath, sb, i, ino, err)) return false;

        // Node por inodo (tabla tipo ejemplo)
        dot << "  inode" << i << " [label=<\n";
        dot << "    <table border='1' cellborder='1' cellspacing='0' cellpadding='6'>\n";
        dot << "      <tr><td bgcolor='#e9e1ff' colspan='2'><b>Inodo " << i << "</b></td></tr>\n";

        dot << "      <tr><td><b>i_uid</b></td><td>" << ino.i_uid << "</td></tr>\n";
        dot << "      <tr><td><b>i_gid</b></td><td>" << ino.i_gid << "</td></tr>\n";
        dot << "      <tr><td><b>i_size</b></td><td>" << ino.i_s << "</td></tr>\n";
        dot << "      <tr><td><b>i_atime</b></td><td>" << escHtml(fmtTime(ino.i_atime)) << "</td></tr>\n";
        dot << "      <tr><td><b>i_ctime</b></td><td>" << escHtml(fmtTime(ino.i_ctime)) << "</td></tr>\n";
        dot << "      <tr><td><b>i_mtime</b></td><td>" << escHtml(fmtTime(ino.i_mtime)) << "</td></tr>\n";

        dot << "      <tr><td><b>i_type</b></td><td>" << (ino.i_type == '0' ? "0 (carpeta)" : "1 (archivo)") << "</td></tr>\n";
        dot << "      <tr><td><b>i_perm</b></td><td>" << escHtml(permToString3(ino.i_perm)) << "</td></tr>\n";

        // i_block[0..14]
        for (int k = 0; k < 15; k++) {
            dot << "      <tr><td><b>i_block_" << (k + 1) << "</b></td><td>" << ino.i_block[k] << "</td></tr>\n";
        }

        dot << "    </table>\n";
        dot << "  >];\n";

        shown++;
    }

    if (shown == 0) {
        dot << "  none [label=<\n";
        dot << "    <table border='1' cellborder='0' cellspacing='0' cellpadding='10'>\n";
        dot << "      <tr><td>No hay inodos utilizados para mostrar.</td></tr>\n";
        dot << "    </table>\n";
        dot << "  >];\n";
    }

    dot << "}\n";

    dotOut = dot.str();
    return true;
}

} // namespace reports