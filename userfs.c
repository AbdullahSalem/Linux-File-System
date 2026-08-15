#define _POSIX_C_SOURCE 200809L

#include "userfs.h"
#include "ufs_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

/* =========================================================
 * Global Filesystem State
 * ========================================================= */

/* Disk image currently mounted */
static FILE *disk = NULL;

/* Superblock loaded in RAM */
static struct ufs_superblock sb;

static struct ufs_open_file open_files[UFS_MAX_OPEN_FILES];

/* Bitmaps loaded in RAM */
static uint8_t *inode_bitmap;
static uint8_t *block_bitmap;

/* Currently opened files */

/* =========================================================
 * Bitmap Helper Functions
 * ========================================================= */

/*
 * Set a bit to 1.
 *
 * 1 = used
 * 0 = free
 */
static void bitmap_set(uint8_t *bitmap, uint64_t index)
{
    bitmap[index / 8] |=
        (uint8_t)(1u << (index % 8));
}

/*
 * Clear a bit to 0.
 */
static void bitmap_clear(uint8_t *bitmap, uint64_t index)
{
    bitmap[index / 8] &=
        (uint8_t)~(1u << (index % 8));
}

/*
 * Check whether a bit is set.
 *
 * Returns:
 * 1 -> used
 * 0 -> free
 */
static int bitmap_test(const uint8_t *bitmap,
                       uint64_t index)
{
    return (bitmap[index / 8] >>
            (index % 8)) &
           1u;
}

/*
 * Calculate how many bitmap blocks
 * are required to represent 'count' objects.
 */
static uint64_t bitmap_blocks_needed(uint64_t count)
{
    return (count + UFS_BITS_PER_BITMAP_BLOCK - 1) / UFS_BITS_PER_BITMAP_BLOCK;
}

/*
 * Find the first free bit.
 *
 * Returns:
 * >= 0 -> free index
 * -1   -> no free object
 */
static int64_t bitmap_find_free(const uint8_t *bitmap,
                                uint64_t count)
{
    for (uint64_t i = 0; i < count; i++)
    {
        if (!bitmap_test(bitmap, i))
        {
            return (int64_t)i;
        }
    }

    return -1;
}

/* =========================================================
 * Low Level Disk I/O
 * ========================================================= */

/*
 * Read a single UFS_BLOCK_SIZE block from the mounted image.
 */
static int disk_read_block(uint64_t block_num, void *buf)
{
    if (disk == NULL)
    {
        errno = EBADF;
        return -1;
    }

    if (fseeko(disk, (off_t)(block_num * UFS_BLOCK_SIZE), SEEK_SET) != 0)
    {
        return -1;
    }

    if (fread(buf, 1, UFS_BLOCK_SIZE, disk) != UFS_BLOCK_SIZE)
    {
        errno = EIO;
        return -1;
    }

    return 0;
}

/*
 * Write a single UFS_BLOCK_SIZE block to the mounted image.
 */
static int disk_write_block(uint64_t block_num, const void *buf)
{
    if (disk == NULL)
    {
        errno = EBADF;
        return -1;
    }

    if (fseeko(disk, (off_t)(block_num * UFS_BLOCK_SIZE), SEEK_SET) != 0)
    {
        return -1;
    }

    if (fwrite(buf, 1, UFS_BLOCK_SIZE, disk) != UFS_BLOCK_SIZE)
    {
        errno = EIO;
        return -1;
    }

    if (fflush(disk) != 0)
    {
        return -1;
    }

    return 0;
}

/*
 * Persist the in-RAM inode bitmap back to disk.
 */
static int flush_inode_bitmap(void)
{
    for (uint32_t i = 0; i < sb.inode_bitmap_blocks; i++)
    {
        if (disk_write_block(sb.inode_bitmap_start + i,
                             inode_bitmap + (size_t)i * UFS_BLOCK_SIZE) != 0)
        {
            return -1;
        }
    }

    return 0;
}

/*
 * Persist the in-RAM block bitmap back to disk.
 */
static int flush_block_bitmap(void)
{
    for (uint32_t i = 0; i < sb.block_bitmap_blocks; i++)
    {
        if (disk_write_block(sb.block_bitmap_start + i,
                             block_bitmap + (size_t)i * UFS_BLOCK_SIZE) != 0)
        {
            return -1;
        }
    }

    return 0;
}

/* =========================================================
 * Inode Helpers
 * ========================================================= */

#define UFS_INODES_PER_BLOCK (UFS_BLOCK_SIZE / sizeof(struct ufs_inode))

static int read_inode(uint32_t inum, struct ufs_inode *out)
{
    if (inum >= sb.total_inodes)
    {
        errno = EINVAL;
        return -1;
    }

    uint64_t block = sb.inode_table_start + inum / UFS_INODES_PER_BLOCK;
    uint32_t offset = (inum % UFS_INODES_PER_BLOCK) * sizeof(struct ufs_inode);

    uint8_t buf[UFS_BLOCK_SIZE];
    if (disk_read_block(block, buf) != 0)
    {
        return -1;
    }

    memcpy(out, buf + offset, sizeof(struct ufs_inode));
    return 0;
}

