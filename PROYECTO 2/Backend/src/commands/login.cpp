#include "commands/login.hpp"
#include "commands/mount.hpp"
#include "Structures.hpp"

#include <fstream>
#include <cstring>
#include <vector>
#include <sstream>

// ===============================
// VARIABLES GLOBALES DE SESIÓN
// ===============================
static bool g_logged = false;
static std::string g_user;
static int32_t g_uid = -1;
static int32_t g_gid = -1;

static std::string g_diskPath;
static int32_t g_partStart = 0;
static int32_t g_partSize = 0;


// ===============================
// HELPERS
// ===============================

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

static std::vector<std::string> split(const std::string& line, char delim) {
    std::vector<std::string> parts;
    std::stringstream ss(line);
    std::string item;

    while (std::getline(ss, item, delim)) {
        parts.push_back(item);
    }

    return parts;
}


// ===============================
// LOGIN
// ===============================
namespace cmd {

bool login(const std::string& user,
           const std::string& pass,
           const std::string& id,
           std::string& outMsg) {

    if (g_logged) {
        outMsg = "Error: ya existe una sesión activa.";
        return false;
    }

    if (user.empty() || pass.empty() || id.empty()) {
        outMsg = "Error: login requiere -user, -pass y -id.";
        return false;
    }

    // 1) Obtener partición montada por ID
    std::string disk;
    int32_t start = 0;
    int32_t size = 0;
    std::string err;

    if (!cmd::getMountedById(id, disk, start, size, err)) {
        outMsg = err;
        return false;
    }

    // 2) Leer Superblock
    Superblock sb{};
    if (!readAt(disk, start, &sb, sizeof(Superblock), err)) {
        outMsg = err;
        return false;
    }

    if (sb.s_magic != 0xEF53) {
        outMsg = "Error: la partición no está formateada en EXT2.";
        return false;
    }

    // 3) Leer inodo 1 (users.txt)
    Inode usersIno{};
    int32_t inode1Pos = sb.s_inode_start + sizeof(Inode); // índice 1

    if (!readAt(disk, inode1Pos, &usersIno, sizeof(Inode), err)) {
        outMsg = err;
        return false;
    }

    if (usersIno.i_type != '1') {
        outMsg = "Error interno: users.txt no es archivo.";
        return false;
    }

    // 4) Leer contenido de users.txt
    std::string content;
    int32_t remaining = usersIno.i_s;

    for (int i = 0; i < 12 && remaining > 0; i++) {
        int32_t blockIndex = usersIno.i_block[i];
        if (blockIndex < 0) continue;

        Block64 b{};
        int32_t blockPos = sb.s_block_start + blockIndex * 64;

        if (!readAt(disk, blockPos, &b, sizeof(Block64), err)) {
            outMsg = err;
            return false;
        }

        int32_t take = std::min(remaining, 64);
        content.append(b.bytes, b.bytes + take);
        remaining -= take;
    }

    // 5) Buscar usuario en el archivo
    std::stringstream ss(content);
    std::string line;

    while (std::getline(ss, line)) {
        if (line.empty()) continue;

        auto parts = split(line, ',');

        // Formato: 1,U,grp,user,pass
        if (parts.size() == 5 && parts[1] == "U") {
            std::string fileUser = parts[3];
            std::string filePass = parts[4];

            if (fileUser == user && filePass == pass) {
                // LOGIN EXITOSO
                g_logged = true;
                g_user = user;
                g_uid = std::stoi(parts[0]);  // UID
                g_gid = 1; // simplificado

                g_diskPath = disk;
                g_partStart = start;
                g_partSize = size;

                outMsg = "Login exitoso. Bienvenido " + user + ".";
                return true;
            }
        }
    }

    outMsg = "Error: usuario o contraseña incorrectos.";
    return false;
}


// ===============================
// LOGOUT
// ===============================
bool logout(std::string& outMsg) {
    if (!g_logged) {
        outMsg = "Error: no existe una sesión activa.";
        return false;
    }

    g_logged = false;
    g_user.clear();
    g_uid = -1;
    g_gid = -1;
    g_diskPath.clear();
    g_partStart = 0;
    g_partSize = 0;

    outMsg = "Sesión cerrada correctamente.";
    return true;
}


// ===============================
// ESTADO SESIÓN
// ===============================
bool hasActiveSession() {
    return g_logged;
}

bool getSessionPartition(std::string& diskPath,
                         int32_t& start,
                         int32_t& size,
                         std::string& outMsg) {

    if (!g_logged) {
        outMsg = "Error: no existe una sesión activa.";
        return false;
    }

    diskPath = g_diskPath;
    start = g_partStart;
    size = g_partSize;
    return true;
}

int32_t sessionUid() { return g_uid; }
int32_t sessionGid() { return g_gid; }

} // namespace cmd