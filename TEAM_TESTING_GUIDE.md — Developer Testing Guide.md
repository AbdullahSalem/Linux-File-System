# UserFS — Team Testing Guide

## Purpose

This document is intended for the development team.

Unlike `README.md`, which explains normal use of the shell, this document describes how to systematically test the UserFS implementation and isolate problems.

The goal is to test each API layer independently before moving to more complex operations.

---

# 1. Build

Compile the implementation and interactive shell:

```bash
gcc -Wall -Wextra -std=c11 userfs.c ufs_shell.c -o ufs_shell
```

Run:

```bash
./ufs_shell
```

---

# 2. Testing Philosophy

Do not begin by testing the entire filesystem with a large sequence of commands.

Instead, test progressively:

```text
Filesystem
    ↓
Root / Path Resolution
    ↓
Directories
    ↓
Files
    ↓
File Metadata
    ↓
File Tags
    ↓
Open File Descriptors
    ↓
Read / Write
    ↓
Seek
    ↓
Truncate
    ↓
Deletion
    ↓
Filesystem Consistency
    ↓
Persistence
    ↓
Large-File Allocation
```

When a test fails, stop at that level and investigate before continuing.

---

# 3. Level 1 — Filesystem Initialization

Start with a fresh image:

```text
ufs> format filesystem.img 1048576
```

Expected:

```text
Filesystem formatted successfully.
```

Then:

```text
ufs> mount filesystem.img
```

Expected:

```text
Filesystem mounted successfully.
```

---

# 4. Level 2 — Root and Path Resolution

Immediately test:

```text
ufs> stat /
ufs> listdir /
```

The root path should resolve successfully.

Then test a nonexistent path:

```text
ufs> stat /does_not_exist
```

The operation should fail appropriately.

This level is important because almost every other API operation depends on path resolution.

If `/` cannot be resolved, do not continue with higher-level testing.

---

# 5. Level 3 — Directory Operations

Create:

```text
ufs> mkdir /docs
```

Verify:

```text
ufs> stat /docs
ufs> listdir /
```

Create a nested directory:

```text
ufs> mkdir /docs/course
```

Verify:

```text
ufs> stat /docs/course
ufs> listdir /docs
```

Test duplicate creation:

```text
ufs> mkdir /docs
```

The second operation should fail rather than silently replacing the existing directory.

---

# 6. Level 4 — File Creation

Create:

```text
ufs> create /docs/course/test.txt
```

Verify:

```text
ufs> stat /docs/course/test.txt
ufs> listdir /docs/course
```

The new file should initially have size zero.

Test duplicate creation:

```text
ufs> create /docs/course/test.txt
```

The second operation should fail.

Test a nonexistent parent:

```text
ufs> create /missing/test.txt
```

This should fail.

---

# 7. Level 5 — File Metadata

The `stat` API should be tested independently from file contents.

Run:

```text
ufs> stat /docs/course/test.txt
```

Verify:

- The type is reported as `FILE`.
- The initial size is zero.
- The operation succeeds for an existing file.

Also test a directory:

```text
ufs> stat /docs
```

The type should be reported as `DIRECTORY`.

Test a nonexistent path:

```text
ufs> stat /missing
```

The operation should fail with an appropriate error.

---

# 8. Level 6 — File Tags

UserFS supports assigning a tag to a file and finding files by tag.

The two relevant APIs are:

```c
int ufs_set_tag(const char *path, const char *tag);

int ufs_find_by_tag(
    const char *tag,
    uint32_t *matching_inums,
    size_t max_results
);
```

The shell should expose these operations through commands for setting a tag and searching for matching files.

## Set a Tag

Create a few files:

```text
ufs> create /docs/file1.txt
ufs> create /docs/file2.txt
ufs> create /docs/file3.txt
```

Assign the same tag to two files:

```text
ufs> settag /docs/file1.txt project
ufs> settag /docs/file2.txt project
```

Assign a different tag to the third file:

```text
ufs> settag /docs/file3.txt test
```

## Find Files by Tag

Search for the `project` tag:

```text
ufs> findtag project
```

The result should contain the files associated with that tag.