static int write_inode(uint32_t inum, const struct ufs_inode *in)
{
    if (inum >= sb.total_inodes)
    {
        errno = EINVAL;
        return -1;
    }

    uint64_t block = sb.inode_table_start + inum / UFS_INODES_PER_BLOCK;
    uint32_t offset = (inum % UFS_INODES_PER_BLOCK) * sizeof(struct ufs_inode);

    uint8_t buf[UFS_BLOCK_SIZE];
    if (disk_read_block(block, buf) != 0)
    {
        return -1;
    }

    memcpy(buf + offset, in, sizeof(struct ufs_inode));

    return disk_write_block(block, buf);
}

/*
 * Allocate a free inode number. Marks it used in the bitmap
 * (and flushes the bitmap) but does NOT write inode contents.
 */
static int64_t alloc_inode(void)
{
    int64_t idx = bitmap_find_free(inode_bitmap, sb.total_inodes);
    if (idx < 0)
    {
        errno = ENOSPC;
        return -1;
    }

    bitmap_set(inode_bitmap, (uint64_t)idx);

    if (flush_inode_bitmap() != 0)
    {
        bitmap_clear(inode_bitmap, (uint64_t)idx);
        return -1;
    }

    return idx;
}

static void free_inode(uint32_t inum)
{
    bitmap_clear(inode_bitmap, inum);
    flush_inode_bitmap();
}

/*
 * Allocate a free data block. Returns the ABSOLUTE block
 * number.
 */
static int64_t alloc_block(void)
{
    int64_t idx = bitmap_find_free(block_bitmap, sb.total_blocks);
    if (idx < 0)
    {
        errno = ENOSPC;
        return -1;
    }

    bitmap_set(block_bitmap, (uint64_t)idx);

    if (flush_block_bitmap() != 0)
    {
        bitmap_clear(block_bitmap, (uint64_t)idx);
        return -1;
    }

    uint8_t zero[UFS_BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    disk_write_block((uint64_t)idx, zero);

    return (int64_t)idx;
}

static void free_block(uint64_t abs_block_num)
{
    if (abs_block_num < sb.data_start || abs_block_num >= sb.total_blocks)
    {
        return;
    }

    bitmap_clear(block_bitmap, abs_block_num);
    flush_block_bitmap();
}

/* =========================================================
 * Directory Helpers
 *
 * A directory's contents live in its direct_blocks[] array
 * (indirect blocks are not used for directories in this
 * implementation, capping a directory at
 * 10 * UFS_DIRENTS_PER_BLOCK entries).
 * ========================================================= */

static int dir_find_entry(const struct ufs_inode *dir,
                          const char *name,
                          uint32_t *out_inode)
{
    for (uint32_t b = 0; b < dir->block_count && b < 10; b++)
    {
        if (dir->direct_blocks[b] == 0)
        {
            continue;
        }

        uint8_t buf[UFS_BLOCK_SIZE];
        if (disk_read_block(dir->direct_blocks[b], buf) != 0)
        {
            return -1;
        }

        struct ufs_disk_dirent *entries = (struct ufs_disk_dirent *)buf;
        for (unsigned e = 0; e < UFS_DIRENTS_PER_BLOCK; e++)
        {
            if (entries[e].used && strcmp(entries[e].name, name) == 0)
            {
                if (out_inode)
                {
                    *out_inode = entries[e].inode;
                }
                return 1; /* found */
            }
        }
    }

    return 0; /* not found */
}

/*
 * Add (name -> child_inum) to a directory. Updates and
 * persists `dir` (the parent inode) if a new block had to be
 * allocated.
 */
static int dir_add_entry(uint32_t dir_inum, struct ufs_inode *dir,
                         const char *name, uint32_t child_inum)
{
    /* Try to find a free slot in an already-allocated block. */
    for (uint32_t b = 0; b < dir->block_count && b < 10; b++)
    {
        uint8_t buf[UFS_BLOCK_SIZE];
        if (disk_read_block(dir->direct_blocks[b], buf) != 0)
        {
            return -1;
        }

        struct ufs_disk_dirent *entries = (struct ufs_disk_dirent *)buf;
        for (unsigned e = 0; e < UFS_DIRENTS_PER_BLOCK; e++)
        {
            if (!entries[e].used)
            {
                entries[e].used = 1;
                entries[e].inode = child_inum;
                strncpy(entries[e].name, name, UFS_MAX_NAME);
                entries[e].name[UFS_MAX_NAME] = '\0';

                return disk_write_block(dir->direct_blocks[b], buf);
            }
        }
    }

    /* No free slot: allocate a new block, if there's room for one. */
    if (dir->block_count >= 10)
    {
        errno = ENOSPC;
        return -1;
    }

    int64_t new_block = alloc_block();
    if (new_block < 0)
    {
        return -1;
    }

    uint8_t buf[UFS_BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));

    struct ufs_disk_dirent *entries = (struct ufs_disk_dirent *)buf;
    entries[0].used = 1;
    entries[0].inode = child_inum;
    strncpy(entries[0].name, name, UFS_MAX_NAME);
    entries[0].name[UFS_MAX_NAME] = '\0';

    if (disk_write_block((uint64_t)new_block, buf) != 0)
    {
        free_block((uint64_t)new_block);
        return -1;
    }

    dir->direct_blocks[dir->block_count] = (uint32_t)new_block;
    dir->block_count++;

    return write_inode(dir_inum, dir);
}

