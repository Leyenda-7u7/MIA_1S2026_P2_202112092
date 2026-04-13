#pragma once
#include <cstdint>
#include <ctime>

#pragma pack(push, 1)

struct Partition {
    char    part_status;        
    char    part_type;          // 'P' o 'E'
    char    part_fit;           // 'B' 'F' 'W'
    int32_t part_start;         // byte inicio
    int32_t part_s;             // tamaño en bytes
    char    part_name[16];      // nombre
    int32_t part_correlative;   
    char    part_id[16];        
};

struct MBR {
    int32_t    mbr_tamano;
    std::time_t mbr_fecha_creacion;
    int32_t    mbr_dsk_signature;
    char       dsk_fit;
    Partition  mbr_partitions[4];
};

struct EBR {
    char    part_mount;       // montada o no
    char    part_fit;         // B F W
    int32_t part_start;       // inicio lógica
    int32_t part_s;           // tamaño lógica
    int32_t part_next;        // byte siguiente EBR (-1 si no hay)
    char    part_name[16];    // nombre
};

// ===================== EXT2 =====================

struct Superblock {
    int32_t s_filesystem_type;   
    int32_t s_inodes_count;
    int32_t s_blocks_count;
    int32_t s_free_blocks_count;
    int32_t s_free_inodes_count;
    std::time_t s_mtime;
    std::time_t s_umtime;
    int32_t s_mnt_count;
    int32_t s_magic;             
    int32_t s_inode_s;           
    int32_t s_block_s;         
    int32_t s_first_ino;
    int32_t s_first_blo;
    int32_t s_bm_inode_start;
    int32_t s_bm_block_start;
    int32_t s_inode_start;
    int32_t s_block_start;
};

struct Inode {
    int32_t i_uid;
    int32_t i_gid;
    int32_t i_s;
    std::time_t i_atime;
    std::time_t i_ctime;
    std::time_t i_mtime;
    int32_t i_block[15];
    char i_type;            
    char i_perm[3];         
};

struct Block { char _dummy[64]; };

struct Content {
    char    b_name[12];
    int32_t b_inodo;
};

struct FolderBlock { Content b_content[4]; };

struct FileBlock { char b_content[64]; };

struct PointerBlock { int32_t b_pointers[16]; };

struct Block64 { char bytes[64]; };

#pragma pack(pop)