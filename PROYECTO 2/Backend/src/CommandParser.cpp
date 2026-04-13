#include "CommandParser.hpp"

#include <cctype>
#include <algorithm>
#include <sstream>

#include "DiskManager.hpp"
#include "commands/mkfs.hpp"   


#include "commands/login.hpp"
#include "commands/cat.hpp"
#include "commands/mkgrp.hpp"
#include "commands/rmgrp.hpp"
#include "commands/mkusr.hpp"
#include "commands/rmusr.hpp"
#include "commands/chgrp.hpp"

#include "commands/mkfile.hpp"
#include "commands/mkdir.hpp"

#include "commands/exec.hpp"

#include "commands/rep.hpp"

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

std::vector<std::string> tokenizeRespectQuotes(const std::string& line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inQuotes = false;

    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];

        if (c == '"') {
            inQuotes = !inQuotes;
            cur.push_back(c);
            continue;
        }

        if (!inQuotes && std::isspace((unsigned char)c)) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }

    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

static std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size()-2);
    return s;
}

ParsedCommand parseCommand(
    const std::string& line,
    const std::vector<std::string>& allowedParamsLower
) {
    ParsedCommand out{};
    auto tokens = tokenizeRespectQuotes(line);
    if (tokens.empty()) return out;

    out.name = toLower(tokens[0]);

    auto isAllowed = [&](const std::string& keyLower){
        for (auto& a : allowedParamsLower) if (a == keyLower) return true;
        return false;
    };

    for (size_t i = 1; i < tokens.size(); i++) {
        std::string t = tokens[i];

        if (t.empty() || t[0] != '-') continue;

        // Caso -param=valor
        auto eq = t.find('=');
        if (eq != std::string::npos) {
            std::string key = toLower(t.substr(0, eq));
            std::string val = stripQuotes(t.substr(eq + 1));
            if (!isAllowed(key)) out.unknown.push_back(key);
            else out.params[key] = val;
            continue;
        }

        // Caso -param valor
        std::string key = toLower(t);
        std::string val;

        if (i + 1 < tokens.size() && (!tokens[i+1].empty() && tokens[i+1][0] != '-')) {
            val = stripQuotes(tokens[i+1]);
            i++;
        } else {
            val = ""; // bandera sin valor
        }

        if (!isAllowed(key)) out.unknown.push_back(key);
        else out.params[key] = val;
    }

    return out;
}

