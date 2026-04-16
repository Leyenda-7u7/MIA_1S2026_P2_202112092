#include "commands/find.hpp"
#include "commands/login.hpp"
#include "commands/mount.hpp"
#include "Structures.hpp"

#include <fstream>
#include <string>
#include <vector>
#include <cstring>
#include <sstream>
#include <functional>


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

static bool readSuperblock(const std::string& disk, int32_t partStart, Superblock& sb, std::string& err) {
    if (!readAt(disk, partStart, &sb, sizeof(Superblock), err)) return false;
    if (sb.s_magic != 0xEF53) {
        err = "Error: la partición no parece EXT2/EXT3 (magic inválido).";
        return false;
    }
    return true;
}

static bool readInode(const std::string& disk, const Superblock& sb, int32_t idx, Inode& ino, std::string& err) {
    int32_t pos = sb.s_inode_start + idx * (int32_t)sizeof(Inode);
    return readAt(disk, pos, &ino, sizeof(Inode), err);
}

static bool readFolderBlock(const std::string& disk, const Superblock& sb, int32_t bidx, FolderBlock& fb, std::string& err) {
    int32_t pos = sb.s_block_start + bidx * 64;
    return readAt(disk, pos, &fb, sizeof(FolderBlock), err);
}

static int digitToInt(char c) {
    return (c >= '0' && c <= '7') ? (c - '0') : 0;
}

static bool canReadInode(const Inode& ino, int32_t uid, int32_t gid) {
    if (uid == 1) return true; // root bypass

    int u = digitToInt(ino.i_perm[0]);
    int g = digitToInt(ino.i_perm[1]);
    int o = digitToInt(ino.i_perm[2]);

    if (uid == ino.i_uid) return (u & 4) != 0;
    if (gid == ino.i_gid) return (g & 4) != 0;
    return (o & 4) != 0;
}

static std::string name12ToString(const char n[12]) {
    size_t len = 0;
    while (len < 12 && n[len] != '\0') len++;
    return std::string(n, n + len);
}

static bool splitAbsPath(const std::string& path, std::vector<std::string>& parts, std::string& err) {
    parts.clear();

    if (path.empty() || path[0] != '/') {
        err = "Error: find requiere ruta absoluta que inicie con '/'.";
        return false;
    }

    std::string cur;
    for (size_t i = 1; i < path.size(); i++) {
        char c = path[i];
        if (c == '/') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }

    if (!cur.empty()) parts.push_back(cur);
    return true;
}

