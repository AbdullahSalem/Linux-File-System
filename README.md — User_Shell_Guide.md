# UserFS — User Guide

## Overview

`ufs_shell` is an interactive command-line interface for interacting with the UserFS filesystem.

It allows you to work with the filesystem without writing a separate C program for every operation.

The shell provides commands for:

- Formatting and mounting a filesystem image
- Creating and removing directories
- Creating and deleting files
- Opening and closing files
- Reading and writing file data
- Moving the file position with `seek`
- Changing file size with `truncate`
- Viewing file metadata with `stat`
- Assigning tags to files with `settag`
- Finding files by tag with `findtag`
- Checking filesystem consistency with `fsck`

---

# 1. Building

From the project directory, compile the filesystem and shell together:

```bash
gcc -Wall -Wextra -std=c11 userfs.c ufs_shell.c -o ufs_shell
```

If compilation succeeds, run:

```bash
./ufs_shell
```

You should see:

```text
=====================================
             UserFS Shell
=====================================
Type 'help' for available commands.

ufs>
```

---

# 2. Getting Help

Inside the shell:

```text
ufs> help
```

This displays all available commands, their syntax, supported open flags, and seek positions.

---

# 3. Basic Workflow

A normal filesystem session follows this order:

```text
Format
  ↓
Mount
  ↓
Create/use files and directories
  ↓
Set/search file tags
  ↓
Check filesystem
  ↓
Unmount
```

For example:

```text
ufs> format filesystem.img 1048576
ufs> mount filesystem.img
ufs> mkdir /docs
ufs> create /docs/test.txt
ufs> ...
ufs> fsck
ufs> unmount
ufs> exit
```

---

# 4. Filesystem Commands

## Format

Creates a new filesystem image.

```text
format <image> <size>
```

Example:

```text
ufs> format filesystem.img 1048576
```

**Warning:** Formatting a filesystem creates a fresh filesystem and may destroy the previous contents of that image.

---

## Mount

Mounts an existing filesystem image.

```text
mount <image>
```

Example:

```text
ufs> mount filesystem.img
```

The filesystem should be mounted before performing normal filesystem operations.

---

## Unmount

Unmounts the currently mounted filesystem.

```text
unmount
```

Example:

```text
ufs> unmount
```

It is recommended to run `fsck` before unmounting during testing.

---

# 5. Directory Commands

## Create a Directory

```text
mkdir <path>
```

Example:

```text
ufs> mkdir /docs
```

Nested directories can also be created:

```text
ufs> mkdir /docs/course
```

---

## List a Directory

```text
listdir <path>
```

Example:

```text
ufs> listdir /
```

or:

```text
ufs> listdir /docs
```

The command displays the entries contained in the directory, including their type, name, and size.

---

## Remove a Directory

```text
rmdir <path>
```

Example:

```text
ufs> rmdir /docs/course
```

The directory should normally be empty before removing it.

---

# 6. File Commands

## Create a File

```text
create <path>
```

Example:

```text
ufs> create /docs/test.txt
```

---

## Remove a File

```text
unlink <path>
```

Example:

```text
ufs> unlink /docs/test.txt
```

---

## View File Information

```text
stat <path>
```

Example:

```text
ufs> stat /docs/test.txt
```

This displays information such as:

```text
Type: FILE
Size: 12 bytes
```

For directories, the type is displayed as:

```text
Type: DIRECTORY
```

---

# 7. Opening Files

Files must be opened before reading or writing.

```text
open <path> <flags>
```

Available flags:

```text
rdonly
wronly
rdwr
append
```

Examples:

```text
ufs> open /docs/test.txt wronly
```

or:

```text
ufs> open /docs/test.txt rdonly
```

The shell returns a file descriptor:

```text
File descriptor: 0
```

Use this number for subsequent `read`, `write`, `seek`, and `close` operations.

---

# 8. Writing Data

```text
write <fd> <text>
```

Example:

```text
ufs> write 0 Hello UserFS
```

Text containing spaces is supported:

```text
ufs> write 0 Hello this is my filesystem
```

The shell reports the number of bytes written:

```text
Bytes written: 12
```

The file must be opened with a write-capable flag such as `wronly`, `rdwr`, or an appropriate append mode.

---

# 9. Reading Data

```text
read <fd> <count>
```

Example:

```text
ufs> read 0 20
```

The shell displays the number of bytes returned and the data:

```text
Bytes read: 12
Data: Hello UserFS
```

The file descriptor must have been opened with a read-capable mode.

---

# 10. Changing the File Position

```text
seek <fd> <offset> <whence>
```

Supported values:

```text
set
cur
end
```

Examples:

Move to the beginning:

```text
ufs> seek 0 0 set
```

Move five bytes forward from the current position:

```text
ufs> seek 0 5 cur
```

Move two bytes before the end:

```text
ufs> seek 0 -2 end
```