For example, the result should identify the inodes corresponding to:

```text
/docs/file1.txt
/docs/file2.txt
```

The file with the `test` tag should not appear in the `project` results.

## Test a Tag With No Matches

```text
ufs> findtag nonexistent
```

This should return no matching files rather than reporting unrelated files.

## Change an Existing Tag

Change the tag of a file:

```text
ufs> settag /docs/file1.txt test
```

Then:

```text
ufs> findtag project
```

`file1.txt` should no longer be associated with `project`.

Search again:

```text
ufs> findtag test
```

The result should now include `file1.txt` and `file3.txt`.

## Tag Error Cases

Test tagging a nonexistent file:

```text
ufs> settag /missing.txt project
```

This should fail appropriately.

Test searching with an invalid or empty tag according to the API's validation rules.

## Tag Persistence

After assigning tags:

```text
ufs> fsck
ufs> unmount
```

Remount the same image:

```text
ufs> mount filesystem.img
```

Then search again:

```text
ufs> findtag project
ufs> findtag test
```

Tags should remain associated with the correct files after remounting if tag information is stored persistently by the implementation.

---

# 9. Level 7 — Open and Close

Open the file:

```text
ufs> open /docs/course/test.txt wronly
```

Record the returned file descriptor.

For example:

```text
File descriptor: 0
```

Then close it:

```text
ufs> close 0
```

Test an invalid descriptor:

```text
ufs> close 99
```

The operation should fail with an appropriate error.

---

# 10. Level 8 — Write

Open:

```text
ufs> open /docs/course/test.txt wronly
```

Write:

```text
ufs> write 0 Hello UserFS
```

Verify the file size:

```text
ufs> stat /docs/course/test.txt
```

The inode size should correspond to the amount written.

Test multiple writes:

```text
ufs> write 0 First
ufs> write 0 Second
ufs> write 0 Third
```

Then verify the resulting file contents.

---

# 11. Level 9 — Read

Close the write descriptor:

```text
ufs> close 0
```

Open for reading:

```text
ufs> open /docs/course/test.txt rdonly
```

Move to the beginning:

```text
ufs> seek 0 0 set
```

Read:

```text
ufs> read 0 20
```

Verify that the returned data matches what was written.

---

# 12. Level 10 — Seek

Test all three `whence` modes.

## SEEK_SET

```text
ufs> seek 0 0 set
```

Expected position:

```text
0
```

## SEEK_CUR

```text
ufs> seek 0 5 set
ufs> seek 0 3 cur
```

Expected position:

```text
8
```

## SEEK_END

For a file of known size:

```text
ufs> seek 0 0 end
```

The returned position should equal the file size.

Then:

```text
ufs> seek 0 -2 end
```

The position should be two bytes before the end.

---

# 13. Level 11 — Truncate

First determine the current size:

```text
ufs> stat /docs/course/test.txt
```

Reduce it:

```text
ufs> truncate /docs/course/test.txt 5
```

Verify:

```text
ufs> stat /docs/course/test.txt
```

Then read the file and verify that only the expected data remains.

If the implementation supports extending files through `truncate`, test that separately as well.

---

# 14. Level 12 — File Deletion

Close any open descriptor:

```text
ufs> close 0
```

Remove the file:

```text
ufs> unlink /docs/course/test.txt
```

Verify:

```text
ufs> listdir /docs/course
```

Then:

```text
ufs> stat /docs/course/test.txt
```

The file should no longer be resolvable.

If the file had a tag, also test the tag index after deletion:

```text
ufs> findtag project
```

The deleted file should not continue to appear as a valid matching file.

---

# 15. Level 13 — Directory Deletion

After deleting the file:

```text
ufs> rmdir /docs/course
```

Then:

```text
ufs> rmdir /docs
```

Finally:

```text
ufs> listdir /
```

The root should no longer contain `/docs`.

---

# 16. Level 14 — Filesystem Consistency

Run:

```text
ufs> fsck
```

Run `fsck` after major allocation/deallocation operations, particularly:

```text
create
write
truncate
unlink
rmdir
```

Also run it after tag-related metadata changes if tags are stored in filesystem metadata.

