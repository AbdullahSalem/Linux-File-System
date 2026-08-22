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

#ifndef EUCLEAN
#define EUCLEAN 117
#endif

void ufs_journal_start_txn(void);
int ufs_journal_add_block(uint32_t block_num, const uint8_t *data);
int ufs_journal_commit_txn(void);
void ufs_journal_abort_txn(void);

static uint32_t crc32_block(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
        {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }

    return ~crc;
}

static FILE *disk = NULL;
static struct ufs_superblock sb;
static struct ufs_open_file open_files[UFS_MAX_OPEN_FILES];
static uint8_t *inode_bitmap;
static uint8_t *block_bitmap;

#define MAX_TXN_BLOCKS 128

typedef struct
{
    uint32_t dest_blocks[MAX_TXN_BLOCKS];
    uint8_t block_data[MAX_TXN_BLOCKS][UFS_BLOCK_SIZE];
    uint32_t block_count;
    uint8_t is_active;
} ufs_journal_txn_t;

static ufs_journal_txn_t current_txn = {0};

static void bitmap_set(uint8_t *bitmap, uint64_t index)
{
    bitmap[index / 8] |= (uint8_t)(1u << (index % 8));
}

static void bitmap_clear(uint8_t *bitmap, uint64_t index)
{
    bitmap[index / 8] &= (uint8_t)~(1u << (index % 8));
}

static int bitmap_test(const uint8_t *bitmap,
                       uint64_t index)
{
    return (bitmap[index / 8] >>
            (index % 8)) &
           1u;
}

static uint64_t bitmap_blocks_needed(uint64_t count)
{
    return (count + UFS_BITS_PER_BITMAP_BLOCK - 1) / UFS_BITS_PER_BITMAP_BLOCK;
}

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

static int disk_read_block(uint64_t block_num, void *buf)
{
    size_t bytes_read;
    off_t offset;
    int seek_result;

    if (disk == NULL)
    {
        errno = EBADF;
        return -1;
    }

    offset = (off_t)(block_num * UFS_BLOCK_SIZE);

    seek_result = fseeko(disk, offset, SEEK_SET);

    if (seek_result != 0)
    {
        return -1;
    }

    bytes_read = fread(buf, 1, UFS_BLOCK_SIZE, disk);

    if (bytes_read != UFS_BLOCK_SIZE)
    {
        errno = EIO;
        return -1;
    }

    return 0;
}

static int disk_write_block(uint64_t block_num, const void *buf)
{
    size_t bytes_written;
    off_t offset;
    int seek_result;
    int flush_result;

    if (disk == NULL)
    {
        errno = EBADF;
        return -1;
    }

    offset = (off_t)(block_num * UFS_BLOCK_SIZE);

    seek_result = fseeko(disk, offset, SEEK_SET);

    if (seek_result != 0)
    {
        return -1;
    }

    bytes_written = fwrite(buf, 1, UFS_BLOCK_SIZE, disk);

    if (bytes_written != UFS_BLOCK_SIZE)
    {
        errno = EIO;
        return -1;
    }

    flush_result = fflush(disk);

    if (flush_result != 0)
    {
        return -1;
    }

    return 0;
}

static int flush_inode_bitmap(void)
{
    uint32_t i;
    uint64_t block_num;
    uint8_t *buffer;
    int write_result;

    for (i = 0; i < sb.inode_bitmap_blocks; i++)
    {
        block_num = sb.inode_bitmap_start + i;

        buffer = inode_bitmap + (size_t)i * UFS_BLOCK_SIZE;

        write_result = ufs_journal_add_block(block_num, buffer);

        if (write_result != 0)
        {
            return -1;
        }
    }

    return 0;
}

static int flush_block_bitmap(void)
{
    uint32_t i;
    uint64_t block_num;
    uint8_t *buffer;
    int write_result;

    for (i = 0; i < sb.block_bitmap_blocks; i++)
    {
        block_num = sb.block_bitmap_start + i;

        buffer = block_bitmap + (size_t)i * UFS_BLOCK_SIZE;

        write_result = ufs_journal_add_block(block_num, buffer);

        if (write_result != 0)
        {
            return -1;
        }
    }

    return 0;
}

#define UFS_INODES_PER_BLOCK \
    (UFS_BLOCK_SIZE / sizeof(struct ufs_inode))

static int read_inode(uint32_t inum, struct ufs_inode *out)
{
    uint64_t block;
    uint32_t offset;
    uint8_t buf[UFS_BLOCK_SIZE];
    int read_result;

    if (inum >= sb.total_inodes)
    {
        errno = EINVAL;
        return -1;
    }

    block = sb.inode_table_start +
            inum / UFS_INODES_PER_BLOCK;

    offset = (inum % UFS_INODES_PER_BLOCK) *
             sizeof(struct ufs_inode);

    read_result = disk_read_block(block, buf);

    if (read_result != 0)
    {
        return -1;
    }

    memcpy(out, buf + offset, sizeof(struct ufs_inode));

    return 0;
}

static int write_inode(uint32_t inum, const struct ufs_inode *in)
{
    uint64_t block;
    uint32_t offset;
    uint8_t buf[UFS_BLOCK_SIZE];
    int read_result;
    int write_result;

    if (inum >= sb.total_inodes)
    {
        errno = EINVAL;
        return -1;
    }

    block = sb.inode_table_start +
            inum / UFS_INODES_PER_BLOCK;

    offset = (inum % UFS_INODES_PER_BLOCK) *
             sizeof(struct ufs_inode);

    if (current_txn.is_active)
    {
        for (uint32_t i = 0; i < current_txn.block_count; i++)
        {
            if (current_txn.dest_blocks[i] == block)
            {
                memcpy(buf, current_txn.block_data[i], UFS_BLOCK_SIZE);
                goto apply_inode_write;
            }
        }
    }

    read_result = disk_read_block(block, buf);

    if (read_result != 0)
    {
        return -1;
    }

apply_inode_write:
    memcpy(buf + offset, in, sizeof(struct ufs_inode));

    write_result = ufs_journal_add_block(block, buf);

    if (write_result != 0)
    {
        return -1;
    }

    return 0;
}

static int64_t alloc_inode(void)
{
    int64_t inum;
    int flush_result;

    inum = bitmap_find_free(inode_bitmap, sb.total_inodes);

    if (inum < 0)
    {
        errno = ENOSPC;
        return -1;
    }

    bitmap_set(inode_bitmap, (uint64_t)inum);

    flush_result = flush_inode_bitmap();

    if (flush_result != 0)
    {
        bitmap_clear(inode_bitmap, (uint64_t)inum);
        return -1;
    }

    return inum;
}

