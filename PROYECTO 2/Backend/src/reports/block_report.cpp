#include "reports/block_report.hpp"
#include "Structures.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cctype>

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
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c; break;
        }
    }
    return out;
}

static std::string name12ToString(const char n[12]) {
    size_t len = 0;
    while (len < 12 && n[len] != '\0') len++;
    return std::string(n, n + len);
}

static bool readSuperblock(const std::string& diskPath, int32_t partStart, Superblock& sb, std::string& err) {
    if (!readAt(diskPath, partStart, &sb, sizeof(Superblock), err)) return false;
    if (sb.s_magic != 0xEF53) {
        err = "Error: la partición no parece EXT2 (magic inválido).";
        return false;
    }
    return true;
}

static bool readBitmapByte(const std::string& diskPath, int32_t bmStart, int32_t index, char& out, std::string& err) {
    return readAt(diskPath, bmStart + index, &out, 1, err);
}

static bool readInodeAt(const std::string& diskPath, const Superblock& sb, int32_t inodeIndex, Inode& ino, std::string& err) {
    int32_t pos = sb.s_inode_start + inodeIndex * (int32_t)sizeof(Inode);
    return readAt(diskPath, pos, &ino, sizeof(Inode), err);
}

static bool readFolderBlockAt(const std::string& diskPath, const Superblock& sb, int32_t blockIndex, FolderBlock& fb, std::string& err) {
    int32_t pos = sb.s_block_start + blockIndex * 64;
    return readAt(diskPath, pos, &fb, sizeof(FolderBlock), err);
}

static bool readBlock64At(const std::string& diskPath, const Superblock& sb, int32_t blockIndex, Block64& b, std::string& err) {
    int32_t pos = sb.s_block_start + blockIndex * 64;
    return readAt(diskPath, pos, &b, sizeof(Block64), err);
}

static bool readPointerBlockAt(const std::string& diskPath, const Superblock& sb, int32_t blockIndex, PointerBlock& pb, std::string& err) {
    int32_t pos = sb.s_block_start + blockIndex * 64;
    return readAt(diskPath, pos, &pb, sizeof(PointerBlock), err);
}

static std::string prettyFileContent(const Block64& b) {
    // Muestra printable; reemplaza no imprimibles con '.'
    std::string s(b.bytes, b.bytes + 64);

    for (char& c : s) {
        unsigned char uc = (unsigned char)c;
        if (c == '\0') c = ' ';
        else if (uc < 32 && c != '\n' && c != '\t') c = '.';
    }

    // recortar espacios del final (solo para presentación)
    while (!s.empty() && (s.back() == ' ')) s.pop_back();

    // insertar <br/> cada 32 chars aprox para que se vea como ejemplo
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        out.push_back(s[i]);
        if ((i + 1) % 32 == 0 && i + 1 < s.size()) out += "<br/>";
    }
    return out.empty() ? "-" : escHtml(out);
}

