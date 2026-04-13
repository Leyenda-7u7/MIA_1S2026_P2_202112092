#include "reports/tree_report.hpp"
#include "Structures.hpp"

#include <fstream>
#include <sstream>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cstring>

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
            default:
                if ((unsigned char)c < 32) out += ' ';
                else out += c;
        }
    }
    return out;
}

static std::string trimName12(const char n[12]) {
    size_t len = 0;
    while (len < 12 && n[len] != '\0') len++;
    return std::string(n, n + len);
}

static bool readSuperblock(const std::string& disk, int32_t partStart, Superblock& sb, std::string& err) {
    if (!readAt(disk, partStart, &sb, sizeof(Superblock), err)) return false;
    if (sb.s_magic != 0xEF53) {
        err = "Error: la partición no parece EXT2 (magic inválido).";
        return false;
    }
    return true;
}

static bool readInodeAt(const std::string& disk, const Superblock& sb, int32_t inodeIndex, Inode& ino, std::string& err) {
    int32_t pos = sb.s_inode_start + inodeIndex * (int32_t)sizeof(Inode);
    return readAt(disk, pos, &ino, sizeof(Inode), err);
}

static bool readBlock64At(const std::string& disk, const Superblock& sb, int32_t blockIndex, Block64& b, std::string& err) {
    int32_t pos = sb.s_block_start + blockIndex * 64;
    return readAt(disk, pos, &b, sizeof(Block64), err);
}

static bool readFolderBlockAt(const std::string& disk, const Superblock& sb, int32_t blockIndex, FolderBlock& fb, std::string& err) {
    int32_t pos = sb.s_block_start + blockIndex * 64;
    return readAt(disk, pos, &fb, sizeof(FolderBlock), err);
}

static bool readPointerBlockAt(const std::string& disk, const Superblock& sb, int32_t blockIndex, PointerBlock& pb, std::string& err) {
    int32_t pos = sb.s_block_start + blockIndex * 64;
    return readAt(disk, pos, &pb, sizeof(PointerBlock), err);
}

// -------------------- DOT builders --------------------
static std::string inodeNodeId(int32_t i) { return "inode_" + std::to_string(i); }
static std::string blockNodeId(int32_t b, const char* kind) { return std::string(kind) + "_" + std::to_string(b); }

static std::string inodeHtmlTable(int32_t idx, const Inode& ino) {
    std::ostringstream o;
    o << "<TABLE BORDER=\"1\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"6\">";
    o << "<TR><TD COLSPAN=\"2\" BGCOLOR=\"#264653\"><FONT COLOR=\"white\"><B>Inodo " << idx << "</B></FONT></TD></TR>";
    o << "<TR><TD>i_uid</TD><TD>" << ino.i_uid << "</TD></TR>";
    o << "<TR><TD>i_gid</TD><TD>" << ino.i_gid << "</TD></TR>";
    o << "<TR><TD>i_s</TD><TD>" << ino.i_s << "</TD></TR>";
    o << "<TR><TD>i_type</TD><TD>" << (ino.i_type == '0' ? "Carpeta" : "Archivo") << "</TD></TR>";
    o << "<TR><TD>i_perm</TD><TD>" << std::string(ino.i_perm, ino.i_perm + 3) << "</TD></TR>";

    for (int i = 0; i < 15; i++) {
        o << "<TR><TD>i_block[" << i << "]</TD><TD>" << ino.i_block[i] << "</TD></TR>";
    }

    o << "</TABLE>";
    return o.str();
}

static std::string folderBlockHtmlTable(int32_t bidx, const FolderBlock& fb) {
    std::ostringstream o;
    o << "<TABLE BORDER=\"1\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"6\">";
    o << "<TR><TD COLSPAN=\"2\" BGCOLOR=\"#2a9d8f\"><FONT COLOR=\"white\"><B>Bloque Carpeta " << bidx << "</B></FONT></TD></TR>";
    o << "<TR><TD><B>b_name</B></TD><TD><B>b_inodo</B></TD></TR>";
    for (int i = 0; i < 4; i++) {
        std::string nm = trimName12(fb.b_content[i].b_name);
        o << "<TR><TD>" << escHtml(nm) << "</TD><TD>" << fb.b_content[i].b_inodo << "</TD></TR>";
    }
    o << "</TABLE>";
    return o.str();
}