static int dir_remove_entry(struct ufs_inode *dir, const char *name)
{
    for (uint32_t b = 0; b < dir->block_count && b < 10; b++)
    {
        if (dir->direct_blocks[b] == 0)
        {
            continue;
        }

        uint8_t buf[UFS_BLOCK_SIZE];
        if (disk_read_block(dir->direct_blocks[b], buf) != 0)
        {
            return -1;
        }

        struct ufs_disk_dirent *entries = (struct ufs_disk_dirent *)buf;
        for (unsigned e = 0; e < UFS_DIRENTS_PER_BLOCK; e++)
        {
            if (entries[e].used && strcmp(entries[e].name, name) == 0)
            {
                memset(&entries[e], 0, sizeof(entries[e]));
                return disk_write_block(dir->direct_blocks[b], buf);
            }
        }
    }

    errno = ENOENT;
    return -1;
}

static int dir_is_empty(const struct ufs_inode *dir)
{
    for (uint32_t b = 0; b < dir->block_count && b < 10; b++)
    {
        if (dir->direct_blocks[b] == 0)
        {
            continue;
        }

        uint8_t buf[UFS_BLOCK_SIZE];
        if (disk_read_block(dir->direct_blocks[b], buf) != 0)
        {
            /* Treat unreadable block as non-empty to be safe. */
            return 0;
        }

        struct ufs_disk_dirent *entries = (struct ufs_disk_dirent *)buf;
        for (unsigned e = 0; e < UFS_DIRENTS_PER_BLOCK; e++)
        {
            if (entries[e].used)
            {
                return 0;
            }
        }
    }

    return 1;
}

/* =========================================================
 * Path Resolution
 * ========================================================= */

/*
 * Resolve `path` to an inode number.
 * "/" resolves to the root inode.
 */
