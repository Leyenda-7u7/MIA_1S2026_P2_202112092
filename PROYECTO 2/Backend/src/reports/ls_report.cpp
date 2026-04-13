#include "reports/ls_report.hpp"
#include "Structures.hpp"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <ctime>
#include <string>

static bool readAt(const std::string& path, int32_t offset, void* data, size_t sz, std::string& err) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        err = "Error: no se pudo abrir disco";
        return false;
    }
    file.seekg(offset, std::ios::beg);
    file.read(reinterpret_cast<char*>(data), (std::streamsize)sz);
    if (!file.good()) {
        err = "Error: no se pudo leer disco (offset=" + std::to_string(offset) + ")";
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
            default:
                if ((unsigned char)c < 32) out += ' ';
                else out += c;
        }
    }
    return out;
}

static std::string name12(const char n[12]) {
    size_t len = 0;
    while (len < 12 && n[len] != '\0') len++;
    return std::string(n, n + len);
}

static std::string permsToStr(const char perm[3], bool isDir) {
    int u = (perm[0] >= '0' && perm[0] <= '7') ? (perm[0] - '0') : 0;
    int g = (perm[1] >= '0' && perm[1] <= '7') ? (perm[1] - '0') : 0;
    int o = (perm[2] >= '0' && perm[2] <= '7') ? (perm[2] - '0') : 0;

    std::string s;
    s += (isDir ? 'd' : '-');

    s += (u & 4) ? 'r' : '-';
    s += (u & 2) ? 'w' : '-';
    s += (u & 1) ? 'x' : '-';

    s += (g & 4) ? 'r' : '-';
    s += (g & 2) ? 'w' : '-';
    s += (g & 1) ? 'x' : '-';

    s += (o & 4) ? 'r' : '-';
    s += (o & 2) ? 'w' : '-';
    s += (o & 1) ? 'x' : '-';

    return s;
}

static std::string timeStr(time_t t) {
    if (t <= 0) return "01/01/1970 00:00";
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream o;
    o << std::put_time(&tm, "%d/%m/%Y %H:%M");
    return o.str();
}

namespace reports {

bool buildLsDot(const std::string& diskPath,
                int32_t partStart,
                const std::string& absPath,
                std::string& outDot,
                std::string& err)
{
    Superblock sb{};
    if(!readAt(diskPath, partStart, &sb, sizeof(Superblock), err))
        return false;

    if (sb.s_magic != 0xEF53) {
        err = "Error: la partición no parece EXT2 (magic inválido).";
        return false;
    }

    int inodeRoot = 0;
    Inode root{};
    if(!readAt(diskPath, sb.s_inode_start + inodeRoot*(int32_t)sizeof(Inode),
               &root, sizeof(Inode), err))
        return false;

    std::ostringstream rows;

    for(int i=0;i<12;i++)
    {
        int b = root.i_block[i];
        if(b < 0) continue;

        FolderBlock fb{};
        if(!readAt(diskPath, sb.s_block_start + b*64, &fb, sizeof(FolderBlock), err))
            return false;

        for(int j=0;j<4;j++)
        {
            if(fb.b_content[j].b_inodo < 0) continue;

            int inodeIndex = fb.b_content[j].b_inodo;

            Inode ino{};
            if(!readAt(diskPath, sb.s_inode_start + inodeIndex*(int32_t)sizeof(Inode),
                       &ino, sizeof(Inode), err))
                return false;

            std::string name = name12(fb.b_content[j].b_name);
            if (name.empty()) name = "-";

            std::string perm = permsToStr(ino.i_perm, ino.i_type=='0');
            std::string type = (ino.i_type=='0') ? "Carpeta" : "Archivo";

            std::string date = timeStr(ino.i_mtime);
            std::string fecha = (date.size() >= 10) ? date.substr(0,10) : date;
            std::string hora  = (date.size() >= 16) ? date.substr(11)   : "";

            rows
            << "<TR>"
            << "<TD>" << escHtml(perm) << "</TD>"
            << "<TD>" << ino.i_uid << "</TD>"
            << "<TD>" << ino.i_gid << "</TD>"
            << "<TD>" << ino.i_s << "</TD>"
            << "<TD>" << escHtml(fecha) << "</TD>"
            << "<TD>" << escHtml(hora) << "</TD>"
            << "<TD>" << escHtml(type) << "</TD>"
            << "<TD>" << escHtml(name) << "</TD>"
            << "</TR>\n";
        }
    }

    std::ostringstream dot;
    dot << "digraph G{\n";
    dot << "  rankdir=TB;\n";
    dot << "  node[shape=plaintext fontname=\"Helvetica\"];\n";

    dot << "  tabla[label=<\n";
    dot << "    <TABLE BORDER=\"1\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"6\">\n";

    // Título (opcional, se ve pro)
    dot << "      <TR><TD COLSPAN=\"8\" BGCOLOR=\"#145A32\">"
        << "<FONT COLOR=\"white\"><B>REPORTE LS</B></FONT>"
        << "</TD></TR>\n";
    dot << "      <TR><TD COLSPAN=\"8\" ALIGN=\"LEFT\">"
        << "<B>Ruta:</B> " << escHtml(absPath.empty() ? "/" : absPath)
        << "</TD></TR>\n";

    // ✅ Encabezados
    dot << "      <TR BGCOLOR=\"#2E8B57\">"
        << "<TD><FONT COLOR=\"white\"><B>Permisos</B></FONT></TD>"
        << "<TD><FONT COLOR=\"white\"><B>Owner</B></FONT></TD>"
        << "<TD><FONT COLOR=\"white\"><B>Grupo</B></FONT></TD>"
        << "<TD><FONT COLOR=\"white\"><B>Size (bytes)</B></FONT></TD>"
        << "<TD><FONT COLOR=\"white\"><B>Fecha</B></FONT></TD>"
        << "<TD><FONT COLOR=\"white\"><B>Hora</B></FONT></TD>"
        << "<TD><FONT COLOR=\"white\"><B>Tipo</B></FONT></TD>"
        << "<TD><FONT COLOR=\"white\"><B>Name</B></FONT></TD>"
        << "</TR>\n";

    dot << rows.str();

    dot << "    </TABLE>\n";
    dot << "  >];\n";   // ✅ cierre correcto del HTML label (UN SOLO >)

    dot << "}\n";

    outDot = dot.str();
    return true;
}

} // namespace reports