static int free_inode(uint32_t inum)
{
    int flush_result;

    bitmap_clear(inode_bitmap, inum);

    flush_result = flush_inode_bitmap();

    if (flush_result != 0)
    {
        bitmap_set(inode_bitmap, inum);
        return -1;
    }

    return 0;
}

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

static void set_dir_entry(struct ufs_disk_dirent *entry,
                          const char *name,
                          uint32_t inode)
{
    entry->used = 1;
    entry->inode = inode;

    strncpy(entry->name, name, UFS_MAX_NAME);
    entry->name[UFS_MAX_NAME] = '\0';
}

static int dir_find_entry(const struct ufs_inode *dir,
                          const char *name,
                          uint32_t *out_inode)
{
    uint32_t block_index;
    uint32_t entry_index;

    uint8_t buffer[UFS_BLOCK_SIZE];

    struct ufs_disk_dirent *entries;

    int read_result;

    for (block_index = 0;
         block_index < dir->block_count && block_index < 10;
         block_index++)
    {
        if (dir->direct_blocks[block_index] == 0)
        {
            continue;
        }

        read_result = disk_read_block(
            dir->direct_blocks[block_index],
            buffer);

        if (read_result != 0)
        {
            return -1;
        }

        entries = (struct ufs_disk_dirent *)buffer;

        for (entry_index = 0;
             entry_index < UFS_DIRENTS_PER_BLOCK;
             entry_index++)
        {
            if (entries[entry_index].used == 0)
            {
                continue;
            }

            if (strcmp(entries[entry_index].name, name) != 0)
            {
                continue;
            }

            if (out_inode != NULL)
            {
                *out_inode = entries[entry_index].inode;
            }

            return 1;
        }
    }

    return 0;
}

static int dir_add_entry(uint32_t dir_inum,
                         struct ufs_inode *dir,
                         const char *name,
                         uint32_t child_inum)
{
    uint32_t block_index;
    uint32_t entry_index;

    uint8_t buffer[UFS_BLOCK_SIZE];

    struct ufs_disk_dirent *entries;

    int read_result;
    int write_result;

    int64_t new_block;

    for (block_index = 0;
         block_index < dir->block_count && block_index < 10;
         block_index++)
    {
        if (dir->direct_blocks[block_index] == 0)
        {
            continue;
        }

        read_result = disk_read_block(
            dir->direct_blocks[block_index],
            buffer);

        if (read_result != 0)
        {
            return -1;
        }

        entries = (struct ufs_disk_dirent *)buffer;

        for (entry_index = 0;
             entry_index < UFS_DIRENTS_PER_BLOCK;
             entry_index++)
        {
            if (entries[entry_index].used != 0)
            {
                continue;
            }

            set_dir_entry(
                &entries[entry_index],
                name,
                child_inum);

            write_result = disk_write_block(
                dir->direct_blocks[block_index],
                buffer);

            return write_result;
        }
    }

    if (dir->block_count >= 10)
    {
        errno = ENOSPC;
        return -1;
    }

    new_block = alloc_block();

    if (new_block < 0)
    {
        return -1;
    }

    memset(buffer, 0, sizeof(buffer));

    entries = (struct ufs_disk_dirent *)buffer;

    set_dir_entry(
        &entries[0],
        name,
        child_inum);

    write_result = disk_write_block(
        (uint64_t)new_block,
        buffer);

    if (write_result != 0)
    {
        free_block((uint64_t)new_block);
        return -1;
    }

    dir->direct_blocks[dir->block_count] = (uint32_t)new_block;
    dir->block_count++;

    write_result = write_inode(dir_inum, dir);

    if (write_result != 0)
    {
        return -1;
    }

    return 0;
}

static int dir_remove_entry(struct ufs_inode *dir,
                            const char *name)
{
    uint32_t block_index;
    uint32_t entry_index;

    uint8_t buffer[UFS_BLOCK_SIZE];

    struct ufs_disk_dirent *entries;

    int read_result;
    int write_result;

    for (block_index = 0;
         block_index < dir->block_count && block_index < 10;
         block_index++)
    {
        if (dir->direct_blocks[block_index] == 0)
        {
            continue;
        }

        read_result = disk_read_block(
            dir->direct_blocks[block_index],
            buffer);

        if (read_result != 0)
        {
            return -1;
        }

        entries = (struct ufs_disk_dirent *)buffer;

        for (entry_index = 0;
             entry_index < UFS_DIRENTS_PER_BLOCK;
             entry_index++)
        {
            if (entries[entry_index].used == 0)
            {
                continue;
            }

            if (strcmp(entries[entry_index].name, name) != 0)
            {
                continue;
            }

            memset(
                &entries[entry_index],
                0,
                sizeof(entries[entry_index]));

            write_result = disk_write_block(
                dir->direct_blocks[block_index],
                buffer);

            return write_result;
        }
    }

    errno = ENOENT;
    return -1;
}

static int dir_is_empty(const struct ufs_inode *dir)
{
    uint32_t block_index;
    uint32_t entry_index;

    uint8_t buffer[UFS_BLOCK_SIZE];

    struct ufs_disk_dirent *entries;

    int read_result;

    for (block_index = 0;
         block_index < dir->block_count && block_index < 10;
         block_index++)
    {
        if (dir->direct_blocks[block_index] == 0)
        {
            continue;
        }

        read_result = disk_read_block(
            dir->direct_blocks[block_index],
            buffer);

        if (read_result != 0)
        {
            return 0;
        }

        entries = (struct ufs_disk_dirent *)buffer;

        for (entry_index = 0;
             entry_index < UFS_DIRENTS_PER_BLOCK;
             entry_index++)
        {
            if (entries[entry_index].used != 0)
            {
                return 0;
            }
        }
    }

    return 1;
}

static int resolve_path(const char *path, uint32_t *out_inode)
{
    uint32_t current_inode;
    uint32_t next_inode;

    char copy[UFS_MAX_PATH + 1];
    char *token;
    char *save_ptr;

    struct ufs_inode current;

    int found;

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

    current_inode = sb.root_inode;

    if (strcmp(path, "/") == 0)
    {
        *out_inode = current_inode;
        return 0;
    }

    strcpy(copy, path);

    save_ptr = NULL;
    token = strtok_r(copy, "/", &save_ptr);

    while (token != NULL)
    {
        int read_result;

        read_result = read_inode(current_inode, &current);

        if (read_result != 0)
        {
            return -1;
        }

        if (current.type != UFS_TYPE_DIR)
        {
            errno = ENOTDIR;
            return -1;
        }

        found = dir_find_entry(
            &current,
            token,
            &next_inode);

        if (found < 0)
        {
            return -1;
        }

        if (found == 0)
        {
            errno = ENOENT;
            return -1;
        }

        current_inode = next_inode;

        token = strtok_r(NULL, "/", &save_ptr);
    }

    *out_inode = current_inode;

    return 0;
}

