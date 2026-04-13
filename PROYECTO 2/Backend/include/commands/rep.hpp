#pragma once
#include <string>

namespace cmd {

bool rep(
        const std::string& name,          // mbr, disk, inode, ...
        const std::string& path,          // salida /home/..../reporte.png
        const std::string& id,            // id de partición montada
        const std::string& path_file_ls,  // opcional (file y ls)
        std::string& outMsg
    );

}