static int resolve_path(const char *path, uint32_t *out_inode)
{
    if (path == NULL || path[0] != '/')
    {
        errno = EINVAL;
        return -1;
    }

    if (strlen(path) > UFS_MAX_PATH)
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    uint32_t current = sb.root_inode;

    if (strcmp(path, "/") == 0)
    {
        *out_inode = current;
        return 0;
    }

    char copy[UFS_MAX_PATH + 1];
    strncpy(copy, path, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(copy, "/", &saveptr);

    while (tok != NULL)
    {
        struct ufs_inode cur_inode;
        if (read_inode(current, &cur_inode) != 0)
        {
            return -1;
        }

        if (cur_inode.type != UFS_TYPE_DIR)
        {
            errno = ENOTDIR;
            return -1;
        }

        uint32_t next;
        int found = dir_find_entry(&cur_inode, tok, &next);
        if (found < 0)
        {
            return -1;
        }
        if (found == 0)
        {
            errno = ENOENT;
            return -1;
        }

        current = next;
        tok = strtok_r(NULL, "/", &saveptr);
    }

    *out_inode = current;
    return 0;
}

/*
 * Split `path` into its parent directory (resolved to an
 * inode number) and the final path component (copied into
 * `name_out`, which must hold at least UFS_MAX_NAME + 1
 * bytes).
 */
static int resolve_parent(const char *path, uint32_t *parent_inode,
                          char *name_out)
{
    if (path == NULL || path[0] != '/')
    {
        errno = EINVAL;
        return -1;
    }

    size_t len = strlen(path);
    if (len == 0 || len > UFS_MAX_PATH)
    {
        errno = EINVAL;
        return -1;
    }

    if (strcmp(path, "/") == 0)
    {
        /* The root has no parent. */
        errno = EINVAL;
        return -1;
    }

    char copy[UFS_MAX_PATH + 1];
    strncpy(copy, path, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    /* Strip a single trailing slash, if present (e.g. "/a/b/"). */
    size_t clen = strlen(copy);
    if (clen > 1 && copy[clen - 1] == '/')
    {
        copy[clen - 1] = '\0';
    }

    char *slash = strrchr(copy, '/');
    if (slash == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    const char *name = slash + 1;
    if (strlen(name) == 0 || strlen(name) > UFS_MAX_NAME)
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    strcpy(name_out, name);

    if (slash == copy)
    {
        /* Parent is the root, e.g. "/foo". */
        *parent_inode = sb.root_inode;
        return 0;
    }

    *slash = '\0';
    return resolve_path(copy, parent_inode);
}

/* =========================================================
 * Filesystem API
 * ========================================================= */

int ufs_format(const char *image_path, size_t image_size)
{
    if (image_path == NULL || image_size < UFS_BLOCK_SIZE * 32)
    {
        errno = EINVAL;
        return -1;
    }

    FILE *f = fopen(image_path, "wb+");
    if (f == NULL)
    {
        return -1;
    }

    uint64_t total_blocks = image_size / UFS_BLOCK_SIZE;

    /* Zero-fill the whole image up front. */
    uint8_t zero[UFS_BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    for (uint64_t i = 0; i < total_blocks; i++)
    {
        if (fwrite(zero, 1, UFS_BLOCK_SIZE, f) != UFS_BLOCK_SIZE)
        {
            fclose(f);
            unlink(image_path);
            errno = EIO;
            return -1;
        }
    }

    uint32_t total_inodes = (uint32_t)(total_blocks / 4);
    if (total_inodes < 16)
    {
        total_inodes = 16;
    }

    struct ufs_superblock new_sb;
    memset(&new_sb, 0, sizeof(new_sb));

    new_sb.magic = UFS_MAGIC;
    new_sb.block_size = UFS_BLOCK_SIZE;
    new_sb.total_blocks = total_blocks;
    new_sb.total_inodes = total_inodes;
    new_sb.version = UFS_VERSION;

    new_sb.inode_bitmap_start = 1;
    new_sb.inode_bitmap_blocks =
        (uint32_t)bitmap_blocks_needed(total_inodes);

    new_sb.block_bitmap_start =
        new_sb.inode_bitmap_start + new_sb.inode_bitmap_blocks;
    new_sb.block_bitmap_blocks =
        (uint32_t)bitmap_blocks_needed(total_blocks);

    new_sb.inode_table_start =
        new_sb.block_bitmap_start + new_sb.block_bitmap_blocks;
    new_sb.inode_table_blocks = (uint32_t)(((uint64_t)total_inodes * sizeof(struct ufs_inode) + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE);

    new_sb.data_start =
        new_sb.inode_table_start + new_sb.inode_table_blocks;

    if (new_sb.data_start >= total_blocks)
    {
        fclose(f);
        unlink(image_path);
        errno = ENOSPC;
        return -1;
    }

    new_sb.data_blocks = total_blocks - new_sb.data_start;
    new_sb.root_inode = 0;

    /* Write superblock (block 0). */
    uint8_t sb_block[UFS_BLOCK_SIZE];
    memset(sb_block, 0, sizeof(sb_block));
    memcpy(sb_block, &new_sb, sizeof(new_sb));
    fseeko(f, 0, SEEK_SET);
    fwrite(sb_block, 1, UFS_BLOCK_SIZE, f);

    /* Build and write the inode bitmap: inode 0 (root) is used. */
    size_t ibmp_bytes = (size_t)new_sb.inode_bitmap_blocks * UFS_BLOCK_SIZE;
    uint8_t *ibmp = calloc(1, ibmp_bytes);
    ibmp[0] |= 0x1;

    fseeko(f, (off_t)new_sb.inode_bitmap_start * UFS_BLOCK_SIZE, SEEK_SET);
    fwrite(ibmp, 1, ibmp_bytes, f);
    free(ibmp);

    /* Build and write the block bitmap: all metadata blocks
     * (everything before data_start) are marked used. */
    size_t bbmp_bytes = (size_t)new_sb.block_bitmap_blocks * UFS_BLOCK_SIZE;
    uint8_t *bbmp = calloc(1, bbmp_bytes);
    for (uint64_t i = 0; i < new_sb.data_start; i++)
    {
        bbmp[i / 8] |= (uint8_t)(1u << (i % 8));
    }

    fseeko(f, (off_t)new_sb.block_bitmap_start * UFS_BLOCK_SIZE, SEEK_SET);
    fwrite(bbmp, 1, bbmp_bytes, f);
    free(bbmp);

    /* Write inode table: inode 0 is the root directory. */
    struct ufs_inode root;
    memset(&root, 0, sizeof(root));
    root.used = 1;
    root.type = UFS_TYPE_DIR;
    strcpy(root.name, "/");
    root.size = 0;
    root.parent_inode = -1;
    root.permissions = 0755;
    root.created_at = (int64_t)time(NULL);
    root.modified_at = root.created_at;
    root.block_count = 0;

    uint8_t inode_block[UFS_BLOCK_SIZE];
    memset(inode_block, 0, sizeof(inode_block));
    memcpy(inode_block, &root, sizeof(root));

    fseeko(f, (off_t)new_sb.inode_table_start * UFS_BLOCK_SIZE, SEEK_SET);
    fwrite(inode_block, 1, UFS_BLOCK_SIZE, f);

    fflush(f);
    fclose(f);
    return 0;
}

int ufs_mount(const char *image_path)
{
    if (disk != NULL)
    {
        errno = EBUSY;
        return -1;
    }

    disk = fopen(image_path, "rb+");
    if (disk == NULL)
    {
        return -1;
    }

    if (fseeko(disk, 0, SEEK_SET) != 0 ||
        fread(&sb, 1, sizeof(sb), disk) != sizeof(sb))
    {
        fclose(disk);
        disk = NULL;
        errno = EINVAL;
        return -1;
    }

    if (sb.magic != UFS_MAGIC)
    {
        fclose(disk);
        disk = NULL;
        errno = EINVAL;
        return -1;
    }

    size_t ibmp_bytes = (size_t)sb.inode_bitmap_blocks * UFS_BLOCK_SIZE;
    inode_bitmap = malloc(ibmp_bytes);
    fseeko(disk, (off_t)sb.inode_bitmap_start * UFS_BLOCK_SIZE, SEEK_SET);
    fread(inode_bitmap, 1, ibmp_bytes, disk);

    size_t bbmp_bytes = (size_t)sb.block_bitmap_blocks * UFS_BLOCK_SIZE;
    block_bitmap = malloc(bbmp_bytes);
    fseeko(disk, (off_t)sb.block_bitmap_start * UFS_BLOCK_SIZE, SEEK_SET);
    fread(block_bitmap, 1, bbmp_bytes, disk);

    return 0;
}

int ufs_unmount(void)
{
    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (fclose(disk) != 0)
    {
        return -1;
    }

    disk = NULL;
    memset(&sb, 0, sizeof(sb));

    if (inode_bitmap)
    {
        free(inode_bitmap);
        inode_bitmap = NULL;
    }
    if (block_bitmap)
    {
        free(block_bitmap);
        block_bitmap = NULL;
    }

    return 0;
}

int ufs_mkdir(const char *path)
{
    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    uint32_t parent_inum;
    char name[UFS_MAX_NAME + 1];

    if (resolve_parent(path, &parent_inum, name) != 0)
    {
        return -1;
    }

    struct ufs_inode parent;
    if (read_inode(parent_inum, &parent) != 0)
    {
        return -1;
    }

    if (parent.type != UFS_TYPE_DIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    int found = dir_find_entry(&parent, name, NULL);
    if (found < 0)
    {
        return -1;
    }
    if (found == 1)
    {
        errno = EEXIST;
        return -1;
    }

    int64_t new_inum = alloc_inode();
    if (new_inum < 0)
    {
        return -1;
    }

    struct ufs_inode new_dir;
    memset(&new_dir, 0, sizeof(new_dir));
    new_dir.used = 1;
    new_dir.type = UFS_TYPE_DIR;
    strcpy(new_dir.name, name);
    new_dir.size = 0;
    new_dir.parent_inode = (int32_t)parent_inum;
    new_dir.permissions = 0755;
    new_dir.created_at = (int64_t)time(NULL);
    new_dir.modified_at = new_dir.created_at;
    new_dir.block_count = 0;

    if (write_inode((uint32_t)new_inum, &new_dir) != 0)
    {
        free_inode((uint32_t)new_inum);
        return -1;
    }

    /* re-read parent since dir_add_entry may have allocated a
     * block for a previous, unrelated operation in between */
    if (dir_add_entry(parent_inum, &parent, name, (uint32_t)new_inum) != 0)
    {
        int saved_errno = errno;
        new_dir.used = 0;
        write_inode((uint32_t)new_inum, &new_dir);
        free_inode((uint32_t)new_inum);
        errno = saved_errno;
        return -1;
    }

    parent.modified_at = (int64_t)time(NULL);
    write_inode(parent_inum, &parent);

    return 0;
}

int ufs_rmdir(const char *path)
{
    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (path != NULL && strcmp(path, "/") == 0)
    {
        errno = EBUSY;
        return -1;
    }

    uint32_t parent_inum;
    char name[UFS_MAX_NAME + 1];

    if (resolve_parent(path, &parent_inum, name) != 0)
    {
        return -1;
    }

    struct ufs_inode parent;
    if (read_inode(parent_inum, &parent) != 0)
    {
        return -1;
    }

    uint32_t target_inum;
    int found = dir_find_entry(&parent, name, &target_inum);
    if (found < 0)
    {
        return -1;
    }
    if (found == 0)
    {
        errno = ENOENT;
        return -1;
    }

    struct ufs_inode target;
    if (read_inode(target_inum, &target) != 0)
    {
        return -1;
    }

    if (target.type != UFS_TYPE_DIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    if (!dir_is_empty(&target))
    {
        errno = ENOTEMPTY;
        return -1;
    }

    if (dir_remove_entry(&parent, name) != 0)
    {
        return -1;
    }

    parent.modified_at = (int64_t)time(NULL);
    write_inode(parent_inum, &parent);

    /* Free any (empty) data blocks the directory still owns. */
    for (uint32_t b = 0; b < target.block_count && b < 10; b++)
    {
        if (target.direct_blocks[b] != 0)
        {
            free_block(target.direct_blocks[b]);
        }
    }

    memset(&target, 0, sizeof(target));
    target.used = 0;
    write_inode(target_inum, &target);
    free_inode(target_inum);

    return 0;
}

int ufs_listdir(const char *path, struct ufs_dirent *entries, size_t max_entries)
{
    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (path == NULL || (entries == NULL && max_entries > 0))
    {
        errno = EINVAL;
        return -1;
    }

    uint32_t dir_inum;
    if (resolve_path(path, &dir_inum) != 0)
    {
        return -1;
    }

    struct ufs_inode dir;
    if (read_inode(dir_inum, &dir) != 0)
    {
        return -1;
    }

    if (dir.type != UFS_TYPE_DIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    size_t count = 0;

    for (uint32_t b = 0; b < dir.block_count && b < 10; b++)
    {
        if (dir.direct_blocks[b] == 0)
        {
            continue;
        }

        uint8_t buf[UFS_BLOCK_SIZE];
        if (disk_read_block(dir.direct_blocks[b], buf) != 0)
        {
            return -1;
        }

        struct ufs_disk_dirent *dentries = (struct ufs_disk_dirent *)buf;
        for (unsigned e = 0; e < UFS_DIRENTS_PER_BLOCK; e++)
        {
            if (!dentries[e].used)
            {
                continue;
            }

            if (count < max_entries)
            {
                struct ufs_inode child;
                if (read_inode(dentries[e].inode, &child) != 0)
                {
                    return -1;
                }

                strncpy(entries[count].name, dentries[e].name, UFS_MAX_NAME);
                entries[count].name[UFS_MAX_NAME] = '\0';
                entries[count].type = (int)child.type;
                entries[count].size = (size_t)child.size;
            }

            count++;
        }
    }

    return (int)count;
}

int ufs_create(const char *path)
{
    (void)path;
    errno = ENOSYS;
    return -1;
}

int ufs_unlink(const char *path)
{
    (void)path;
    errno = ENOSYS;
    return -1;
}

// 4
int ufs_open(const char *path, int flags)
{
    uint32_t inode_number;
    int fd;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    inode_number = resolve_path(path); // resolve function needs an edit

    if (inode_number < 0)
    {
        errno = ENOENT;
        return -1;
    }

    for (fd = 0; fd < UFS_MAX_OPEN_FILES; fd++)
    {
        if (!open_files[fd].used)
        {
            open_files[fd].used = 1;
            open_files[fd].inode_number = inode_number;
            open_files[fd].position = 0;
            open_files[fd].flags = flags;

            return fd;
        }
    }

    errno = EMFILE;
    return -1;
}

// 4
int ufs_close(int fd)
{
    if (fd < 0 || fd >= UFS_MAX_OPEN_FILES)
    {
        errno = EBADF;
        return -1;
    }

    if (!open_files[fd].used)
    {
        errno = EBADF;
        return -1;
    }

    open_files[fd].used = 0;

    return 0;
}

// 4
off_t ufs_seek(int fd, off_t offset, int whence)
{
    off_t new_position;

    if (fd < 0 || fd >= UFS_MAX_OPEN_FILES)
    {
        errno = EBADF;
        return -1;
    }

    if (!open_files[fd].used)
    {
        errno = EBADF;
        return -1;
    }

    if (whence == SEEK_SET)
    {
        new_position = offset;
    }
    else if (whence == SEEK_CUR)
    {
        new_position = open_files[fd].position + offset;
    }
    else if (whence == SEEK_END)
    {
        // cannot get file size
        new_position = file_size + offset;
    }
    else
    {
        errno = EINVAL;
        return -1;
    }

    if (new_position < 0)
    {
        errno = EINVAL;
        return -1;
    }

    open_files[fd].position = new_position;

    return new_position;
}

ssize_t ufs_read(int fd, void *buf, size_t count)
{
      if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (fd < 0 || fd >= UFS_MAX_OPEN_FILES || !open_files[fd].used)
    {
        errno = EBADF;
        return -1;
    }

    /* UFS_O_RDONLY=0x1, UFS_O_WRONLY=0x2, UFS_O_RDWR=0x3.
     * flags & UFS_O_RDONLY is nonzero for both RDONLY and RDWR. */
    if (!(open_files[fd].flags & UFS_O_RDONLY))
    {
        errno = EBADF;
        return -1;
    }

    if (buf == NULL && count > 0)
    {
        errno = EINVAL;
        return -1;
    }

    struct ufs_inode inode;
    if (read_inode((uint32_t)open_files[fd].inode_number, &inode) != 0)
    {
        return -1;
    }

    if (inode.type != UFS_TYPE_FILE)
    {
        errno = EISDIR;
        return -1;
    }

    off_t offset = open_files[fd].position;

    /* EOF guard must come BEFORE the subtraction below: offset can
     * legitimately exceed inode.size (e.g. after a seek past EOF),
     * and inode.size - offset would underflow (these are unsigned)
     * if we didn't check this first. */
    if ((uint64_t)offset >= inode.size)
    {
        return 0; /* EOF, not an error */
    }

    uint64_t available = inode.size - (uint64_t)offset;
    size_t to_read = (count < available) ? count : (size_t)available;

    size_t bytes_done = 0;
    uint8_t block_buf[UFS_BLOCK_SIZE];
    const uint32_t ptrs_per_block = UFS_BLOCK_SIZE / sizeof(uint32_t);

    while (bytes_done < to_read)
    {
        uint64_t cur = (uint64_t)offset + bytes_done;
        uint32_t block_index = (uint32_t)(cur / UFS_BLOCK_SIZE);
        uint32_t in_block_off = (uint32_t)(cur % UFS_BLOCK_SIZE);

        /* Resolve block_index -> physical block number:
         * direct_blocks[0..9] first, then the single
         * indirect_block for anything beyond that. */
        uint32_t disk_block = 0;
        if (block_index < 10)
        {
            disk_block = inode.direct_blocks[block_index];
        }
        else
        {
            uint32_t ind_index = block_index - 10;
            if (ind_index >= ptrs_per_block)
            {
                errno = EFBIG;
                return -1;
            }
            if (inode.indirect_block != 0)
            {
                uint32_t ptrs[UFS_BLOCK_SIZE / sizeof(uint32_t)];
                if (disk_read_block(inode.indirect_block, ptrs) != 0)
                {
                    return -1;
                }
                disk_block = ptrs[ind_index];
            }
        }

        if (disk_block == 0)
        {
            /* A hole: this range was never written. Treat as zeros
             * rather than reading garbage or failing. */
            memset(block_buf, 0, UFS_BLOCK_SIZE);
        }
        else if (disk_read_block(disk_block, block_buf) != 0)
        {
            return -1;
        }

        size_t chunk = UFS_BLOCK_SIZE - in_block_off;
        size_t remaining = to_read - bytes_done;
        if (chunk > remaining)
        {
            chunk = remaining;
        }

        memcpy((uint8_t *)buf + bytes_done, block_buf + in_block_off, chunk);
        bytes_done += chunk;
    }

    open_files[fd].position += (off_t)bytes_done;
    return (ssize_t)bytes_done;

}

ssize_t ufs_write(int fd, const void *buf, size_t count)
{
    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (fd < 0 || fd >= UFS_MAX_OPEN_FILES || !open_files[fd].used)
    {
        errno = EBADF;
        return -1;
    }

    /* UFS_O_WRONLY=0x2, UFS_O_RDWR=0x3.
     * flags & UFS_O_WRONLY is nonzero for both WRONLY and RDWR. */
    if (!(open_files[fd].flags & UFS_O_WRONLY))
    {
        errno = EBADF;
        return -1;
    }

    if (buf == NULL && count > 0)
    {
        errno = EINVAL;
        return -1;
    }

    if (count == 0)
    {
        return 0;
    }

    uint32_t inum = (uint32_t)open_files[fd].inode_number;
    struct ufs_inode inode;
    if (read_inode(inum, &inode) != 0)
    {
        return -1;
    }

    if (inode.type != UFS_TYPE_FILE)
    {
        errno = EISDIR;
        return -1;
    }

    off_t start_offset = open_files[fd].position;
    if (open_files[fd].flags & UFS_O_APPEND)
    {
        /* Always append at the CURRENT end of file, not wherever
         * the fd's position happens to be. */
        start_offset = (off_t)inode.size;
    }

    size_t bytes_done = 0;
    uint8_t block_buf[UFS_BLOCK_SIZE];
    const uint32_t ptrs_per_block = UFS_BLOCK_SIZE / sizeof(uint32_t);

    while (bytes_done < count)
    {
        uint64_t cur = (uint64_t)start_offset + bytes_done;
        uint32_t block_index = (uint32_t)(cur / UFS_BLOCK_SIZE);
        uint32_t in_block_off = (uint32_t)(cur % UFS_BLOCK_SIZE);

        /* Resolve block_index -> physical block number,
         * allocating on the fly if it doesn't exist yet:
         * direct_blocks[0..9] first, then the single
         * indirect_block for anything beyond that. */
        uint32_t disk_block = 0;
        if (block_index < 10)
        {
            if (inode.direct_blocks[block_index] == 0)
            {
                int64_t nb = alloc_block();
                if (nb < 0)
                {
                    break; /* ENOSPC: stop, keep partial progress */
                }
                inode.direct_blocks[block_index] = (uint32_t)nb;
            }
            disk_block = inode.direct_blocks[block_index];
        }
        else
        {
            uint32_t ind_index = block_index - 10;
            if (ind_index >= ptrs_per_block)
            {
                errno = EFBIG;
                break;
            }
            if (inode.indirect_block == 0)
            {
                int64_t nb = alloc_block();
                if (nb < 0)
                {
                    break;
                }
                /* alloc_block() already zero-fills the new block on
                 * disk, so the pointer table starts all-zero. */
                inode.indirect_block = (uint32_t)nb;
            }
            uint32_t ptrs[UFS_BLOCK_SIZE / sizeof(uint32_t)];
            if (disk_read_block(inode.indirect_block, ptrs) != 0)
            {
                break;
            }
            if (ptrs[ind_index] == 0)
            {
                int64_t nb = alloc_block();
                if (nb < 0)
                {
                    break;
                }
                ptrs[ind_index] = (uint32_t)nb;
                if (disk_write_block(inode.indirect_block, ptrs) != 0)
                {
                    break;
                }
            }
            disk_block = ptrs[ind_index];
        }

        size_t chunk = UFS_BLOCK_SIZE - in_block_off;
        size_t remaining = count - bytes_done;
        if (chunk > remaining)
        {
            chunk = remaining;
        }

        /* Read-modify-write: only needed when NOT overwriting an
         * entire block, since the disk can't be told to touch just
         * part of a block. */
        if (chunk < UFS_BLOCK_SIZE)
        {
            if (disk_read_block(disk_block, block_buf) != 0)
            {
                break;
            }
        }

        memcpy(block_buf + in_block_off, (const uint8_t *)buf + bytes_done, chunk);

        if (disk_write_block(disk_block, block_buf) != 0)
        {
            break;
        }

        bytes_done += chunk;
    }

    uint64_t new_end = (uint64_t)start_offset + bytes_done;
    if (new_end > inode.size)
    {
        inode.size = new_end;
    }
    inode.modified_at = (int64_t)time(NULL);

    /* Persist size + any newly-populated direct_blocks[]/
     * indirect_block pointers set above. If even this fails and
     * nothing was written, report failure; if some bytes did make
     * it to disk, still report that progress. */
    if (write_inode(inum, &inode) != 0 && bytes_done == 0)
    {
        return -1;
    }

    open_files[fd].position = start_offset + (off_t)bytes_done;

    if (bytes_done == 0 && count > 0)
    {
        /* errno was already set by whichever call above failed
         * (alloc_block -> ENOSPC, indirect range -> EFBIG, or
         * disk I/O -> EIO). */
        return -1;
    }

    return (ssize_t)bytes_done;

}

int ufs_truncate(const char *path, size_t size)
{
     if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    uint32_t inum;
    if (resolve_path(path, &inum) != 0)
    {
        return -1;
    }

    struct ufs_inode inode;
    if (read_inode(inum, &inode) != 0)
    {
        return -1;
    }

    if (inode.type != UFS_TYPE_FILE)
    {
        errno = EISDIR;
        return -1;
    }

    uint64_t new_size = (uint64_t)size;
    const uint32_t ptrs_per_block = UFS_BLOCK_SIZE / sizeof(uint32_t);

    /* Number of blocks needed to hold `n` bytes, i.e. ceil(n / UFS_BLOCK_SIZE). */
    uint32_t old_blocks = (uint32_t)((inode.size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE);
    uint32_t new_blocks = (uint32_t)((new_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE);

    if (new_size < inode.size)
    {
        /* SHRINKING: free every block index in [new_blocks, old_blocks)
         * and clear the inode's pointer to each one. */
        for (uint32_t bi = new_blocks; bi < old_blocks; bi++)
        {
            uint32_t disk_block = 0;
            if (bi < 10)
            {
                disk_block = inode.direct_blocks[bi];
            }
            else if (inode.indirect_block != 0)
            {
                uint32_t ind_index = bi - 10;
                uint32_t ptrs[UFS_BLOCK_SIZE / sizeof(uint32_t)];
                if (disk_read_block(inode.indirect_block, ptrs) != 0)
                {
                    return -1;
                }
                disk_block = ptrs[ind_index];
            }

            if (disk_block == 0)
            {
                continue;
            }

            free_block(disk_block);

            if (bi < 10)
            {
                inode.direct_blocks[bi] = 0;
            }
            else if (inode.indirect_block != 0)
            {
                uint32_t ind_index = bi - 10;
                uint32_t ptrs[UFS_BLOCK_SIZE / sizeof(uint32_t)];
                if (disk_read_block(inode.indirect_block, ptrs) == 0)
                {
                    ptrs[ind_index] = 0;
                    disk_write_block(inode.indirect_block, ptrs);
                }
            }
        }

        /* If shrinking dropped back to direct-block range entirely
         * and the indirect block no longer points to anything real,
         * free the indirect block itself too. */
        if (inode.indirect_block != 0 && new_blocks <= 10)
        {
            uint32_t ptrs[UFS_BLOCK_SIZE / sizeof(uint32_t)];
            if (disk_read_block(inode.indirect_block, ptrs) == 0)
            {
                int any_used = 0;
                for (uint32_t i = 0; i < ptrs_per_block; i++)
                {
                    if (ptrs[i] != 0)
                    {
                        any_used = 1;
                        break;
                    }
                }
                if (!any_used)
                {
                    free_block(inode.indirect_block);
                    inode.indirect_block = 0;
                }
            }
        }
    }
    else if (new_size > inode.size)
    {
        /* GROWING.
         *
         * First: if the OLD size didn't end exactly on a block
         * boundary, the tail of that last block may hold stale
         * bytes left over from an earlier, larger write that was
         * since shrunk. Zero that tail explicitly so a later read
         * of the newly-grown region returns zeros, not leftovers. */
        uint32_t old_tail_off = (uint32_t)(inode.size % UFS_BLOCK_SIZE);
        if (inode.size > 0 && old_tail_off != 0)
        {
            uint32_t old_last_index = (uint32_t)((inode.size - 1) / UFS_BLOCK_SIZE);

            uint32_t disk_block = 0;
            if (old_last_index < 10)
            {
                disk_block = inode.direct_blocks[old_last_index];
            }
            else if (inode.indirect_block != 0)
            {
                uint32_t ind_index = old_last_index - 10;
                uint32_t ptrs[UFS_BLOCK_SIZE / sizeof(uint32_t)];
                if (disk_read_block(inode.indirect_block, ptrs) != 0)
                {
                    return -1;
                }
                disk_block = ptrs[ind_index];
            }

            if (disk_block != 0)
            {
                uint8_t block_buf[UFS_BLOCK_SIZE];
                if (disk_read_block(disk_block, block_buf) != 0)
                {
                    return -1;
                }
                memset(block_buf + old_tail_off, 0, UFS_BLOCK_SIZE - old_tail_off);
                if (disk_write_block(disk_block, block_buf) != 0)
                {
                    return -1;
                }
            }
        }

        /* Then: allocate and zero-fill every wholly-new block
         * needed to reach the new size. */
        uint8_t zeros[UFS_BLOCK_SIZE];
        memset(zeros, 0, sizeof(zeros));

        for (uint32_t bi = old_blocks; bi < new_blocks; bi++)
        {
            uint32_t disk_block = 0;
            if (bi < 10)
            {
                if (inode.direct_blocks[bi] == 0)
                {
                    int64_t nb = alloc_block();
                    if (nb < 0)
                    {
                        return -1;
                    }
                    inode.direct_blocks[bi] = (uint32_t)nb;
                }
                disk_block = inode.direct_blocks[bi];
            }
            else
            {
                uint32_t ind_index = bi - 10;
                if (ind_index >= ptrs_per_block)
                {
                    errno = EFBIG;
                    return -1;
                }
                if (inode.indirect_block == 0)
                {
                    int64_t nb = alloc_block();
                    if (nb < 0)
                    {
                        return -1;
                    }
                    inode.indirect_block = (uint32_t)nb;
                }
                uint32_t ptrs[UFS_BLOCK_SIZE / sizeof(uint32_t)];
                if (disk_read_block(inode.indirect_block, ptrs) != 0)
                {
                    return -1;
                }
                if (ptrs[ind_index] == 0)
                {
                    int64_t nb = alloc_block();
                    if (nb < 0)
                    {
                        return -1;
                    }
                    ptrs[ind_index] = (uint32_t)nb;
                    if (disk_write_block(inode.indirect_block, ptrs) != 0)
                    {
                        return -1;
                    }
                }
                disk_block = ptrs[ind_index];
            }

            if (disk_write_block(disk_block, zeros) != 0)
            {
                return -1;
            }
        }
    }

    inode.size = new_size;
    inode.modified_at = (int64_t)time(NULL);
    if (write_inode(inum, &inode) != 0)
    {
        return -1;
    }

    return 0;

}

int ufs_stat(const char *path, struct ufs_stat *st)
{
    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    uint32_t inum;
    if (resolve_path(path, &inum) != 0)
    {
        return -1;
    }

    struct ufs_inode inode;
    if (read_inode(inum, &inode) != 0)
    {
        return -1;
    }

    if (st != NULL)
    {
        st->type = (int)inode.type;
        st->size = (size_t)inode.size;
    }

    return 0;
}