A failure here should be investigated before continuing.

---

# 17. Level 15 — Persistence

Create a file:

```text
ufs> create /persistent.txt
ufs> open /persistent.txt wronly
ufs> write 0 Persistent data
ufs> close 0
```

Assign a tag:

```text
ufs> settag /persistent.txt persistent
```

Verify:

```text
ufs> stat /persistent.txt
ufs> findtag persistent
```

Run:

```text
ufs> fsck
```

Unmount:

```text
ufs> unmount
```

Then start the shell again if desired:

```bash
./ufs_shell
```

Mount the same image:

```text
ufs> mount filesystem.img
```

Verify:

```text
ufs> stat /persistent.txt
ufs> open /persistent.txt rdonly
ufs> read 0 20
ufs> close 0
```

Verify the tag:

```text
ufs> findtag persistent
```

The file data, metadata, and tag information should still be present after remounting if these structures are persistent.

---

# 18. Error Testing

The API should reject invalid operations appropriately.

## Nonexistent File

```text
ufs> open /missing.txt rdonly
```

## Nonexistent Directory

```text
ufs> listdir /missing
```

## Duplicate File

```text
ufs> create /test.txt
ufs> create /test.txt
```

## Duplicate Directory

```text
ufs> mkdir /docs
ufs> mkdir /docs
```

## Invalid File Descriptor

```text
ufs> read 99 10
ufs> write 99 hello
ufs> close 99
ufs> seek 99 0 set
```

## Invalid Path

```text
ufs> stat /does/not/exist
ufs> settag /does/not/exist test
```

## Tag Search

```text
ufs> findtag nonexistent
```

The command should return no matching files.

Record both the shell output and `errno`.

---

# 19. Allocation and Deallocation Testing

After basic functionality works, test block allocation.

Create a file and write enough data to require multiple data blocks.

For example:

```text
ufs> create /large.txt
ufs> open /large.txt wronly
```

Then write increasingly larger amounts of data.

After each stage:

```text
ufs> stat /large.txt
ufs> fsck
```

Then delete the file:

```text
ufs> close 0
ufs> unlink /large.txt
ufs> fsck
```

The purpose is to verify that blocks are correctly allocated and released.

---

# 20. Direct and Indirect Block Testing

Large files should be specifically tested to cross the boundary between direct and indirect addressing.

The test should verify:

1. Data fits correctly within direct blocks.
2. Data crossing into the indirect region is written correctly.
3. Data can be read back correctly.
4. File size is updated correctly.
5. Blocks are released correctly when the file is truncated or deleted.
6. `fsck` reports a consistent filesystem afterward.

Do not consider large-file support verified merely because a small file can be read and written.

---

# 21. Tagging Regression Test

A complete tag regression test should be run after changes to inode, directory, allocation, or metadata code.

Example:

```text
ufs> format filesystem.img 1048576
ufs> mount filesystem.img

ufs> mkdir /projects
ufs> create /projects/a.txt
ufs> create /projects/b.txt
ufs> create /projects/c.txt

ufs> settag /projects/a.txt important
ufs> settag /projects/b.txt important
ufs> settag /projects/c.txt temporary

ufs> findtag important
ufs> findtag temporary

ufs> settag /projects/a.txt temporary

ufs> findtag important
ufs> findtag temporary

ufs> fsck
ufs> unmount
ufs> mount filesystem.img

ufs> findtag important
ufs> findtag temporary
```

Verify that:

- `a.txt` and `b.txt` initially match `important`.
- `c.txt` initially matches `temporary`.
- After changing `a.txt`, it no longer matches `important`.
- `a.txt` and `c.txt` match `temporary`.
- The results remain correct after unmounting and remounting.
- `fsck` remains successful.

---

# 22. Recommended Regression Test

After modifying filesystem code, run at least:

```text
format
mount
stat /
listdir /
mkdir
create
stat
settag
findtag
open
write
seek
read
stat
truncate
close
unlink
findtag
rmdir
fsck
unmount
mount
stat
findtag
fsck
```

This provides a basic regression check for the complete API, including the tag functionality.

---

# 23. Debugging Procedure

