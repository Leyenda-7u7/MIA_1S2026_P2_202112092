#include "commands/rep.hpp"
#include "commands/mount.hpp"          // getMountedById

#include "reports/mbr_report.hpp"
#include "reports/disk_report.hpp"
#include "reports/inode_report.hpp"
#include "reports/block_report.hpp"
#include "reports/bm_inode_report.hpp"
#include "reports/bm_bloc_report.hpp"
#include "reports/tree_report.hpp"
#include "reports/sb_report.hpp"
#include "reports/file_report.hpp"
#include "reports/ls_report.hpp"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cctype>

static bool writeTextFile(const std::string& path, const std::string& content, std::string& err) {
    std::ofstream out(path);
    if (!out.is_open()) {
        err = "Error: no se pudo escribir archivo: " + path;
        return false;
    }
    out << content;
    if (!out) {
        err = "Error: fallo escribiendo archivo: " + path;
        return false;
    }
    return true;
}

// Muy simple: detecta formato por extensión (png, jpg, pdf)
static std::string outputFormatFromPath(const std::string& outPath) {
    auto ext = std::filesystem::path(outPath).extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);

    if (ext == ".png") return "png";
    if (ext == ".jpg" || ext == ".jpeg") return "jpg";
    if (ext == ".pdf") return "pdf";
    return "png"; // default
}

static std::string quote(const std::string& s) {
    return "\"" + s + "\"";
}

namespace cmd {

bool rep(const std::string& name,
         const std::string& path,
         const std::string& id,
         const std::string& path_file_ls,
         std::string& outMsg) {

    if (name.empty() || path.empty() || id.empty()) {
        outMsg = "Error: rep requiere -name, -path, -id.";
        return false;
    }

    // 1) Obtener el disco desde el id montado
    std::string diskPath;
    int32_t partStart = 0, partSize = 0;
    std::string err;
    if (!cmd::getMountedById(id, diskPath, partStart, partSize, err)) {
        outMsg = err;
        return false;
    }

    // =========================================================
    // ✅ AUTOPAD / AUTO-CARPETA DESTINO
    // (Esto es lo que te piden: si no existe la carpeta del -path, crearla)
    // =========================================================
    std::filesystem::path outP(path);
    std::filesystem::path outDir = outP.parent_path();
    if (!outDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        if (ec) {
            outMsg = "Error: no se pudo crear carpeta destino: " + outDir.string();
            return false;
        }
    }

    // 3) Normalizar nombre de reporte
    std::string reportName = name;
    for (auto& c : reportName) c = (char)std::tolower((unsigned char)c);

    // =========================================================
    // Reportes TXT (NO graphviz)
    // =========================================================
    if (reportName == "bm_inode") {
        std::string content;
        if (!reports::buildBmInodeText(diskPath, partStart, content, err)) {
            outMsg = err;
            return false;
        }

        if (!writeTextFile(path, content, err)) {
            outMsg = err;
            return false;
        }

        outMsg = "Reporte bm_inode generado: " + path;
        return true;

    } else if (reportName == "file") {
        if (path_file_ls.empty()) {
            outMsg = "Error: rep -name=file requiere -path_file_ls=/ruta/dentro/del/fs.";
            return false;
        }

        std::string content;
        if (!reports::buildFileText(diskPath, partStart, path_file_ls, content, err)) {
            outMsg = err;
            return false;
        }

        if (!writeTextFile(path, content, err)) {
            outMsg = err;
            return false;
        }

        outMsg = "Reporte file generado: " + path;
        return true;
    }

    // =========================================================
    // Reportes con Graphviz
    // =========================================================
    std::string dot;

    if (reportName == "mbr") {
        if (!reports::buildMbrDot(diskPath, dot, err)) { outMsg = err; return false; }

    } else if (reportName == "disk") {
        if (!reports::buildDiskDot(diskPath, dot, err)) { outMsg = err; return false; }

    } else if (reportName == "inode") {
        if (!reports::buildInodeDot(diskPath, partStart, dot, err)) { outMsg = err; return false; }

    } else if (reportName == "block") {
        if (!reports::buildBlockDot(diskPath, partStart, dot, err)) { outMsg = err; return false; }

    } else if (reportName == "bm_bloc" || reportName == "bm_block") {
        std::string content;
        if (!reports::buildBmBlockText(diskPath, partStart, content, err)) {
            outMsg = err;
            return false;
        }

        if (!writeTextFile(path, content, err)) {
            outMsg = err;
            return false;
        }

        outMsg = "Reporte bm_bloc generado: " + path;
        return true;

    } else if (reportName == "tree") {
        if (!reports::buildTreeDot(diskPath, partStart, dot, err)) { outMsg = err; return false; }

    } else if (reportName == "sb") {
        if (!reports::buildSbDot(diskPath, partStart, dot, err)) { outMsg = err; return false; }

    } else if (reportName == "ls") {
        if (path_file_ls.empty()) {
            outMsg = "Error: ls requiere -path_file_ls";
            return false;
        }

        if (!reports::buildLsDot(diskPath, partStart, path_file_ls, dot, err)) {
            outMsg = err;
            return false;
        }
    } else {
        outMsg = "Error: reporte no soportado aún: " + reportName;
        return false;
    }

    // 4) Guardar .dot al lado del output
    std::filesystem::path dotPath = outP;
    dotPath.replace_extension(".dot");

    if (!writeTextFile(dotPath.string(), dot, err)) {
        outMsg = err;
        return false;
    }

    // 5) Ejecutar Graphviz
    std::string fmt = outputFormatFromPath(path);
    std::string cmdLine = "dot -T" + fmt + " " + quote(dotPath.string()) + " -o " + quote(path);

    int rc = std::system(cmdLine.c_str());
    if (rc != 0) {
        outMsg = "Error: graphviz falló ejecutando: " + cmdLine;
        return false;
    }

    outMsg = "Reporte generado: " + path;
    return true;
}

} // namespace cmd