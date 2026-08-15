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
- Checking filesystem consistency with `fsck`

---

## 1. Building

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

## 2. Getting Help

Inside the shell:

```text
ufs> help
```

This displays all available commands and their syntax.

---

## 3. Basic Workflow

A normal filesystem session follows this order:

```text
Format
  ↓
Mount
  ↓
Create/use files and directories
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

This displays information such as the file type and size.

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

The shell reports the number of bytes written.

---

# 9. Reading Data

```text
read <fd> <count>
```

Example:

```text
ufs> read 0 20
```

The shell displays the number of bytes returned and the data.

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

Move five bytes forward:

```text
ufs> seek 0 5 cur
```

Move two bytes before the end:

```text
ufs> seek 0 -2 end
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

---

# 13. Checking the Filesystem

Run:

```text
ufs> fsck
```

This checks the filesystem consistency.

It is particularly useful after creating, deleting, or modifying files.

---

# 14. Ending a Session

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

# 15. Complete Example

```text
ufs> format filesystem.img 1048576
ufs> mount filesystem.img

ufs> mkdir /docs
ufs> create /docs/test.txt

ufs> open /docs/test.txt wronly
File descriptor: 0

ufs> write 0 Hello UserFS
Bytes written: 12

ufs> close 0

ufs> open /docs/test.txt rdonly
File descriptor: 0

ufs> seek 0 0 set
New position: 0

ufs> read 0 12
Bytes read: 12
Data: Hello UserFS

ufs> close 0

ufs> stat /docs/test.txt
ufs> listdir /docs

ufs> fsck
ufs> unmount
ufs> exit
```