namespace reports {

bool buildBlockDot(const std::string& diskPath, int32_t partStart, std::string& dotOut, std::string& err) {
    Superblock sb{};
    if (!readSuperblock(diskPath, partStart, sb, err)) return false;

    // 1) Cargar inodos utilizados (para inferir tipo de bloque y crear flechas)
    std::vector<Inode> usedInodes;
    std::vector<int32_t> usedInodeIndex;

    for (int32_t i = 0; i < sb.s_inodes_count; i++) {
        char v = '0';
        if (!readBitmapByte(diskPath, sb.s_bm_inode_start, i, v, err)) return false;
        if (v != '1') continue;

        Inode ino{};
        if (!readInodeAt(diskPath, sb, i, ino, err)) return false;
        usedInodes.push_back(ino);
        usedInodeIndex.push_back(i);
    }

    // 2) Recolectar:
    // - blocks que están en i_block[*]
    // - cuáles de esos son "pointer blocks" (si están en posiciones 12/13/14)
    // - y para flechas: la secuencia de bloques por inodo
    std::unordered_set<int32_t> inodeReferencedBlocks;
    std::unordered_set<int32_t> pointerBlocks; // i_block[12..14]
    std::vector<std::vector<int32_t>> inodeBlockChains; // para flechas entre bloques

    inodeBlockChains.reserve(usedInodes.size());

    for (size_t idx = 0; idx < usedInodes.size(); idx++) {
        const Inode& ino = usedInodes[idx];

        std::vector<int32_t> chain;
        chain.reserve(15);

        for (int k = 0; k < 15; k++) {
            int32_t b = ino.i_block[k];
            if (b < 0) continue;
            inodeReferencedBlocks.insert(b);
            chain.push_back(b);

            if (k >= 12) pointerBlocks.insert(b);
        }

        inodeBlockChains.push_back(chain);
    }

    // 3) Bitmap de bloques: usados reales
    std::unordered_set<int32_t> usedBlocks;
    for (int32_t b = 0; b < sb.s_blocks_count; b++) {
        char v = '0';
        if (!readBitmapByte(diskPath, sb.s_bm_block_start, b, v, err)) return false;
        if (v == '1') usedBlocks.insert(b);
    }

    // 4) También: si hay pointer blocks, leer sus punteros para:
    // - dibujar flechas pointer -> bloques apuntados
    // - y asegurarnos que esos bloques existan como nodos (si están usados)
    std::unordered_map<int32_t, std::vector<int32_t>> pointerEdges; // pb -> {targets}
    for (int32_t pbIdx : pointerBlocks) {
        if (usedBlocks.find(pbIdx) == usedBlocks.end()) continue;

        PointerBlock pb{};
        if (!readPointerBlockAt(diskPath, sb, pbIdx, pb, err)) return false;

        std::vector<int32_t> targets;
        for (int i = 0; i < 16; i++) {
            int32_t t = pb.b_pointers[i];
            if (t >= 0) targets.push_back(t);
        }
        pointerEdges[pbIdx] = targets;
    }

    // 5) DOT
    std::ostringstream dot;
    dot << "digraph G {\n";
    dot << "  rankdir=LR;\n";
    dot << "  node [shape=plaintext];\n";
    dot << "  edge [arrowsize=0.8];\n";

    // Título
    dot << "  title [label=<\n";
    dot << "    <table border='1' cellborder='0' cellspacing='0' cellpadding='8'>\n";
    dot << "      <tr><td bgcolor='#3b1f5b'><font color='white'><b>REPORTE BLOCK</b></font></td></tr>\n";
    dot << "      <tr><td>" << escHtml(diskPath) << "</td></tr>\n";
    dot << "    </table>\n";
    dot << "  >];\n\n";

    int shown = 0;

    // Para que se vea “encadenado” como tu imagen, vamos a forzar un mismo rank por bloques
    dot << "  { rank=same; title; }\n";

    // 6) Crear nodos de todos los bloques usados
    //    - Si es pointer block => "Bloque Apuntadores"
    //    - Si NO, intentamos inferir por inodos:
    //         * si algún inodo tipo carpeta lo referencia => carpeta
    //         * si algún inodo tipo archivo lo referencia => archivo
    //      (si no se logra, lo dejamos como archivo por defecto)
    auto inferBlockKind = [&](int32_t blockIndex) -> char {
        if (pointerBlocks.count(blockIndex)) return 'P'; // pointer
        // buscar si lo referencia un inodo carpeta o archivo
        for (size_t i = 0; i < usedInodes.size(); i++) {
            const Inode& ino = usedInodes[i];
            for (int k = 0; k < 12; k++) { // directos
                if (ino.i_block[k] == blockIndex) {
                    return (ino.i_type == '0') ? 'D' : 'F'; // Directory/File
                }
            }
        }
        // si viene desde un pointer block, no sabemos si es carpeta o archivo:
        // lo más seguro: archivo (porque mkfile usará esto mucho)
        return 'F';
    };

    // Ordenar para salida estable (1..N)
    std::vector<int32_t> orderedBlocks(usedBlocks.begin(), usedBlocks.end());
    std::sort(orderedBlocks.begin(), orderedBlocks.end());

    for (int32_t b : orderedBlocks) {
        char kind = inferBlockKind(b);

        if (kind == 'D') {
            FolderBlock fb{};
            if (!readFolderBlockAt(diskPath, sb, b, fb, err)) return false;

            dot << "  block" << b << " [label=<\n";
            dot << "    <table border='1' cellborder='1' cellspacing='0' cellpadding='6'>\n";
            dot << "      <tr><td bgcolor='#efefef' colspan='2'><b>Bloque Carpeta " << b << "</b></td></tr>\n";
            dot << "      <tr><td><b>b_name</b></td><td><b>b_inodo</b></td></tr>\n";

            for (int i = 0; i < 4; i++) {
                std::string nm = name12ToString(fb.b_content[i].b_name);
                int32_t in = fb.b_content[i].b_inodo;
                if (nm.empty()) nm = "-";
                dot << "      <tr><td>" << escHtml(nm) << "</td><td>" << in << "</td></tr>\n";
            }

            dot << "    </table>\n";
            dot << "  >];\n\n";
            shown++;
        }
        else if (kind == 'P') {
            PointerBlock pb{};
            if (!readPointerBlockAt(diskPath, sb, b, pb, err)) return false;

            dot << "  block" << b << " [label=<\n";
            dot << "    <table border='1' cellborder='0' cellspacing='0' cellpadding='8'>\n";
            dot << "      <tr><td bgcolor='#efefef'><b>Bloque Apuntadores " << b << "</b></td></tr>\n";
            dot << "      <tr><td>";

            for (int i = 0; i < 16; i++) {
                dot << pb.b_pointers[i];
                if (i != 15) dot << ", ";
                if ((i + 1) % 8 == 0 && i != 15) dot << "<br/>";
            }

            dot << "</td></tr>\n";
            dot << "    </table>\n";
            dot << "  >];\n\n";
            shown++;
        }
        else { // 'F' archivo
            Block64 bb{};
            if (!readBlock64At(diskPath, sb, b, bb, err)) return false;

            dot << "  block" << b << " [label=<\n";
            dot << "    <table border='1' cellborder='0' cellspacing='0' cellpadding='8'>\n";
            dot << "      <tr><td bgcolor='#efefef'><b>Bloque Archivo " << b << "</b></td></tr>\n";
            dot << "      <tr><td align='left'>" << prettyFileContent(bb) << "</td></tr>\n";
            dot << "    </table>\n";
            dot << "  >];\n\n";
            shown++;
        }
    }

    if (shown == 0) {
        dot << "  none [label=<\n";
        dot << "    <table border='1' cellborder='0' cellspacing='0' cellpadding='10'>\n";
        dot << "      <tr><td>No hay bloques utilizados para mostrar.</td></tr>\n";
        dot << "    </table>\n";
        dot << "  >];\n";
        dot << "}\n";
        dotOut = dot.str();
        return true;
    }

    // 7) Flechas tipo “cadena” como tu imagen:
    //    Para cada inodo, conectamos sus bloques en el orden i_block (directos y luego indirectos)
    for (const auto& chain : inodeBlockChains) {
        for (size_t i = 1; i < chain.size(); i++) {
            int32_t a = chain[i - 1];
            int32_t b = chain[i];
            if (usedBlocks.count(a) && usedBlocks.count(b)) {
                dot << "  block" << a << " -> block" << b << ";\n";
            }
        }
    }

    // 8) Flechas pointerBlock -> bloques apuntados (cuando existan)
    for (const auto& kv : pointerEdges) {
        int32_t pb = kv.first;
        for (int32_t t : kv.second) {
            if (usedBlocks.count(pb) && usedBlocks.count(t)) {
                dot << "  block" << pb << " -> block" << t << ";\n";
            }
        }
    }

    dot << "}\n";
    dotOut = dot.str();
    return true;
}

} // namespace reports