When an operation fails:

### Step 1 — Identify the API

For example:

```text
stat /docs/test.txt
```

means the first suspect is:

```c
ufs_stat()
```

and then any helper it calls, such as path resolution.

For:

```text
settag /docs/test.txt project
```

the first suspect is:

```c
ufs_set_tag()
```

For:

```text
findtag project
```

the first suspect is:

```c
ufs_find_by_tag()
```

### Step 2 — Test the lower-level operation

For example, before testing a complicated file path, verify:

```text
stat /
stat /docs
stat /docs/test.txt
```

This identifies where path resolution begins to fail.

For tag functionality, first verify that the target file exists:

```text
stat /docs/test.txt
```

Then:

```text
settag /docs/test.txt project
findtag project
```

### Step 3 — Check the return value

Every API function should be checked for failure.

### Step 4 — Check `errno`

The shell prints:

```text
ERROR: <message> (errno=<number>)
```

Record this value when reporting a bug.

### Step 5 — Run `fsck`

If the operation modifies filesystem metadata:

```text
ufs> fsck
```

### Step 6 — Reproduce from a fresh image

If corruption is suspected, format a new test image and reproduce the smallest sequence that causes the failure.

---

# 24. Reporting a Bug to the Team

When reporting a problem, provide:

1. The exact command sequence.
2. The expected output.
3. The actual output.
4. The `errno` value.
5. Whether the image was freshly formatted.
6. Whether the filesystem was mounted.
7. Whether `fsck` passed before the failure.
8. Any relevant source-code changes.
9. For tag-related problems, the tag value and files that were expected to match.

Example:

```text
Image:
filesystem.img

Commands:

format filesystem.img 1048576
mount filesystem.img
create /test.txt
settag /test.txt project
findtag project
```

Result:

```text
ERROR: Invalid argument (errno=22)
```

Expected:

```text
/test.txt should be returned as a matching file.
```

This is much more useful than simply reporting:

```text
tag search is broken
```

---

# 25. Test Completion Checklist

## Filesystem

- [ ] Format works
- [ ] Mount works
- [ ] Unmount works
- [ ] Root inode can be accessed
- [ ] `fsck` succeeds

## Directories

- [ ] `mkdir`
- [ ] `listdir`
- [ ] Nested directories
- [ ] Duplicate directory rejection
- [ ] `rmdir`
- [ ] Empty-directory handling

## Files

- [ ] `create`
- [ ] Duplicate file rejection
- [ ] `unlink`
- [ ] `stat`

## Tags

- [ ] Set a tag on a file
- [ ] Find files by tag
- [ ] Multiple files can share a tag
- [ ] Files with different tags are not returned
- [ ] Changing a tag updates search results
- [ ] Searching for a nonexistent tag returns no matches
- [ ] Tagging a nonexistent file fails appropriately
- [ ] Tags survive unmount/remount if persistent tag storage is implemented
- [ ] Deleted files do not remain as valid tag matches

## File Descriptors

- [ ] `open`
- [ ] `close`
- [ ] Invalid descriptor handling

## Data

- [ ] `write`
- [ ] `read`
- [ ] Multiple writes
- [ ] `seek`
- [ ] `truncate`

## Persistence

- [ ] Data survives unmount
- [ ] Data survives remount
- [ ] Metadata survives remount
- [ ] Tag information survives remount

## Allocation

- [ ] Multiple-block files
- [ ] Direct-block boundary
- [ ] Indirect-block boundary
- [ ] Block deallocation
- [ ] `fsck` after allocation/deallocation

## Error Handling

- [ ] Missing paths
- [ ] Duplicate paths
- [ ] Invalid descriptors
- [ ] Invalid arguments
- [ ] Invalid tag operations
- [ ] Correct `errno` values

---

# 26. Files in the Project

The intended separation is:

```text
userfs.c
    ↓
Actual UserFS implementation

userfs.h
    ↓
Public UserFS API

ufs_shell.c
    ↓
Interactive API interface

README.md
    ↓
Instructions for normal users

TEAM_TESTING_GUIDE.md
    ↓
Detailed testing and debugging procedures
```