static std::string fileBlockHtmlTable(int32_t bidx, const Block64& b) {
    std::string s(b.bytes, b.bytes + 64);
    size_t z = s.find('\0');
    if (z != std::string::npos) s.resize(z);
    for (char& c : s) if ((unsigned char)c < 32 && c != '\n' && c != '\t') c = ' ';
    if (s.size() > 120) s = s.substr(0, 120) + "...";

    std::ostringstream o;
    o << "<TABLE BORDER=\"1\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"6\">";
    o << "<TR><TD BGCOLOR=\"#e76f51\"><FONT COLOR=\"white\"><B>Bloque Archivo " << bidx << "</B></FONT></TD></TR>";
    o << "<TR><TD ALIGN=\"LEFT\">" << escHtml(s) << "</TD></TR>";
    o << "</TABLE>";
    return o.str();
}

static std::string pointerBlockHtmlTable(int32_t bidx, const PointerBlock& pb) {
    std::ostringstream o;
    o << "<TABLE BORDER=\"1\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"6\">";
    o << "<TR><TD COLSPAN=\"2\" BGCOLOR=\"#f4a261\"><B>Bloque Apuntadores " << bidx << "</B></TD></TR>";
    o << "<TR><TD><B>Index</B></TD><TD><B>Ptr</B></TD></TR>";
    for (int i = 0; i < 16; i++) {
        o << "<TR><TD>" << i << "</TD><TD>" << pb.b_pointers[i] << "</TD></TR>";
    }
    o << "</TABLE>";
    return o.str();
}

// -------------------- Recorrido TREE --------------------
struct Ctx {
    std::string disk;
    Superblock sb{};
    std::ostringstream dot;

    // ✅ Separar: lo dibujado vs lo visitado (bug arreglado)
    std::unordered_set<int32_t> visitedInodes;
    std::unordered_set<int32_t> drawnInodes;

    std::unordered_set<int32_t> drawnFolderBlocks;
    std::unordered_set<int32_t> drawnFileBlocks;
    std::unordered_set<int32_t> drawnPtrBlocks;
};

static void ensureInodeNode(Ctx& c, int32_t idx, const Inode& ino) {
    if (c.drawnInodes.count(idx)) return;
    c.drawnInodes.insert(idx);

    c.dot << inodeNodeId(idx)
          << " [shape=plaintext label=<"
          << inodeHtmlTable(idx, ino)
          << ">];\n";
}

static void ensureFolderBlockNode(Ctx& c, int32_t bidx, const FolderBlock& fb) {
    if (c.drawnFolderBlocks.count(bidx)) return;
    c.drawnFolderBlocks.insert(bidx);

    c.dot << blockNodeId(bidx, "bcarp")
          << " [shape=plaintext label=<"
          << folderBlockHtmlTable(bidx, fb)
          << ">];\n";
}

static void ensureFileBlockNode(Ctx& c, int32_t bidx, const Block64& b) {
    if (c.drawnFileBlocks.count(bidx)) return;
    c.drawnFileBlocks.insert(bidx);

    c.dot << blockNodeId(bidx, "barch")
          << " [shape=plaintext label=<"
          << fileBlockHtmlTable(bidx, b)
          << ">];\n";
}

static void ensurePointerBlockNode(Ctx& c, int32_t bidx, const PointerBlock& pb) {
    if (c.drawnPtrBlocks.count(bidx)) return;
    c.drawnPtrBlocks.insert(bidx);

    c.dot << blockNodeId(bidx, "bptr")
          << " [shape=plaintext label=<"
          << pointerBlockHtmlTable(bidx, pb)
          << ">];\n";
}

static void walkInode(Ctx& c, int32_t inodeIdx, std::string& err);

