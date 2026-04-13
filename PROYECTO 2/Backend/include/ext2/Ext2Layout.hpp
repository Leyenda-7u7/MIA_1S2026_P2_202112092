#pragma once
#include <cstdint>
#include <algorithm>

struct Ext2Layout {
    int32_t sb_start;
    int32_t bm_inodes_start;
    int32_t bm_blocks_start;
    int32_t inodes_start;
    int32_t blocks_start;
    int32_t n_inodes;
    int32_t n_blocks; // 3n
};

template <typename SuperblockT, typename InodeT, typename BlockT>
int32_t calculate_n(int32_t partitionSizeBytes) {
    const int32_t sb = (int32_t)sizeof(SuperblockT);
    const int32_t inodeSz = (int32_t)sizeof(InodeT);
    const int32_t blockSz = (int32_t)sizeof(BlockT);

    // n = floor((part_size - sizeof(SB)) / (4 + sizeof(inode) + 3*sizeof(block)))
    const int32_t denom = 4 + inodeSz + 3 * blockSz;

    const int64_t numer = (int64_t)partitionSizeBytes - sb;
    if (numer <= 0) return 0;

    const int32_t n = (int32_t)(numer / denom); // división entera = floor
    return std::max<int32_t>(0, n);
}

template <typename SuperblockT, typename InodeT, typename BlockT>
Ext2Layout build_layout(int32_t part_start, int32_t part_size) {
    Ext2Layout L{};
    L.sb_start = part_start;

    L.n_inodes = calculate_n<SuperblockT, InodeT, BlockT>(part_size);
    L.n_blocks = 3 * L.n_inodes;

    const int32_t sb = (int32_t)sizeof(SuperblockT);

    L.bm_inodes_start = part_start + sb;
    L.bm_blocks_start = L.bm_inodes_start + L.n_inodes;
    L.inodes_start    = L.bm_blocks_start + L.n_blocks;
    L.blocks_start    = L.inodes_start + L.n_inodes * (int32_t)sizeof(InodeT);

    return L;
}