
#include "userfs.h"
#include "ufs_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


/* =========================================================
 * Global Filesystem State
 * ========================================================= */

/* Disk image currently mounted */
static FILE *disk = NULL;

/* Superblock loaded in RAM */
static struct ufs_superblock sb;

/* Bitmaps loaded in RAM */
static uint8_t *inode_bitmap;
static uint8_t *block_bitmap;

/* Currently opened files */
static struct ufs_open_file
    open_files[UFS_MAX_OPEN_FILES];

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
            (index % 8)) & 1u;
}


/*
 * Calculate how many bitmap blocks
 * are required to represent 'count' objects.
 */
static uint64_t bitmap_blocks_needed(uint64_t count)
{
    return
        (count + UFS_BITS_PER_BITMAP_BLOCK - 1)
        / UFS_BITS_PER_BITMAP_BLOCK;
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



int ufs_format(const char *image_path, size_t image_size)
{
    (void)image_path;
    (void)image_size;
    errno = ENOSYS;
    return -1;
}

int ufs_mount(const char *image_path)
{
    (void)image_path;
    errno = ENOSYS;
    return -1;
}

int ufs_unmount(void)
{
    errno = ENOSYS;
    return -1;
}

int ufs_mkdir(const char *path)
{
    (void)path;
    errno = ENOSYS;
    return -1;
}

int ufs_rmdir(const char *path)
{
    (void)path;
    errno = ENOSYS;
    return -1;
}

int ufs_listdir(const char *path, struct ufs_dirent *entries, size_t max_entries)
{
    (void)path;
    (void)entries;
    (void)max_entries;
    errno = ENOSYS;
    return -1;
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

int ufs_open(const char *path, int flags)
{
    (void)path;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int ufs_close(int fd)
{
    (void)fd;
    errno = ENOSYS;
    return -1;
}

ssize_t ufs_read(int fd, void *buf, size_t count)
{
    (void)fd;
    (void)buf;
    (void)count;
    errno = ENOSYS;
    return -1;
}

ssize_t ufs_write(int fd, const void *buf, size_t count)
{
    (void)fd;
    (void)buf;
    (void)count;
    errno = ENOSYS;
    return -1;
}

off_t ufs_seek(int fd, off_t offset, int whence)
{
    (void)fd;
    (void)offset;
    (void)whence;
    errno = ENOSYS;
    return -1;
}

int ufs_truncate(const char *path, size_t size)
{
    (void)path;
    (void)size;
    errno = ENOSYS;
    return -1;
}

int ufs_stat(const char *path, struct ufs_stat *st)
{
    (void)path;
    (void)st;
    errno = ENOSYS;
    return -1;
}