// Apuntador simple: i_block[12] apunta a PointerBlock (16 ptrs)
static void walkSingleIndirect(Ctx& c, int32_t ownerInodeIdx, char ownerType, int32_t ptrBlockIdx, std::string& err) {
    if (ptrBlockIdx < 0) return;

    PointerBlock pb{};
    if (!readPointerBlockAt(c.disk, c.sb, ptrBlockIdx, pb, err)) return;

    ensurePointerBlockNode(c, ptrBlockIdx, pb);

    // inode -> ptrBlock
    c.dot << inodeNodeId(ownerInodeIdx) << " -> " << blockNodeId(ptrBlockIdx, "bptr") << ";\n";

    for (int i = 0; i < 16; i++) {
        int32_t b = pb.b_pointers[i];
        if (b < 0) continue;

        if (ownerType == '0') {
            FolderBlock fb{};
            if (!readFolderBlockAt(c.disk, c.sb, b, fb, err)) return;
            ensureFolderBlockNode(c, b, fb);

            // ptrBlock -> folderBlock
            c.dot << blockNodeId(ptrBlockIdx, "bptr") << " -> " << blockNodeId(b, "bcarp") << ";\n";

            // folderBlock -> childInode
            for (int k = 0; k < 4; k++) {
                int32_t child = fb.b_content[k].b_inodo;
                if (child < 0) continue;

                std::string nm = trimName12(fb.b_content[k].b_name);
                if (nm == "." || nm == "..") continue;

                c.dot << blockNodeId(b, "bcarp") << " -> " << inodeNodeId(child) << ";\n";
                walkInode(c, child, err);
                if (!err.empty()) return;
            }
        } else {
            Block64 bl{};
            if (!readBlock64At(c.disk, c.sb, b, bl, err)) return;
            ensureFileBlockNode(c, b, bl);

            // ptrBlock -> fileBlock
            c.dot << blockNodeId(ptrBlockIdx, "bptr") << " -> " << blockNodeId(b, "barch") << ";\n";
        }
    }
}

static void walkInode(Ctx& c, int32_t inodeIdx, std::string& err) {
    if (inodeIdx < 0) return;

    // ✅ visited para no ciclar
    if (c.visitedInodes.count(inodeIdx)) return;
    c.visitedInodes.insert(inodeIdx);

    Inode ino{};
    if (!readInodeAt(c.disk, c.sb, inodeIdx, ino, err)) return;

    ensureInodeNode(c, inodeIdx, ino);

    // Directos 0..11
    for (int d = 0; d < 12; d++) {
        int32_t b = ino.i_block[d];
        if (b < 0) continue;

        if (ino.i_type == '0') {
            FolderBlock fb{};
            if (!readFolderBlockAt(c.disk, c.sb, b, fb, err)) return;
            ensureFolderBlockNode(c, b, fb);

            // inode -> folderBlock
            c.dot << inodeNodeId(inodeIdx) << " -> " << blockNodeId(b, "bcarp") << ";\n";

            // folderBlock -> childInode
            for (int k = 0; k < 4; k++) {
                int32_t child = fb.b_content[k].b_inodo;
                if (child < 0) continue;

                std::string nm = trimName12(fb.b_content[k].b_name);
                if (nm == "." || nm == "..") continue;

                c.dot << blockNodeId(b, "bcarp") << " -> " << inodeNodeId(child) << ";\n";
                walkInode(c, child, err);
                if (!err.empty()) return;
            }
        } else {
            Block64 bl{};
            if (!readBlock64At(c.disk, c.sb, b, bl, err)) return;
            ensureFileBlockNode(c, b, bl);

            // inode -> fileBlock
            c.dot << inodeNodeId(inodeIdx) << " -> " << blockNodeId(b, "barch") << ";\n";
        }
    }

    // ✅ Indirecto simple (apuntadores visibles)
    if (ino.i_block[12] >= 0) {
        walkSingleIndirect(c, inodeIdx, ino.i_type, ino.i_block[12], err);
        if (!err.empty()) return;
    }

    // Doble y triple (13,14): si quieres los hacemos luego.
}

namespace reports {

bool buildTreeDot(const std::string& diskPath,
                  int32_t partStart,
                  std::string& outDot,
                  std::string& err) {

    Superblock sb{};
    if (!readSuperblock(diskPath, partStart, sb, err)) return false;

    Ctx c;
    c.disk = diskPath;
    c.sb = sb;

    c.dot << "digraph G {\n";
    c.dot << "  rankdir=LR;\n";
    c.dot << "  splines=ortho;\n";
    c.dot << "  nodesep=0.35;\n";
    c.dot << "  ranksep=0.55;\n";
    c.dot << "  node [fontname=\"Helvetica\"];\n";
    c.dot << "  edge [fontname=\"Helvetica\", arrowsize=0.75];\n\n";

    // Root inode 0
    walkInode(c, 0, err);
    if (!err.empty()) return false;

    c.dot << "}\n";
    outDot = c.dot.str();
    return true;
}

} // namespace reports