A successful operation reports the new file position:

```text
New position: 5
```

---

# 11. Closing Files

```text
close <fd>
```

Example:

```text
ufs> close 0
```

Use the file descriptor returned by `open`.

Closing a file descriptor releases the corresponding open-file resource.

---

# 12. Changing File Size

```text
truncate <path> <size>
```

Example:

```text
ufs> truncate /docs/test.txt 5
```

Verify the new size with:

```text
ufs> stat /docs/test.txt
```

For example:

```text
Size: 5 bytes
```

---

# 13. File Tags

UserFS supports assigning a tag to a file and searching for files associated with a tag.

Tags can be used to organize or classify files without changing their filenames or directory locations.

## Set a Tag

```text
settag <path> <tag>
```

Example:

```text
ufs> settag /docs/test.txt important
```

This associates the specified tag with the file.

The path must identify an existing file.

---

## Find Files by Tag

```text
findtag <tag>
```

Example:

```text
ufs> findtag important
```

This searches the filesystem for files associated with the specified tag.

The underlying UserFS API returns the inode numbers of matching files. Therefore, the shell reports the matching inode numbers rather than assuming that the API directly returns file paths.

For example, a result may be displayed in the form:

```text
Matching inodes: 1 5 8
```

The exact output depends on the implementation of the shell command.

---

# 14. Checking the Filesystem

Run:

```text
ufs> fsck
```

This checks the filesystem consistency.

It is particularly useful after creating, deleting, tagging, or modifying files.

A successful check reports:

```text
fsck completed successfully.
```

---

# 15. Ending a Session

A recommended ending sequence is:

```text
ufs> fsck
ufs> unmount
ufs> exit
```

You can also use:

```text
ufs> quit
```

---

# 16. Complete Example

The following example demonstrates formatting, mounting, directory and file creation, writing, reading, tagging, seeking, metadata inspection, filesystem checking, and unmounting.

```text
ufs> format filesystem.img 1048576
Filesystem formatted successfully.

ufs> mount filesystem.img
Filesystem mounted successfully.

ufs> mkdir /docs
Directory created.

ufs> create /docs/test.txt
File created.

ufs> open /docs/test.txt wronly
File descriptor: 0

ufs> write 0 Hello UserFS
Bytes written: 12

ufs> close 0
File descriptor closed.

ufs> open /docs/test.txt rdonly
File descriptor: 0

ufs> seek 0 0 set
New position: 0

ufs> read 0 12
Bytes read: 12
Data: Hello UserFS

ufs> close 0
File descriptor closed.

ufs> stat /docs/test.txt
Type: FILE
Size: 12 bytes

ufs> settag /docs/test.txt important

ufs> findtag important

ufs> listdir /docs

ufs> fsck
fsck completed successfully.

ufs> unmount
Filesystem unmounted successfully.

ufs> exit
```

---

# 17. Command Reference

| Command | Syntax | Purpose |
|---|---|---|
| `format` | `format <image> <size>` | Create a new filesystem image |
| `mount` | `mount <image>` | Mount a filesystem image |
| `unmount` | `unmount` | Unmount the filesystem |
| `mkdir` | `mkdir <path>` | Create a directory |
| `rmdir` | `rmdir <path>` | Remove a directory |
| `listdir` | `listdir <path>` | List directory contents |
| `create` | `create <path>` | Create a file |
| `unlink` | `unlink <path>` | Remove a file |
| `open` | `open <path> <flags>` | Open a file |
| `close` | `close <fd>` | Close an open file |
| `read` | `read <fd> <count>` | Read file data |
| `write` | `write <fd> <text>` | Write data to a file |
| `seek` | `seek <fd> <offset> <whence>` | Change file position |
| `truncate` | `truncate <path> <size>` | Change file size |
| `stat` | `stat <path>` | Display file metadata |
| `settag` | `settag <path> <tag>` | Assign a tag to a file |
| `findtag` | `findtag <tag>` | Find files associated with a tag |
| `fsck` | `fsck` | Check filesystem consistency |
| `help` | `help` | Display command help |
| `exit` | `exit` | Exit the shell |
| `quit` | `quit` | Exit the shell |

---

# 18. Open Flags Reference

| Flag | Purpose |
|---|---|
| `rdonly` | Open for reading |
| `wronly` | Open for writing |
| `rdwr` | Open for reading and writing |
| `append` | Open with append behavior |

---

# 19. Seek Positions Reference

| Position | Meaning |
|---|---|
| `set` | Position relative to the beginning of the file |
| `cur` | Position relative to the current file position |
| `end` | Position relative to the end of the file |

---

## Summary

`ufs_shell` provides a command-line interface over the UserFS API, allowing filesystem operations to be tested interactively. The shell supports filesystem management, directories, files, file descriptors, data I/O, file positioning, file resizing, metadata inspection, file tagging, tag-based searching, and filesystem consistency checking.