# UserFS — Design Document

## 1. Overview

UserFS is a block-based filesystem implemented entirely as a user-space C
library (`userfs.c`). It stores all filesystem state — superblock,
allocation bitmaps, inodes, directory entries, and file data — inside a
single flat disk image file. No part of a UserFS file or directory is ever
represented as a separate host-filesystem file; the image is treated as a
raw block device accessed via `fseeko`/`fread`/`fwrite` in fixed
`UFS_BLOCK_SIZE`-byte units.

The filesystem provides APIs for filesystem management, directories, files,
file descriptors, data access, metadata, tags, and consistency checking.

Fixed parameters (from `userfs.h`):

| Constant | Value | Meaning |
|---|---:|---|
| `UFS_BLOCK_SIZE` | 512 bytes | Size of one disk block |
| `UFS_MAX_NAME` | 31 | Max characters in a file/dir name |
| `UFS_MAX_PATH` | 255 | Max characters in a full path |
| `UFS_MAX_OPEN_FILES` | 32 | Max simultaneously-open file descriptors |

---

## 2. On-Disk Layout

The image is divided into contiguous regions, laid out in this fixed order
starting at block 0:

```text
Block 0                 Superblock (1 block, 512 bytes, zero-padded)
Block 1 ..              Inode bitmap  (inode_bitmap_blocks blocks)
Block ..                Block bitmap  (block_bitmap_blocks blocks)
Block ..                Inode table   (inode_table_blocks blocks)
Block data_start ..     Data blocks   (data_blocks blocks, to end of image)
```

Every region's start offset and length is computed once, at `ufs_format()`
time, and recorded in the superblock so that `ufs_mount()` can load the
filesystem geometry from the image.

Region sizing, given `total_blocks = image_size / UFS_BLOCK_SIZE`:

- `total_inodes = max(16, total_blocks / 4)`
- `inode_bitmap_blocks = ceil(total_inodes / (UFS_BLOCK_SIZE * 8))`
- `block_bitmap_blocks = ceil(total_blocks / (UFS_BLOCK_SIZE * 8))`
- `inode_table_blocks = ceil(total_inodes * sizeof(ufs_inode) / UFS_BLOCK_SIZE)`
- `data_start = inode_table_start + inode_table_blocks`

Images smaller than 32 blocks are rejected, and formatting fails if no
data blocks remain after the metadata regions.

### 2.1 Superblock

The superblock stores the filesystem geometry, block size, total blocks,
total inodes, metadata-region locations, data-region information, and the
root inode number.

### 2.2 Bitmaps

The inode and block bitmaps track allocated inodes and blocks.

At format time, metadata blocks are marked as used in the block bitmap and
the root inode is marked as used in the inode bitmap.

### 2.3 Inodes

Each inode stores information about a file or directory, including:

- Type and name
- Size
- Parent inode
- Permissions and timestamps
- Direct data-block pointers
- A single-indirect block pointer
- Reserved double- and triple-indirect pointers

The current implementation supports 10 direct blocks and one single-indirect
block.

### 2.4 Directories

Directory contents are stored in data blocks as directory entries.

Directories use direct blocks only and can contain up to 120 entries.
Directory lookup is performed by scanning the directory entries.

---

## 3. Allocation Strategy

Inodes and data blocks use a simple first-free-bit allocation strategy.

Allocated blocks are zero-filled before being used for file data.

Indirect blocks are allocated when a file grows beyond its direct-block
capacity.

---

## 4. Path Resolution

`resolve_path()` resolves absolute paths component by component, while
`resolve_parent()` locates the parent directory and final component used by
creation and deletion operations.

Paths are limited by `UFS_MAX_PATH`.

---

## 5. Open Files, Read/Write, Seek, Truncate

The filesystem maintains an in-memory table of up to
`UFS_MAX_OPEN_FILES` open file descriptors.

- `ufs_open()` creates a file descriptor and records its current position.
- `ufs_read()` reads file data from the current position.
- `ufs_write()` writes data and allocates blocks when required.
- `ufs_seek()` supports `SEEK_SET`, `SEEK_CUR`, and `SEEK_END`.
- `ufs_truncate()` changes the logical size of a file and manages the
  required data blocks.
- `ufs_close()` releases an open file descriptor.

Open file information is runtime state and is not stored in the disk image.

---

## 6. File Metadata and Tags

`ufs_stat()` provides basic information about a filesystem object,
including its type and size.

UserFS also supports file tags through:

```c
int ufs_set_tag(const char *path, const char *tag);
int ufs_find_by_tag(const char *tag,
                    uint32_t *matching_inums,
                    size_t max_results);
```

`ufs_set_tag()` associates a tag with a filesystem object identified by
its path.

`ufs_find_by_tag()` searches for objects associated with a specified tag
and returns their inode numbers, up to the supplied result limit.

The tag functionality provides a way to identify and search filesystem
objects using metadata in addition to their paths.

---

## 7. Consistency Checking

`ufs_fsck()` checks the filesystem's block allocation state and rebuilds
the block bitmap from the blocks referenced by allocated inodes.

It also ensures that filesystem metadata blocks remain marked as allocated.

---

## 8. Key Invariants

- Filesystem state is stored inside the disk image.
- Metadata blocks are never allocated as file data blocks.
- Newly allocated data blocks are zero-filled.
- Inodes sharing a block are preserved through read-modify-write operations.
- Directories must be empty before they can be removed.
- The root directory cannot be removed.
- Files and directories are distinguished through their inode type.
- Open file descriptors are process-local and are not persisted.
- File tags are associated with filesystem objects and can be searched
  through the tag API.
- Filesystem modifications update the corresponding on-disk metadata.