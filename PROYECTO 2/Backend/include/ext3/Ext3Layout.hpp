#pragma once
#include <cstdint>
#include <algorithm>

// Layout de una partición EXT3
struct Ext3Layout {
    int32_t sb_start;            // inicio del superbloque
    int32_t journaling_start;    // inicio del journaling
    int32_t bm_inodes_start;     // inicio bitmap de inodos
    int32_t bm_blocks_start;     // inicio bitmap de bloques
    int32_t inodes_start;        // inicio tabla de inodos
    int32_t blocks_start;        // inicio área de bloques

    int32_t n_inodes;            // cantidad de inodos
    int32_t n_blocks;            // cantidad de bloques = 3*n
    int32_t n_journaling;        // cantidad de journals = n
};

template <typename SuperblockT, typename JournalingT, typename InodeT, typename BlockT>
int32_t calculate_n_ext3(int32_t partitionSizeBytes) {
    const int32_t sb = (int32_t)sizeof(SuperblockT);
    const int32_t journalingSz = (int32_t)sizeof(JournalingT);
    const int32_t inodeSz = (int32_t)sizeof(InodeT);
    const int32_t blockSz = (int32_t)sizeof(BlockT);

    const int32_t denom = journalingSz + 4 + inodeSz + 3 * blockSz;

    const int64_t numer = (int64_t)partitionSizeBytes - sb;
    if (numer <= 0) return 0;

    const int32_t n = (int32_t)(numer / denom);
    return std::max<int32_t>(0, n);
}

template <typename SuperblockT, typename JournalingT, typename InodeT, typename BlockT>
Ext3Layout build_layout_ext3(int32_t part_start, int32_t part_size) {
    Ext3Layout L{};

    L.sb_start = part_start;

    L.n_inodes = calculate_n_ext3<SuperblockT, JournalingT, InodeT, BlockT>(part_size);
    L.n_blocks = 3 * L.n_inodes;
    L.n_journaling = L.n_inodes;

    const int32_t sbSz = (int32_t)sizeof(SuperblockT);
    const int32_t journalingAreaSz = L.n_journaling * (int32_t)sizeof(JournalingT);

    L.journaling_start = part_start + sbSz;
    L.bm_inodes_start  = L.journaling_start + journalingAreaSz;
    L.bm_blocks_start  = L.bm_inodes_start + L.n_inodes;
    L.inodes_start     = L.bm_blocks_start + L.n_blocks;
    L.blocks_start     = L.inodes_start + L.n_inodes * (int32_t)sizeof(InodeT);

    return L;
}