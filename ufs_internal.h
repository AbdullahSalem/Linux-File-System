#ifndef UFS_INTERNAL_H
#define UFS_INTERNAL_H

#include "userfs.h"

#include <stdint.h>
#include <sys/types.h>

#define UFS_MAGIC 0x55465331
#define UFS_VERSION 1

#define UFS_BITS_PER_BITMAP_BLOCK (UFS_BLOCK_SIZE * 8)

#define UFS_INODE_FLAG_TRASHED (1u << 0)
#define UFS_INODE_FLAG_INLINE_DATA (1u << 1)
#define UFS_INODE_FLAG_SAFE_DELETE (1u << 2)

#define UFS_TRASH_EXPIRY_SECONDS (7 * 24 * 60 * 60)

struct ufs_superblock
{
    uint32_t magic;
    uint32_t block_size;

    uint64_t total_blocks;
    uint32_t total_inodes;

    // Inode bitmap
    uint32_t inode_bitmap_start;
    uint32_t inode_bitmap_blocks;

    // Block bitmap
    uint32_t block_bitmap_start;
    uint32_t block_bitmap_blocks;

    // Inode table
    uint32_t inode_table_start;
    uint32_t inode_table_blocks;

    // Data area
    uint64_t data_start;
    uint64_t data_blocks;

    // Root directory
    uint32_t root_inode;

    // Filesystem version
    uint32_t version;

    // Journal area
    uint32_t journal_start;
    uint32_t journal_blocks;

    // Reserved (pads the struct out to exactly 512 bytes)
    uint8_t reserved[432];
};

struct ufs_inode
{
    uint32_t used;
    uint32_t type;

    char name[UFS_MAX_NAME + 1];

    uint64_t size;

    int32_t parent_inode;
    uint32_t permissions;

    int64_t created_at;
    int64_t modified_at;

    uint32_t block_count;

    uint32_t direct_blocks[10];

    uint32_t indirect_block;
    uint32_t double_indirect_block;
    uint32_t triple_indirect_block;

    int64_t expiry_time;
    uint32_t flags;
    char tags[32];
    uint32_t block_checksums[10];

    uint8_t reserved[44];
};

#define UFS_INLINE_DATA_MAX_SIZE (sizeof(((struct ufs_inode *)0)->direct_blocks))

_Static_assert(sizeof(struct ufs_inode) == 256,
               "ufs_inode must be exactly 256 bytes");

_Static_assert(sizeof(struct ufs_superblock) == 512,
               "ufs_superblock must be exactly 512 bytes");

struct ufs_disk_dirent
{
    uint32_t used;
    uint32_t inode;
    char name[UFS_MAX_NAME + 1];
};

#define UFS_DIRENTS_PER_BLOCK \
    (UFS_BLOCK_SIZE / sizeof(struct ufs_disk_dirent))

struct ufs_open_file
{
    int used;

    int inode_number;

    int flags;

    off_t position;
};

#define UFS_JOURNAL_MAGIC 0x4A524E4C
struct ufs_journal_record
{
    uint32_t magic;
    uint64_t target_block;
    uint32_t transaction_id;
    uint32_t is_commit;
    uint8_t padding[492];
};

#endif