std::string executeLine(const std::string& line) {
    if (line.empty()) return "";

    std::vector<std::string> tokens = tokenizeRespectQuotes(line);
    if (tokens.empty()) return "";

    std::string cmdName = toLower(tokens[0]);
    std::string outMsg;

    // =========================================================
    // MKDISK
    // =========================================================
    if (cmdName == "mkdisk") {
        ParsedCommand pc = parseCommand(line, {"-size", "-unit", "-fit", "-path"});
        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en mkdisk.";

        std::string sizeStr = pc.params.count("-size") ? pc.params["-size"] : "";
        std::string unit    = pc.params.count("-unit") ? pc.params["-unit"] : "";
        std::string fit     = pc.params.count("-fit")  ? pc.params["-fit"]  : "";
        std::string path    = pc.params.count("-path") ? pc.params["-path"] : "";

        if (sizeStr.empty() || path.empty()) {
            return "Error: mkdisk requiere -size y -path.";
        }

        int size = 0;
        try { size = std::stoi(sizeStr); }
        catch (...) { return "Error: -size debe ser un entero positivo."; }

        if (!DiskManager::mkdisk(size, unit, fit, path, outMsg)) return outMsg;
        return outMsg;
    }

    // =========================================================
    // RMDISK
    // =========================================================
    if (cmdName == "rmdisk") {
        ParsedCommand pc = parseCommand(line, {"-path"});
        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en rmdisk.";

        std::string path = pc.params.count("-path") ? pc.params["-path"] : "";
        if (path.empty()) return "Error: rmdisk requiere -path.";

        if (!DiskManager::rmdisk(path, outMsg)) return outMsg;
        return outMsg;
    }

    // =========================================================
    // FDISK
    // =========================================================
    if (cmdName == "fdisk") {
        ParsedCommand pc = parseCommand(line, {"-size","-unit","-path","-type","-fit","-name","-delete","-add"});
        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en fdisk.";

        std::string sizeStr = pc.params.count("-size") ? pc.params["-size"] : "";
        std::string unit    = pc.params.count("-unit") ? pc.params["-unit"] : "";
        std::string path    = pc.params.count("-path") ? pc.params["-path"] : "";
        std::string type    = pc.params.count("-type") ? pc.params["-type"] : "";
        std::string fit     = pc.params.count("-fit")  ? pc.params["-fit"]  : "";
        std::string name    = pc.params.count("-name") ? pc.params["-name"] : "";

        if (sizeStr.empty() || path.empty() || name.empty()) {
            return "Error: fdisk requiere -size, -path y -name (para crear).";
        }

        int32_t size = 0;
        try { size = (int32_t)std::stoi(sizeStr); }
        catch (...) { return "Error: -size debe ser entero positivo."; }

        if (!DiskManager::fdiskCreate(size, unit, path, type, fit, name, outMsg)) return outMsg;
        return outMsg;
    }

    // =========================================================
    // MOUNT
    // =========================================================
    if (cmdName == "mount") {
        ParsedCommand pc = parseCommand(line, {"-path", "-name"});

        if (!pc.unknown.empty()) {
            return "Error: parámetro no reconocido en mount.";
        }

        std::string path = pc.params.count("-path") ? pc.params["-path"] : "";
        std::string name = pc.params.count("-name") ? pc.params["-name"] : "";

        if (path.empty() || name.empty()) {
            return "Error: mount requiere -path y -name.";
        }

        if (!DiskManager::mountPartition(path, name, outMsg)) {
            return outMsg;
        }
        return outMsg;
    }

    // =========================================================
    // MOUNTED
    // =========================================================
    if (cmdName == "mounted") {
        ParsedCommand pc = parseCommand(line, {});
        if (!pc.unknown.empty() || !pc.params.empty()) {
            return "Error: mounted no acepta parámetros.";
        }

        if (!DiskManager::mounted(outMsg)) {
            return outMsg;
        }
        return outMsg;
    }

    // =========================================================
    // MKFS
    // =========================================================
    if (cmdName == "mkfs") {
        ParsedCommand pc = parseCommand(line, {"-id", "-type"});
        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en mkfs.";

        std::string id   = pc.params.count("-id")   ? pc.params["-id"]   : "";
        std::string type = pc.params.count("-type") ? pc.params["-type"] : "full";

        if (id.empty()) return "Error: mkfs requiere -id.";

        type = toLower(type);
        if (type.empty()) type = "full";
        if (type != "full") return "Error: mkfs -type solo admite 'full'.";

        if (!cmd::mkfs(id, type, outMsg)) return outMsg;
        return outMsg;
    }

    // =========================================================
    // MKGRP
    // =========================================================
    if (cmdName == "mkgrp") {
        ParsedCommand pc = parseCommand(line, {"-name"});
        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en mkgrp.";

        std::string name = pc.params.count("-name") ? pc.params["-name"] : "";
        if (name.empty()) return "Error: mkgrp requiere -name.";

        // guard (sesión activa) (aunque mkgrp.cpp también lo valida)
        if (!cmd::hasActiveSession()) return "Error: no existe una sesión activa.";

        if (!cmd::mkgrp(name, outMsg)) return outMsg;
        return outMsg;
    }

    // =========================================================
    // RMGRP
    // =========================================================
    if (cmdName == "rmgrp") {
        ParsedCommand pc = parseCommand(line, {"-name"});
        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en rmgrp.";

        std::string name = pc.params.count("-name") ? pc.params["-name"] : "";
        if (name.empty()) return "Error: rmgrp requiere -name.";

        if (!cmd::hasActiveSession()) return "Error: no existe una sesión activa.";

        if (!cmd::rmgrp(name, outMsg)) return outMsg;
        return outMsg;
    }

    // =========================================================
    // MKUSR
    // =========================================================
    if (cmdName == "mkusr") {
        ParsedCommand pc = parseCommand(line, {"-user","-pass","-grp"});
        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en mkusr.";

        std::string user = pc.params.count("-user") ? pc.params["-user"] : "";
        std::string pass = pc.params.count("-pass") ? pc.params["-pass"] : "";
        std::string grp  = pc.params.count("-grp")  ? pc.params["-grp"]  : "";

        if (user.empty() || pass.empty() || grp.empty()) {
            return "Error: mkusr requiere -user, -pass y -grp.";
        }

        if (!cmd::hasActiveSession()) return "Error: no existe una sesión activa.";

        if (!cmd::mkusr(user, pass, grp, outMsg)) return outMsg;
        return outMsg;
    }

    // =========================================================
    // MKFILE
    // =========================================================
    if (cmdName == "mkfile") {
        ParsedCommand pc = parseCommand(line, {"-path","-r","-size","-cont"});
        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en mkfile.";

        std::string path = pc.params.count("-path") ? pc.params["-path"] : "";
        std::string sizeStr = pc.params.count("-size") ? pc.params["-size"] : "";
        std::string cont = pc.params.count("-cont") ? pc.params["-cont"] : "";

        // -r es bandera: si trae valor -> error
        bool recursive = false;
        if (pc.params.count("-r")) {
            if (!pc.params["-r"].empty()) return "Error: -r no recibe valor.";
            recursive = true;
        }

        int32_t size = 0;
        if (!sizeStr.empty()) {
            try { size = (int32_t)std::stoi(sizeStr); }
            catch (...) { return "Error: -size debe ser entero."; }
            if (size < 0) return "Error: -size no puede ser negativo.";
        }

        if (path.empty()) return "Error: mkfile requiere -path.";

        if (!cmd::mkfile(path, recursive, size, cont, outMsg)) return outMsg;
        return outMsg;
    }

    // =========================================================
    // MKDIR
    // =========================================================
    if (cmdName == "mkdir") {
        ParsedCommand pc = parseCommand(line, {"-path","-p"});
        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en mkdir.";

        std::string path = pc.params.count("-path") ? pc.params["-path"] : "";
        if (path.empty()) return "Error: mkdir requiere -path.";

        bool parents = false;
        if (pc.params.count("-p")) {
            if (!pc.params["-p"].empty()) return "Error: -p no recibe valor.";
            parents = true;
        }

        if (!cmd::mkdir(path, parents, outMsg)) return outMsg;
        return outMsg;
    }

    
    // =========================================================
    // LOGIN  (NO requiere sesión previa)
    // login -usr= -pwd= -id=
    // =========================================================
    if (cmdName == "login") {
        ParsedCommand pc = parseCommand(line, {"-user","-pass","-id"});
        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en login.";

        std::string user = pc.params.count("-user") ? pc.params["-user"] : "";
        std::string pass = pc.params.count("-pass") ? pc.params["-pass"] : "";
        std::string id  = pc.params.count("-id")  ? pc.params["-id"]  : "";

        if (!cmd::login(user, pass, id, outMsg)) return outMsg;
        return outMsg;
    }

    // =========================================================
    // RMUSR
    // =========================================================
    if (cmdName == "rmusr") {
        ParsedCommand pc = parseCommand(line, {"-user"});
        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en rmusr.";

        std::string user = pc.params.count("-user") ? pc.params["-user"] : "";
        if (user.empty()) return "Error: rmusr requiere -user.";

        if (!cmd::hasActiveSession()) return "Error: no existe una sesión activa.";

        if (!cmd::rmusr(user, outMsg)) return outMsg;
        return outMsg;
    }

    // =========================================================
    // CHGRP
    // =========================================================
    if (cmdName == "chgrp") {
        ParsedCommand pc = parseCommand(line, {"-user", "-grp"});
        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en chgrp.";

        std::string user = pc.params.count("-user") ? pc.params["-user"] : "";
        std::string grp  = pc.params.count("-grp")  ? pc.params["-grp"]  : "";

        if (user.empty() || grp.empty()) return "Error: chgrp requiere -user y -grp.";
        if (!cmd::hasActiveSession()) return "Error: no existe una sesión activa.";

        if (!cmd::chgrp(user, grp, outMsg)) return outMsg;
        return outMsg;
    }

    // =========================================================
    // LOGOUT
    // =========================================================
    if (cmdName == "logout") {
        ParsedCommand pc = parseCommand(line, {});
        if (!pc.unknown.empty() || !pc.params.empty()) return "Error: logout no acepta parámetros.";

        if (!cmd::logout(outMsg)) return outMsg;
        return outMsg;
    }

    // =========================================================
    // GUARD de sesión
    // =========================================================
    auto requiresSession = [&](const std::string& c) {
        return (c == "cat" || c == "mkdir" || c == "mkfile" || c == "remove" || c == "edit");
    };

    if (requiresSession(cmdName) && !cmd::hasActiveSession()) {
        return "Error: no existe una sesión activa. Use login.";
    }

    // =========================================================
    // CAT
    // =========================================================
    if (cmdName == "cat") {
        // Permitimos -file1..-file20 (puedes ampliar)
        ParsedCommand pc = parseCommand(line, {
            "-file1","-file2","-file3","-file4","-file5",
            "-file6","-file7","-file8","-file9","-file10",
            "-file11","-file12","-file13","-file14","-file15",
            "-file16","-file17","-file18","-file19","-file20"
        });

        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en cat.";

        std::vector<std::string> files;
        for (int i = 1; i <= 20; i++) {
            std::string key = "-file" + std::to_string(i);
            if (pc.params.count(key) && !pc.params[key].empty()) {
                files.push_back(pc.params[key]);
            }
        }

        if (files.empty()) return "Error: cat requiere al menos -file1.";

        if (!cmd::cat(files, outMsg)) return outMsg;
        return outMsg;
    }
    
    // =========================================================
    // EXEC (SCRIPT)
    // =========================================================
    if (cmdName == "exec") {
        ParsedCommand pc = parseCommand(line, {"-path"});
        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en exec.";

        std::string path = pc.params.count("-path") ? pc.params["-path"] : "";
        if (path.empty()) return "Error: exec requiere -path.";

        if (!cmd::execScript(path, outMsg)) return outMsg;
        return outMsg;
    }

    // =========================================================
    // REP
    // =========================================================
    if (cmdName == "rep") {
        ParsedCommand pc = parseCommand(line, {"-name","-path","-id","-path_file_ls"});
        if (!pc.unknown.empty()) return "Error: parámetro no reconocido en rep.";

        std::string name = pc.params.count("-name") ? pc.params["-name"] : "";
        std::string path = pc.params.count("-path") ? pc.params["-path"] : "";
        std::string id   = pc.params.count("-id")   ? pc.params["-id"]   : "";
        std::string pfl  = pc.params.count("-path_file_ls") ? pc.params["-path_file_ls"] : "";

        if (name.empty() || path.empty() || id.empty()) {
            return "Error: rep requiere -name, -path, -id.";
        }

        if (!cmd::rep(toLower(name), path, id, pfl, outMsg)) return outMsg;
        return outMsg;
    }
        
    // =========================================================
    // SALIR
    // =========================================================
    if (cmdName == "exit" || cmdName == "quit") {
        return "EXIT";
    }

    return "Error: comando no reconocido.";
}