static int resolve_parent(const char *path,
                          uint32_t *parent_inode,
                          char *name_out)
{
    char copy[UFS_MAX_PATH + 1];

    char *slash;

    size_t length;

    if (path == NULL || path[0] != '/')
    {
        errno = EINVAL;
        return -1;
    }

    length = strlen(path);

    if (length == 0 || length > UFS_MAX_PATH)
    {
        errno = EINVAL;
        return -1;
    }

    if (strcmp(path, "/") == 0)
    {
        errno = EINVAL;
        return -1;
    }

    strcpy(copy, path);

    length = strlen(copy);

    if (length > 1 && copy[length - 1] == '/')
    {
        copy[length - 1] = '\0';
    }

    slash = strrchr(copy, '/');

    if (slash == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    strcpy(name_out, slash + 1);

    if (strlen(name_out) == 0 ||
        strlen(name_out) > UFS_MAX_NAME)
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (slash == copy)
    {
        *parent_inode = sb.root_inode;
        return 0;
    }

    *slash = '\0';

    return resolve_path(copy, parent_inode);
}
int64_t ufs_get_file_size(int fd)
{
    uint32_t inode_number;
    struct ufs_inode inode;

    int result;

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

    inode_number = open_files[fd].inode_number;

    result = read_inode(
        inode_number,
        &inode);

    if (result != 0)
    {
        return -1;
    }

    return (int64_t)inode.size;
}

// Filesystem API

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

    new_sb.journal_blocks = 100;
    new_sb.journal_start = new_sb.inode_table_start + new_sb.inode_table_blocks;

    new_sb.data_start = new_sb.journal_start + new_sb.journal_blocks;
    new_sb.data_blocks = total_blocks - new_sb.data_start;

    if (new_sb.data_start >= total_blocks)
    {
        fclose(f);
        unlink(image_path);
        errno = ENOSPC;
        return -1;
    }

    new_sb.data_blocks = total_blocks - new_sb.data_start;
    new_sb.root_inode = 0;

    uint8_t sb_block[UFS_BLOCK_SIZE];
    memset(sb_block, 0, sizeof(sb_block));
    memcpy(sb_block, &new_sb, sizeof(new_sb));
    fseeko(f, 0, SEEK_SET);
    fwrite(sb_block, 1, UFS_BLOCK_SIZE, f);

    size_t ibmp_bytes = (size_t)new_sb.inode_bitmap_blocks * UFS_BLOCK_SIZE;
    uint8_t *ibmp = calloc(1, ibmp_bytes);
    ibmp[0] |= 0x1;

    fseeko(f, (off_t)new_sb.inode_bitmap_start * UFS_BLOCK_SIZE, SEEK_SET);
    fwrite(ibmp, 1, ibmp_bytes, f);
    free(ibmp);

    size_t bbmp_bytes = (size_t)new_sb.block_bitmap_blocks * UFS_BLOCK_SIZE;
    uint8_t *bbmp = calloc(1, bbmp_bytes);
    for (uint64_t i = 0; i < new_sb.data_start; i++)
    {
        bbmp[i / 8] |= (uint8_t)(1u << (i % 8));
    }

    fseeko(f, (off_t)new_sb.block_bitmap_start * UFS_BLOCK_SIZE, SEEK_SET);
    fwrite(bbmp, 1, bbmp_bytes, f);
    free(bbmp);

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

    for (uint32_t i = 0; i < sb.journal_blocks; i += 2)
    {
        struct ufs_journal_record j_rec;

        disk_read_block(sb.journal_start + i + 1, &j_rec);

        if (j_rec.magic == UFS_JOURNAL_MAGIC && j_rec.is_commit == 1)
        {

            void *temp_buf = malloc(UFS_BLOCK_SIZE);
            if (temp_buf)
            {
                disk_read_block(sb.journal_start + i, temp_buf);

                disk_write_block(j_rec.target_block, temp_buf);

                free(temp_buf);
            }

            j_rec.magic = 0;
            j_rec.is_commit = 0;
            disk_write_block(sb.journal_start + i + 1, &j_rec);
        }
    }

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

    ufs_journal_start_txn();

    int64_t new_inum = alloc_inode();
    if (new_inum < 0)
    {
        ufs_journal_abort_txn();
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
        ufs_journal_abort_txn();
        return -1;
    }

    if (dir_add_entry(parent_inum, &parent, name, (uint32_t)new_inum) != 0)
    {
        int saved_errno = errno;
        new_dir.used = 0;
        write_inode((uint32_t)new_inum, &new_dir);
        free_inode((uint32_t)new_inum);
        errno = saved_errno;
        ufs_journal_abort_txn();
        return -1;
    }

    parent.modified_at = (int64_t)time(NULL);
    write_inode(parent_inum, &parent);

    return ufs_journal_commit_txn();
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

    ufs_journal_start_txn();

    if (dir_remove_entry(&parent, name) != 0)
    {
        ufs_journal_abort_txn();
        return -1;
    }

    parent.modified_at = (int64_t)time(NULL);
    write_inode(parent_inum, &parent);

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

    return ufs_journal_commit_txn();
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
    uint32_t parent_inum;
    uint32_t target_inum;

    struct ufs_inode parent;
    struct ufs_inode inode;

    char name[UFS_MAX_NAME + 1];

    int found;
    int64_t new_inum;
    int result;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    result = resolve_parent(
        path,
        &parent_inum,
        name);

    if (result != 0)
    {
        return -1;
    }

    result = read_inode(
        parent_inum,
        &parent);

    if (result != 0)
    {
        return -1;
    }

    if (parent.type != UFS_TYPE_DIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    found = dir_find_entry(
        &parent,
        name,
        &target_inum);

    if (found < 0)
    {
        return -1;
    }

    if (found == 1)
    {
        errno = EEXIST;
        return -1;
    }

    ufs_journal_start_txn();

    new_inum = alloc_inode();

    if (new_inum < 0)
    {
        ufs_journal_abort_txn();
        return -1;
    }

    target_inum = (uint32_t)new_inum;

    memset(&inode, 0, sizeof(inode));

    inode.used = 1;
    inode.type = UFS_TYPE_FILE;

    strncpy(
        inode.name,
        name,
        UFS_MAX_NAME);

    inode.name[UFS_MAX_NAME] = '\0';

    inode.size = 0;
    inode.parent_inode = (int32_t)parent_inum;
    inode.permissions = 0644;
    inode.created_at = (int64_t)time(NULL);
    inode.modified_at = inode.created_at;
    inode.block_count = 0;
    inode.flags = UFS_INODE_FLAG_INLINE_DATA;

    result = write_inode(
        target_inum,
        &inode);

    if (result != 0)
    {
        free_inode(target_inum);
        ufs_journal_abort_txn();
        return -1;
    }

    result = dir_add_entry(
        parent_inum,
        &parent,
        name,
        target_inum);

    if (result != 0)
    {
        free_inode(target_inum);
        ufs_journal_abort_txn();
        return -1;
    }

    parent.modified_at = (int64_t)time(NULL);

    result = write_inode(
        parent_inum,
        &parent);

    if (result != 0)
    {
        ufs_journal_abort_txn();
        return -1;
    }

    return ufs_journal_commit_txn();
}

int ufs_unlink(const char *path)
{
    uint32_t parent_inum;
    uint32_t target_inum;

    struct ufs_inode parent;
    struct ufs_inode target;

    char name[UFS_MAX_NAME + 1];

    int found;
    int result;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    result = resolve_parent(
        path,
        &parent_inum,
        name);

    if (result != 0)
    {
        return -1;
    }

    result = read_inode(
        parent_inum,
        &parent);

    if (result != 0)
    {
        return -1;
    }

    if (parent.type != UFS_TYPE_DIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    found = dir_find_entry(
        &parent,
        name,
        &target_inum);

    if (found < 0)
    {
        return -1;
    }

    if (found == 0)
    {
        errno = ENOENT;
        return -1;
    }

    result = read_inode(
        target_inum,
        &target);

    if (result != 0)
    {
        return -1;
    }

    if (target.type == UFS_TYPE_DIR)
    {
        errno = EISDIR;
        return -1;
    }

    ufs_journal_start_txn();

    result = dir_remove_entry(
        &parent,
        name);

    if (result != 0)
    {
        ufs_journal_abort_txn();
        return -1;
    }

    parent.modified_at = (int64_t)time(NULL);

    result = write_inode(
        parent_inum,
        &parent);

    if (result != 0)
    {
        ufs_journal_abort_txn();
        return -1;
    }

    target.flags |= UFS_INODE_FLAG_TRASHED;
    target.expiry_time = (int64_t)time(NULL) + UFS_TRASH_EXPIRY_SECONDS;

    result = write_inode(
        target_inum,
        &target);

    if (result != 0)
    {
        ufs_journal_abort_txn();
        return -1;
    }

    return ufs_journal_commit_txn();
}

// 4
int ufs_open(const char *path, int flags)
{
    uint32_t inode_number;
    int fd;
    int result;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    result = resolve_path(path, &inode_number);

    if (result != 0)
    {
        errno = ENOENT;
        return -1;
    }

    struct ufs_inode inode;
    if (read_inode(inode_number, &inode) != 0)
    {
        return -1;
    }

    if ((flags & (O_WRONLY | O_RDWR)) && !(inode.permissions & 0200))
    {
        errno = EACCES;
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
    int64_t file_size;

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
        file_size = ufs_get_file_size(fd);

        if (file_size < 0)
        {
            return -1;
        }

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

/* ---- Block-tree geometry ------------------------------------------- */

#define UFS_PTRS_PER_BLOCK (UFS_BLOCK_SIZE / sizeof(uint32_t)) /* 128 */
#define UFS_DIRECT_COUNT 10u

#define UFS_SINGLE_COUNT (UFS_PTRS_PER_BLOCK)                                           /* 128        */
#define UFS_DOUBLE_COUNT (UFS_PTRS_PER_BLOCK * UFS_PTRS_PER_BLOCK)                      /* 16 384     */
#define UFS_TRIPLE_COUNT (UFS_PTRS_PER_BLOCK * UFS_PTRS_PER_BLOCK * UFS_PTRS_PER_BLOCK) /* 2 097 152 */

#define UFS_SINGLE_START (UFS_DIRECT_COUNT)                          /* 10      */
#define UFS_DOUBLE_START (UFS_SINGLE_START + UFS_SINGLE_COUNT)       /* 138     */
#define UFS_TRIPLE_START (UFS_DOUBLE_START + UFS_DOUBLE_COUNT)       /* 16 522  */
#define UFS_MAX_LOGICAL_BLOCKS (UFS_TRIPLE_START + UFS_TRIPLE_COUNT) /* 2 113 674 */

/*
 * Resolves (and optionally allocates) the block number stored in *slot.
 * *slot may be a field inside a ufs_inode (indirect_block, ...) or an
 * entry inside a ptrs[] array read from an index block.
 *
 * Returns the block number, or 0 if unallocated (allocate == 0) or on
 * failure (errno is set to a non-zero value in the failure case only).
 */
static uint32_t ufs_bmap_slot(uint32_t *slot, int allocate)
{
    if (*slot != 0)
    {
        return *slot;
    }

    if (!allocate)
    {
        return 0;
    }

    int64_t nb = alloc_block();
    if (nb < 0)
    {
        return 0;
    }

    /* alloc_block() already zero-fills the block on disk. */
    *slot = (uint32_t)nb;
    return *slot;
}

/*
 * Translates a logical block number of a file into a physical block
 * number, walking direct / single / double / triple indirect trees.
 *
 * allocate == 0: pure lookup. Returns 0 for a hole (errno left at 0) or
 *                on error (errno set).
 * allocate == 1: allocates any missing intermediate index blocks and the
 *                final data block, updating `inode` and the on-disk index
 *                blocks as needed. Returns 0 only on failure (errno set).
 */
static uint32_t ufs_bmap(struct ufs_inode *inode, uint32_t logical_block, int allocate)
{
    errno = 0;

    if (logical_block >= UFS_MAX_LOGICAL_BLOCKS)
    {
        errno = EFBIG;
        return 0;
    }

    /* Direct blocks */
    if (logical_block < UFS_DIRECT_COUNT)
    {
        return ufs_bmap_slot(&inode->direct_blocks[logical_block], allocate);
    }

    /* Single indirect */
    if (logical_block < UFS_DOUBLE_START)
    {
        uint32_t idx = logical_block - UFS_SINGLE_START;

        uint32_t ind_blk = ufs_bmap_slot(&inode->indirect_block, allocate);
        if (ind_blk == 0)
        {
            return 0;
        }

        uint32_t ptrs[UFS_PTRS_PER_BLOCK];
        if (disk_read_block(ind_blk, ptrs) != 0)
        {
            return 0;
        }

        uint32_t data_blk = ufs_bmap_slot(&ptrs[idx], allocate);
        if (data_blk == 0)
        {
            return 0;
        }

        if (allocate && disk_write_block(ind_blk, ptrs) != 0)
        {
            return 0;
        }

        return data_blk;
    }

    /* Double indirect */
    if (logical_block < UFS_TRIPLE_START)
    {
        uint32_t rel = logical_block - UFS_DOUBLE_START;
        uint32_t l1_idx = rel / UFS_PTRS_PER_BLOCK;
        uint32_t l2_idx = rel % UFS_PTRS_PER_BLOCK;

        uint32_t dbl_blk = ufs_bmap_slot(&inode->double_indirect_block, allocate);
        if (dbl_blk == 0)
        {
            return 0;
        }

        uint32_t l1_ptrs[UFS_PTRS_PER_BLOCK];
        if (disk_read_block(dbl_blk, l1_ptrs) != 0)
        {
            return 0;
        }

        uint32_t l1_blk = ufs_bmap_slot(&l1_ptrs[l1_idx], allocate);
        if (l1_blk == 0)
        {
            return 0;
        }
        if (allocate && disk_write_block(dbl_blk, l1_ptrs) != 0)
        {
            return 0;
        }

        uint32_t l2_ptrs[UFS_PTRS_PER_BLOCK];
        if (disk_read_block(l1_blk, l2_ptrs) != 0)
        {
            return 0;
        }

        uint32_t data_blk = ufs_bmap_slot(&l2_ptrs[l2_idx], allocate);
        if (data_blk == 0)
        {
            return 0;
        }
        if (allocate && disk_write_block(l1_blk, l2_ptrs) != 0)
        {
            return 0;
        }

        return data_blk;
    }

    /* Triple indirect */
    {
        uint32_t rel = logical_block - UFS_TRIPLE_START;
        uint32_t l1_idx = rel / UFS_DOUBLE_COUNT;
        uint32_t rem = rel % UFS_DOUBLE_COUNT;
        uint32_t l2_idx = rem / UFS_PTRS_PER_BLOCK;
        uint32_t l3_idx = rem % UFS_PTRS_PER_BLOCK;

        uint32_t tpl_blk = ufs_bmap_slot(&inode->triple_indirect_block, allocate);
        if (tpl_blk == 0)
        {
            return 0;
        }

        uint32_t l1_ptrs[UFS_PTRS_PER_BLOCK];
        if (disk_read_block(tpl_blk, l1_ptrs) != 0)
        {
            return 0;
        }

        uint32_t l1_blk = ufs_bmap_slot(&l1_ptrs[l1_idx], allocate);
        if (l1_blk == 0)
        {
            return 0;
        }
        if (allocate && disk_write_block(tpl_blk, l1_ptrs) != 0)
        {
            return 0;
        }

        uint32_t l2_ptrs[UFS_PTRS_PER_BLOCK];
        if (disk_read_block(l1_blk, l2_ptrs) != 0)
        {
            return 0;
        }

        uint32_t l2_blk = ufs_bmap_slot(&l2_ptrs[l2_idx], allocate);
        if (l2_blk == 0)
        {
            return 0;
        }
        if (allocate && disk_write_block(l1_blk, l2_ptrs) != 0)
        {
            return 0;
        }

        uint32_t l3_ptrs[UFS_PTRS_PER_BLOCK];
        if (disk_read_block(l2_blk, l3_ptrs) != 0)
        {
            return 0;
        }

        uint32_t data_blk = ufs_bmap_slot(&l3_ptrs[l3_idx], allocate);
        if (data_blk == 0)
        {
            return 0;
        }
        if (allocate && disk_write_block(l2_blk, l3_ptrs) != 0)
        {
            return 0;
        }

        return data_blk;
    }
}

/* ---- Freeing helpers -------------------------------------------------
 *
 * Frees every allocated block whose logical block number falls in
 * [free_lo, free_hi), inside the subtree rooted at *block_num_ptr.
 * The subtree covers the logical range [subtree_base, subtree_base +
 * subtree_span). child_span == 1 means *block_num_ptr's pointers refer
 * directly to data blocks (i.e. it is a single-indirect block);
 * otherwise each pointer refers to another index block one level down.
 *
 * Data (and emptied index) blocks are zeroed before being freed, and
 * the subtree's own block is freed and *block_num_ptr cleared to 0 once
 * every pointer inside it has been freed. This lets the same routine
 * both truncate a suffix of a file and delete a tree in full (by
 * passing the tree's whole range as [free_lo, free_hi)).
 */
static void ufs_free_range(uint32_t *block_num_ptr,
                           uint64_t subtree_base, uint64_t subtree_span,
                           uint64_t free_lo, uint64_t free_hi)
{
    if (*block_num_ptr == 0)
    {
        return;
    }

    if (free_hi <= subtree_base || free_lo >= subtree_base + subtree_span)
    {
        return;
    }

    uint32_t ptrs[UFS_PTRS_PER_BLOCK];
    if (disk_read_block(*block_num_ptr, ptrs) != 0)
    {
        return;
    }

    uint8_t zeros[UFS_BLOCK_SIZE];
    memset(zeros, 0, sizeof(zeros));

    uint64_t child_span = subtree_span / UFS_PTRS_PER_BLOCK;
    int dirty = 0;
    int any_remaining = 0;

    for (uint32_t i = 0; i < UFS_PTRS_PER_BLOCK; i++)
    {
        uint64_t child_base = subtree_base + (uint64_t)i * child_span;
        uint64_t child_end = child_base + child_span;

        if (free_hi > child_base && free_lo < child_end)
        {
            if (child_span == 1)
            {
                if (ptrs[i] != 0)
                {
                    disk_write_block(ptrs[i], zeros);
                    free_block(ptrs[i]);
                    ptrs[i] = 0;
                    dirty = 1;
                }
            }
            else
            {
                ufs_free_range(&ptrs[i], child_base, child_span, free_lo, free_hi);
                dirty = 1;
            }
        }

        if (ptrs[i] != 0)
        {
            any_remaining = 1;
        }
    }

    if (dirty)
    {
        disk_write_block(*block_num_ptr, ptrs);
    }

    if (!any_remaining)
    {
        disk_write_block(*block_num_ptr, zeros);
        free_block(*block_num_ptr);
        *block_num_ptr = 0;
    }
}

/* Thin, explicit wrappers over ufs_free_range() that free an entire
 * single / double / triple indirect tree, as requested by
 * purge_trashed_inode(). */
static void free_indirect_block(uint32_t *indirect_block_ptr)
{
    ufs_free_range(indirect_block_ptr,
                   UFS_SINGLE_START, UFS_SINGLE_COUNT,
                   UFS_SINGLE_START, UFS_SINGLE_START + UFS_SINGLE_COUNT);
}

static void free_double_indirect_block(uint32_t *double_block_ptr)
{
    ufs_free_range(double_block_ptr,
                   UFS_DOUBLE_START, UFS_DOUBLE_COUNT,
                   UFS_DOUBLE_START, UFS_DOUBLE_START + UFS_DOUBLE_COUNT);
}

static void free_triple_indirect_block(uint32_t *triple_block_ptr)
{
    ufs_free_range(triple_block_ptr,
                   UFS_TRIPLE_START, UFS_TRIPLE_COUNT,
                   UFS_TRIPLE_START, UFS_TRIPLE_START + UFS_TRIPLE_COUNT);
}

/* ---- fsck tree walker --------------------------------------------------
 * Marks block_num itself in shadow_bbmp, then walks its 128 pointers.
 * depth == 1: block_num is a single-indirect block -> pointers are data
 *             blocks, marked directly.
 * depth == 2: block_num is a double-indirect block -> pointers are
 *             single-indirect blocks, recursed with depth 1.
 * depth == 3: block_num is a triple-indirect block -> pointers are
 *             double-indirect blocks, recursed with depth 2.
 */
static void fsck_mark_index_tree(uint32_t block_num, int depth, uint8_t *shadow_bbmp)
{
    if (block_num < sb.data_start || block_num >= sb.total_blocks)
    {
        return;
    }

    shadow_bbmp[block_num / 8] |= (uint8_t)(1u << (block_num % 8));

    uint32_t ptrs[UFS_PTRS_PER_BLOCK];
    if (disk_read_block(block_num, ptrs) != 0)
    {
        return;
    }

    for (uint32_t i = 0; i < UFS_PTRS_PER_BLOCK; i++)
    {
        uint32_t child = ptrs[i];

        if (child < sb.data_start || child >= sb.total_blocks)
        {
            continue;
        }

        if (depth == 1)
        {
            shadow_bbmp[child / 8] |= (uint8_t)(1u << (child % 8));
        }
        else
        {
            fsck_mark_index_tree(child, depth - 1, shadow_bbmp);
        }
    }
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

    if ((uint64_t)offset >= inode.size)
    {
        return 0;
    }

    uint64_t available = inode.size - (uint64_t)offset;
    size_t to_read = (count < available) ? count : (size_t)available;

    if (inode.flags & UFS_INODE_FLAG_INLINE_DATA)
    {
        memcpy(buf, (const uint8_t *)inode.direct_blocks + offset, to_read);
        open_files[fd].position += (off_t)to_read;
        return (ssize_t)to_read;
    }

    size_t bytes_done = 0;
    uint8_t block_buf[UFS_BLOCK_SIZE];

    while (bytes_done < to_read)
    {
        uint64_t cur = (uint64_t)offset + bytes_done;
        uint32_t block_index = (uint32_t)(cur / UFS_BLOCK_SIZE);
        uint32_t in_block_off = (uint32_t)(cur % UFS_BLOCK_SIZE);

        uint32_t disk_block = ufs_bmap(&inode, block_index, 0);

        if (disk_block == 0)
        {
            if (errno != 0)
            {
                return -1;
            }

            memset(block_buf, 0, UFS_BLOCK_SIZE);
        }
        else if (disk_read_block(disk_block, block_buf) != 0)
        {
            return -1;
        }
        else if (block_index < 10 && inode.block_checksums[block_index] != 0)
        {
            uint32_t computed = crc32_block(block_buf, UFS_BLOCK_SIZE);

            if (computed != inode.block_checksums[block_index])
            {
                errno = EUCLEAN;
                return -1;
            }
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

        start_offset = (off_t)inode.size;
    }

    if (inode.flags & UFS_INODE_FLAG_INLINE_DATA)
    {
        uint64_t new_end = (uint64_t)start_offset + count;

        if (new_end <= UFS_INLINE_DATA_MAX_SIZE)
        {
            memcpy((uint8_t *)inode.direct_blocks + start_offset, buf, count);

            if (new_end > inode.size)
            {
                inode.size = new_end;
            }

            inode.modified_at = (int64_t)time(NULL);

            if (write_inode(inum, &inode) != 0)
            {
                return -1;
            }

            open_files[fd].position = start_offset + (off_t)count;
            return (ssize_t)count;
        }

        int64_t nb = alloc_block();
        if (nb < 0)
        {
            return -1;
        }

        uint8_t evict_block[UFS_BLOCK_SIZE];
        memset(evict_block, 0, sizeof(evict_block));
        memcpy(evict_block, inode.direct_blocks, UFS_INLINE_DATA_MAX_SIZE);

        if (disk_write_block((uint64_t)nb, evict_block) != 0)
        {
            free_block((uint64_t)nb);
            return -1;
        }

        inode.block_checksums[0] = crc32_block(evict_block, UFS_BLOCK_SIZE);

        memset(inode.direct_blocks, 0, sizeof(inode.direct_blocks));
        inode.direct_blocks[0] = (uint32_t)nb;
        inode.flags &= ~UFS_INODE_FLAG_INLINE_DATA;
    }

    size_t bytes_done = 0;
    uint8_t block_buf[UFS_BLOCK_SIZE];

    while (bytes_done < count)
    {
        uint64_t cur = (uint64_t)start_offset + bytes_done;
        uint32_t block_index = (uint32_t)(cur / UFS_BLOCK_SIZE);
        uint32_t in_block_off = (uint32_t)(cur % UFS_BLOCK_SIZE);

        uint32_t disk_block = ufs_bmap(&inode, block_index, 1);
        if (disk_block == 0)
        {
            break;
        }

        size_t chunk = UFS_BLOCK_SIZE - in_block_off;
        size_t remaining = count - bytes_done;
        if (chunk > remaining)
        {
            chunk = remaining;
        }

        if (chunk < UFS_BLOCK_SIZE)
        {
            if (disk_read_block(disk_block, block_buf) != 0)
            {
                break;
            }
        }

        memcpy(block_buf + in_block_off, (const uint8_t *)buf + bytes_done, chunk);

        if (block_index < 10)
        {
            inode.block_checksums[block_index] = crc32_block(block_buf, UFS_BLOCK_SIZE);
        }

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

    if (bytes_done > 0)
    {
        inode.modified_at = (int64_t)time(NULL);
    }

    if (write_inode(inum, &inode) != 0)
    {
        return -1;
    }

    open_files[fd].position =
        start_offset + (off_t)bytes_done;

    if (bytes_done == 0 && count > 0)
    {
        return -1;
    }

    return (ssize_t)bytes_done;
}

void ufs_journal_start_txn(void)
{
    current_txn.block_count = 0;
    current_txn.is_active = 1;
}

int ufs_journal_add_block(uint32_t block_num, const uint8_t *data)
{
    if (!current_txn.is_active)
    {
        return disk_write_block(block_num, data);
    }

    for (uint32_t i = 0; i < current_txn.block_count; i++)
    {
        if (current_txn.dest_blocks[i] == block_num)
        {
            memcpy(current_txn.block_data[i], data, UFS_BLOCK_SIZE);
            return 0;
        }
    }

    if (current_txn.block_count >= MAX_TXN_BLOCKS)
    {
        return -1;
    }

    current_txn.dest_blocks[current_txn.block_count] = block_num;
    memcpy(current_txn.block_data[current_txn.block_count], data, UFS_BLOCK_SIZE);
    current_txn.block_count++;
    return 0;
}

int ufs_journal_commit_txn(void)
{
    if (!current_txn.is_active || current_txn.block_count == 0)
    {
        current_txn.is_active = 0;
        return 0;
    }

    uint32_t j_idx = 0;
    for (uint32_t i = 0; i < current_txn.block_count; i++)
    {
        if (j_idx + 1 >= sb.journal_blocks)
        {
            break;
        }

        struct ufs_journal_record j_rec;
        memset(&j_rec, 0, sizeof(j_rec));
        j_rec.magic = UFS_JOURNAL_MAGIC;
        j_rec.is_commit = 1;
        j_rec.target_block = current_txn.dest_blocks[i];

        if (disk_write_block(sb.journal_start + j_idx, current_txn.block_data[i]) != 0)
        {
            return -1;
        }
        if (disk_write_block(sb.journal_start + j_idx + 1, &j_rec) != 0)
        {
            return -1;
        }

        j_idx += 2;
    }

    for (uint32_t i = 0; i < current_txn.block_count; i++)
    {
        if (disk_write_block(current_txn.dest_blocks[i], current_txn.block_data[i]) != 0)
        {
            return -1;
        }
    }

    j_idx = 0;
    for (uint32_t i = 0; i < current_txn.block_count; i++)
    {
        if (j_idx + 1 >= sb.journal_blocks)
        {
            break;
        }

        struct ufs_journal_record j_rec;
        memset(&j_rec, 0, sizeof(j_rec));
        disk_write_block(sb.journal_start + j_idx + 1, &j_rec);

        j_idx += 2;
    }

    current_txn.is_active = 0;
    current_txn.block_count = 0;
    return 0;
}

void ufs_journal_abort_txn(void)
{
    current_txn.is_active = 0;
    current_txn.block_count = 0;
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

    ufs_journal_start_txn();

    if (inode.flags & UFS_INODE_FLAG_INLINE_DATA)
    {
        if (new_size <= UFS_INLINE_DATA_MAX_SIZE)
        {
            if (new_size > inode.size)
            {
                memset((uint8_t *)inode.direct_blocks + inode.size, 0, new_size - inode.size);
            }
            else
            {
                memset((uint8_t *)inode.direct_blocks + new_size, 0, UFS_INLINE_DATA_MAX_SIZE - new_size);
            }

            inode.size = new_size;
            inode.modified_at = (int64_t)time(NULL);

            if (write_inode(inum, &inode) != 0)
            {
                ufs_journal_abort_txn();
                return -1;
            }

            return ufs_journal_commit_txn();
        }

        uint8_t evict_block[UFS_BLOCK_SIZE];
        memset(evict_block, 0, sizeof(evict_block));
        memcpy(evict_block, inode.direct_blocks, UFS_INLINE_DATA_MAX_SIZE);

        int64_t nb = alloc_block();
        if (nb < 0)
        {
            ufs_journal_abort_txn();
            return -1;
        }

        if (ufs_journal_add_block((uint32_t)nb, evict_block) != 0)
        {
            free_block((uint64_t)nb);
            ufs_journal_abort_txn();
            return -1;
        }

        memset(inode.direct_blocks, 0, sizeof(inode.direct_blocks));
        inode.direct_blocks[0] = (uint32_t)nb;
        inode.block_checksums[0] = crc32_block(evict_block, UFS_BLOCK_SIZE);
        inode.flags &= ~UFS_INODE_FLAG_INLINE_DATA;
    }

    uint32_t old_blocks = (uint32_t)((inode.size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE);
    uint32_t new_blocks = (uint32_t)((new_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE);

    if (new_size < inode.size)
    {
        uint32_t offset_in_block = (uint32_t)(new_size % UFS_BLOCK_SIZE);
        if (offset_in_block != 0)
        {
            uint32_t last_block_idx = (uint32_t)(new_size / UFS_BLOCK_SIZE);
            uint32_t disk_block = ufs_bmap(&inode, last_block_idx, 0);

            if (disk_block != 0)
            {
                uint8_t block_buf[UFS_BLOCK_SIZE];
                if (disk_read_block(disk_block, block_buf) == 0)
                {
                    memset(block_buf + offset_in_block, 0, UFS_BLOCK_SIZE - offset_in_block);
                    if (ufs_journal_add_block(disk_block, block_buf) == 0)
                    {
                        if (last_block_idx < 10)
                        {
                            inode.block_checksums[last_block_idx] = crc32_block(block_buf, UFS_BLOCK_SIZE);
                        }
                    }
                }
            }
        }

        uint64_t free_lo = new_blocks;
        uint64_t free_hi = old_blocks;

        for (uint32_t bi = new_blocks; bi < old_blocks && bi < UFS_DIRECT_COUNT; bi++)
        {
            if (inode.direct_blocks[bi] != 0)
            {
                free_block(inode.direct_blocks[bi]);
                inode.direct_blocks[bi] = 0;
                inode.block_checksums[bi] = 0;
            }
        }

        ufs_free_range(&inode.indirect_block, UFS_SINGLE_START, UFS_SINGLE_COUNT, free_lo, free_hi);
        ufs_free_range(&inode.double_indirect_block, UFS_DOUBLE_START, UFS_DOUBLE_COUNT, free_lo, free_hi);
        ufs_free_range(&inode.triple_indirect_block, UFS_TRIPLE_START, UFS_TRIPLE_COUNT, free_lo, free_hi);
    }
    else if (new_size > inode.size)
    {
        uint32_t old_tail_off = (uint32_t)(inode.size % UFS_BLOCK_SIZE);
        if (inode.size > 0 && old_tail_off != 0)
        {
            uint32_t old_last_index = (uint32_t)((inode.size - 1) / UFS_BLOCK_SIZE);
            uint32_t disk_block = ufs_bmap(&inode, old_last_index, 0);

            if (disk_block == 0 && errno != 0)
            {
                ufs_journal_abort_txn();
                return -1;
            }

            if (disk_block != 0)
            {
                uint8_t block_buf[UFS_BLOCK_SIZE];
                if (disk_read_block(disk_block, block_buf) != 0)
                {
                    ufs_journal_abort_txn();
                    return -1;
                }
                memset(block_buf + old_tail_off, 0, UFS_BLOCK_SIZE - old_tail_off);

                if (ufs_journal_add_block(disk_block, block_buf) != 0)
                {
                    ufs_journal_abort_txn();
                    return -1;
                }
                if (old_last_index < 10)
                {
                    inode.block_checksums[old_last_index] = crc32_block(block_buf, UFS_BLOCK_SIZE);
                }
            }
        }

        uint8_t zeros[UFS_BLOCK_SIZE];
        memset(zeros, 0, sizeof(zeros));

        for (uint32_t bi = old_blocks; bi < new_blocks; bi++)
        {
            uint32_t disk_block = ufs_bmap(&inode, bi, 1);
            if (disk_block == 0)
            {
                ufs_journal_abort_txn();
                return -1;
            }

            if (ufs_journal_add_block(disk_block, zeros) != 0)
            {
                ufs_journal_abort_txn();
                return -1;
            }

            if (bi < 10)
            {
                inode.block_checksums[bi] = crc32_block(zeros, UFS_BLOCK_SIZE);
            }
        }
    }

    inode.size = new_size;
    inode.modified_at = (int64_t)time(NULL);
    if (write_inode(inum, &inode) != 0)
    {
        ufs_journal_abort_txn();
        return -1;
    }

    return ufs_journal_commit_txn();
}

int ufs_find_by_tag(const char *tag, uint32_t *matching_inums, size_t max_results)
{
    if (disk == NULL || tag == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    size_t found = 0;

    for (uint32_t i = 0; i < sb.total_inodes; i++)
    {
        struct ufs_inode ino;
        if (read_inode(i, &ino) != 0)
        {
            continue;
        }

        if (ino.used != 1)
        {
            continue;
        }

        char tag_buf[sizeof(ino.tags) + 1];
        memcpy(tag_buf, ino.tags, sizeof(ino.tags));
        tag_buf[sizeof(ino.tags)] = '\0';

        if (strcmp(tag_buf, tag) == 0)
        {
            if (found < max_results)
            {
                matching_inums[found] = i;
            }
            found++;
        }
    }

    return (int)found;
}

int ufs_set_tag(const char *path, const char *tag)
{
    if (disk == NULL || tag == NULL)
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

    if (strlen(tag) >= sizeof(inode.tags))
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    memset(inode.tags, 0, sizeof(inode.tags));
    memcpy(inode.tags, tag, strlen(tag));

    return write_inode(inum, &inode);
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

static int purge_trashed_inode(uint32_t inum, struct ufs_inode *ino)
{
    if (ino->flags & UFS_INODE_FLAG_INLINE_DATA)
    {
        memset(ino, 0, sizeof(*ino));

        if (write_inode(inum, ino) != 0)
        {
            return -1;
        }

        return free_inode(inum);
    }

    uint8_t zeros[UFS_BLOCK_SIZE];
    memset(zeros, 0, sizeof(zeros));

    for (int i = 0; i < 10; i++)
    {
        if (ino->direct_blocks[i] != 0)
        {
            disk_write_block(ino->direct_blocks[i], zeros);
            free_block(ino->direct_blocks[i]);
            ino->direct_blocks[i] = 0;
        }
    }

    free_indirect_block(&ino->indirect_block);
    free_double_indirect_block(&ino->double_indirect_block);
    free_triple_indirect_block(&ino->triple_indirect_block);

    memset(ino, 0, sizeof(*ino));

    if (write_inode(inum, ino) != 0)
    {
        return -1;
    }

    return free_inode(inum);
}

int ufs_fsck(void)
{
    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (sb.magic != UFS_MAGIC || sb.total_inodes == 0 || sb.total_blocks == 0)
    {
        fprintf(stderr, "fsck: Fatal error - Superblock is corrupted.\n");
        return -1;
    }

    int64_t now = (int64_t)time(NULL);
    uint32_t purged_count = 0;

    for (uint32_t i = 0; i < sb.total_inodes; i++)
    {
        struct ufs_inode ino;
        if (read_inode(i, &ino) != 0)
            continue;

        if (ino.used == 1 &&
            (ino.flags & UFS_INODE_FLAG_TRASHED) &&
            ino.expiry_time != 0 &&
            ino.expiry_time <= now)
        {
            if (purge_trashed_inode(i, &ino) == 0)
            {
                purged_count++;
            }
        }
    }

    if (purged_count > 0)
    {
        printf("fsck: Purged %u expired file(s) from recycle bin.\n", purged_count);
    }

    size_t bbmp_bytes = (size_t)sb.block_bitmap_blocks * UFS_BLOCK_SIZE;
    uint8_t *shadow_bbmp = calloc(1, bbmp_bytes);
    if (shadow_bbmp == NULL)
    {
        errno = ENOMEM;
        return -1;
    }

    for (uint64_t i = 0; i < sb.data_start; i++)
    {
        shadow_bbmp[i / 8] |= (uint8_t)(1u << (i % 8));
    }

    for (uint32_t i = 0; i < sb.total_inodes; i++)
    {
        struct ufs_inode ino;
        if (read_inode(i, &ino) != 0)
            continue;

        if (ino.used == 1 && !(ino.flags & UFS_INODE_FLAG_INLINE_DATA))
        {
            /* Check direct blocks */
            for (int b = 0; b < 10; b++)
            {
                uint32_t blk = ino.direct_blocks[b];
                if (blk >= sb.data_start && blk < sb.total_blocks)
                {
                    shadow_bbmp[blk / 8] |= (uint8_t)(1u << (blk % 8));
                }
            }

            /* Check single / double / triple indirect trees */
            fsck_mark_index_tree(ino.indirect_block, 1, shadow_bbmp);
            fsck_mark_index_tree(ino.double_indirect_block, 2, shadow_bbmp);
            fsck_mark_index_tree(ino.triple_indirect_block, 3, shadow_bbmp);
        }
    }

    int is_corrupted = 0;
    for (size_t i = 0; i < bbmp_bytes; i++)
    {
        if (block_bitmap[i] != shadow_bbmp[i])
        {
            block_bitmap[i] = shadow_bbmp[i];
            is_corrupted = 1;
        }
    }

    if (is_corrupted)
    {
        if (flush_block_bitmap() != 0)
        {
            free(shadow_bbmp);
            return -1;
        }
        printf("fsck: File system repaired successfully (Orphaned blocks recovered).\n");
    }
    else
    {
        printf("fsck: File system is clean. No errors found.\n");
    }

    free(shadow_bbmp);
    return 0;
}
