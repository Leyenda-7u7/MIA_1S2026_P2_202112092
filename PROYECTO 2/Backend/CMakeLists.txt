cmake_minimum_required(VERSION 3.16)
project(Proyecto1Backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===============================
# Librería del backend
# ===============================

add_library(core

    src/DiskManager.cpp
    src/ext2/Bitmap.cpp
    src/CommandParser.cpp

    src/commands/mkdisk.cpp
    src/commands/rmdisk.cpp
    src/commands/fdisk.cpp
    src/commands/mount.cpp
    src/commands/mkfs.cpp
    src/commands/login.cpp
    src/commands/cat.cpp
    src/commands/mkgrp.cpp
    src/commands/rmgrp.cpp
    src/commands/mkusr.cpp
    src/commands/chgrp.cpp
    src/commands/rmusr.cpp
    src/commands/mkfile.cpp
    src/commands/mkdir.cpp
    src/commands/exec.cpp

    src/commands/rep.cpp

    src/reports/mbr_report.cpp
    src/reports/disk_report.cpp
    src/reports/inode_report.cpp
    src/reports/block_report.cpp
    src/reports/bm_inode_report.cpp
    src/reports/bm_bloc_report.cpp
    src/reports/tree_report.cpp
    src/reports/file_report.cpp
    src/reports/sb_report.cpp
    src/reports/ls_report.cpp
)

target_include_directories(core PUBLIC include src)

# ===============================
# Ejecutable CLI (tu consola actual)
# ===============================

add_executable(app
    src/main.cpp
)

target_link_libraries(app core)

# ===============================
# Ejecutable servidor API
# ===============================

add_executable(server
    src/server/server.cpp
)

target_link_libraries(server core)