static std::string baseNameFromPath(const std::string& path) {
    if (path == "/") return "/";
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

static bool wildcardMatchRec(const std::string& text, const std::string& pattern, size_t i, size_t j) {
    if (j == pattern.size()) return i == text.size();

    if (pattern[j] == '?') {
        if (i >= text.size()) return false;
        return wildcardMatchRec(text, pattern, i + 1, j + 1);
    }

    if (pattern[j] == '*') {
        // '*' = uno o más caracteres
        for (size_t k = i + 1; k <= text.size(); k++) {
            if (wildcardMatchRec(text, pattern, k, j + 1)) return true;
        }
        return false;
    }

    if (i < text.size() && text[i] == pattern[j]) {
        return wildcardMatchRec(text, pattern, i + 1, j + 1);
    }

    return false;
}

static bool wildcardMatch(const std::string& text, const std::string& pattern) {
    return wildcardMatchRec(text, pattern, 0, 0);
}

struct DirChild {
    std::string name;
    int32_t inode;
};

static bool findEntryInDir(const std::string& disk, const Superblock& sb, int32_t dirIno,
                           const std::string& name, int32_t& outInode, std::string& err) {
    Inode dino{};
    if (!readInode(disk, sb, dirIno, dino, err)) return false;
    if (dino.i_type != '0') {
        err = "Error: no es carpeta.";
        return false;
    }

    for (int i = 0; i < 12; i++) {
        int32_t b = dino.i_block[i];
        if (b < 0) continue;

        FolderBlock fb{};
        if (!readFolderBlock(disk, sb, b, fb, err)) return false;

        for (int k = 0; k < 4; k++) {
            if (fb.b_content[k].b_inodo < 0) continue;
            if (name12ToString(fb.b_content[k].b_name) == name) {
                outInode = fb.b_content[k].b_inodo;
                return true;
            }
        }
    }

    outInode = -1;
    return true;
}

static bool resolvePath(const std::string& disk, const Superblock& sb,
                        const std::string& absPath, int32_t& outInode, std::string& err) {
    std::vector<std::string> parts;
    if (!splitAbsPath(absPath, parts, err)) return false;

    if (parts.empty()) {
        outInode = 0; // root
        return true;
    }

    int32_t current = 0;
    for (const auto& p : parts) {
        int32_t next = -1;
        if (!findEntryInDir(disk, sb, current, p, next, err)) return false;
        if (next < 0) {
            err = "Error: no existe la ruta: " + absPath;
            return false;
        }
        current = next;
    }

    outInode = current;
    return true;
}

static bool collectDirChildren(const std::string& disk, const Superblock& sb,
                               int32_t dirInoIdx,
                               std::vector<DirChild>& children,
                               std::string& err) {
    children.clear();

    Inode dir{};
    if (!readInode(disk, sb, dirInoIdx, dir, err)) return false;
    if (dir.i_type != '0') {
        err = "Error: el inodo no es una carpeta.";
        return false;
    }

    for (int i = 0; i < 12; i++) {
        int32_t b = dir.i_block[i];
        if (b < 0) continue;

        FolderBlock fb{};
        if (!readFolderBlock(disk, sb, b, fb, err)) return false;

        for (int k = 0; k < 4; k++) {
            if (fb.b_content[k].b_inodo < 0) continue;

            std::string nm = name12ToString(fb.b_content[k].b_name);
            if (nm == "." || nm == ".." || nm.empty()) continue;

            children.push_back({nm, fb.b_content[k].b_inodo});
        }
    }

    return true;
}

struct ResultNode {
    std::string name;
    std::vector<ResultNode> children;
};

static bool searchTree(const std::string& disk, const Superblock& sb,
                       int32_t inodeIdx,
                       const std::string& nodeName,
                       const std::string& pattern,
                       int32_t uid, int32_t gid,
                       ResultNode& outNode,
                       std::string& err) {
    Inode ino{};
    if (!readInode(disk, sb, inodeIdx, ino, err)) return false;

    // Si no tiene permiso de lectura sobre este archivo/carpeta, no se incluye ni se recorre
    if (!canReadInode(ino, uid, gid)) {
        return true;
    }

    bool selfMatch = wildcardMatch(nodeName, pattern);
    std::vector<ResultNode> matchedChildren;

    if (ino.i_type == '0') {
        std::vector<DirChild> children;
        if (!collectDirChildren(disk, sb, inodeIdx, children, err)) return false;

        for (const auto& ch : children) {
            ResultNode childNode;
            if (!searchTree(disk, sb, ch.inode, ch.name, pattern, uid, gid, childNode, err)) return false;
            if (!childNode.name.empty()) {
                matchedChildren.push_back(childNode);
            }
        }
    }

    if (selfMatch || !matchedChildren.empty()) {
        outNode.name = nodeName;
        outNode.children = std::move(matchedChildren);
    }

    return true;
}

static void renderTree(const ResultNode& node, std::ostringstream& out, int depth) {
    if (node.name.empty()) return;

    if (depth == 0) {
        out << node.name << "\n";
    } else {
        out << std::string((depth - 1) * 2, ' ') << "|_ " << node.name << "\n";
    }

    for (const auto& ch : node.children) {
        renderTree(ch, out, depth + 1);
    }
}

namespace cmd {

bool find(const std::string& path, const std::string& name, std::string& outMsg) {
    if (path.empty() || name.empty()) {
        outMsg = "Error: find requiere -path y -name.";
        return false;
    }

    if (!cmd::hasActiveSession()) {
        outMsg = "Error: no existe una sesión activa. Use login.";
        return false;
    }

    std::string disk;
    int32_t partStart = 0, partSize = 0;
    std::string err;

    if (!cmd::getSessionPartition(disk, partStart, partSize, err)) {
        outMsg = err;
        return false;
    }

    Superblock sb{};
    if (!readSuperblock(disk, partStart, sb, err)) {
        outMsg = err;
        return false;
    }

    int32_t startIno = -1;
    if (!resolvePath(disk, sb, path, startIno, err)) {
        outMsg = err;
        return false;
    }

    int32_t uid = cmd::sessionUid();
    int32_t gid = cmd::sessionGid();

    std::string rootName = baseNameFromPath(path);
    ResultNode result;

    if (!searchTree(disk, sb, startIno, rootName, name, uid, gid, result, err)) {
        outMsg = err;
        return false;
    }

    if (result.name.empty()) {
        outMsg = "No se encontraron coincidencias.";
        return true;
    }

    std::ostringstream oss;
    renderTree(result, oss, 0);
    outMsg = oss.str();
    return true;
}

} // namespace cmd