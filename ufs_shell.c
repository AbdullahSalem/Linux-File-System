#include "userfs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_SIZE 1024
#define READ_BUFFER_SIZE 4096


static void print_error(void)
{
    printf("ERROR: %s (errno=%d)\n",
           strerror(errno),
           errno);
}


static void print_help(void)
{
    printf("\n");
    printf("Commands:\n");
    printf("  format <image> <size>\n");
    printf("  mount <image>\n");
    printf("  unmount\n");
    printf("\n");

    printf("  mkdir <path>\n");
    printf("  rmdir <path>\n");
    printf("  listdir <path>\n");
    printf("  create <path>\n");
    printf("  unlink <path>\n");
    printf("\n");

    printf("  open <path> <flags>\n");
    printf("  close <fd>\n");
    printf("  read <fd> <count>\n");
    printf("  write <fd> <text>\n");
    printf("  seek <fd> <offset> <whence>\n");
    printf("\n");

    printf("  truncate <path> <size>\n");
    printf("  stat <path>\n");
    printf("  fsck\n");
    printf("  help\n");
    printf("  exit\n");
    printf("\n");

    printf("Open flags:\n");
    printf("  rdonly\n");
    printf("  wronly\n");
    printf("  rdwr\n");
    printf("  append\n");
    printf("\n");

    printf("Seek positions:\n");
    printf("  set\n");
    printf("  cur\n");
    printf("  end\n");
    printf("\n");
}


static int parse_flags(const char *text)
{
    if (strcmp(text, "rdonly") == 0)
        return UFS_O_RDONLY;

    if (strcmp(text, "wronly") == 0)
        return UFS_O_WRONLY;

    if (strcmp(text, "rdwr") == 0)
        return UFS_O_RDWR;

    if (strcmp(text, "append") == 0)
        return UFS_O_APPEND;

    return -1;
}


static int parse_whence(const char *text)
{
    if (strcmp(text, "set") == 0)
        return SEEK_SET;

    if (strcmp(text, "cur") == 0)
        return SEEK_CUR;

    if (strcmp(text, "end") == 0)
        return SEEK_END;

    return -1;
}


static void command_format(char **args, int argc)
{
    char *end;
    unsigned long long image_size;
    int result;

    if (argc != 3)
    {
        printf("Usage: format <image> <size>\n");
        return;
    }

    image_size = strtoull(args[2], &end, 10);

    if (*args[2] == '\0' || *end != '\0')
    {
        printf("Invalid image size.\n");
        return;
    }

    result = ufs_format(
        args[1],
        (size_t)image_size
    );

    if (result == 0)
        printf("Filesystem formatted successfully.\n");
    else
        print_error();
}


static void command_mount(char **args, int argc)
{
    int result;

    if (argc != 2)
    {
        printf("Usage: mount <image>\n");
        return;
    }

    result = ufs_mount(args[1]);

    if (result == 0)
        printf("Filesystem mounted successfully.\n");
    else
        print_error();
}


static void command_unmount(int argc)
{
    int result;

    if (argc != 1)
    {
        printf("Usage: unmount\n");
        return;
    }

    result = ufs_unmount();

    if (result == 0)
        printf("Filesystem unmounted successfully.\n");
    else
        print_error();
}


static void command_mkdir(char **args, int argc)
{
    int result;

    if (argc != 2)
    {
        printf("Usage: mkdir <path>\n");
        return;
    }

    result = ufs_mkdir(args[1]);

    if (result == 0)
        printf("Directory created.\n");
    else
        print_error();
}


static void command_rmdir(char **args, int argc)
{
    int result;

    if (argc != 2)
    {
        printf("Usage: rmdir <path>\n");
        return;
    }

    result = ufs_rmdir(args[1]);

    if (result == 0)
        printf("Directory removed.\n");
    else
        print_error();
}


static void command_listdir(char **args, int argc)
{
    struct ufs_dirent entries[128];
    int count;

    if (argc != 2)
    {
        printf("Usage: listdir <path>\n");
        return;
    }

    count = ufs_listdir(
        args[1],
        entries,
        128
    );

    if (count < 0)
    {
        print_error();
        return;
    }

    printf("Entries: %d\n", count);

    for (int i = 0; i < count; i++)
    {
        const char *type;

        if (entries[i].type == UFS_TYPE_DIR)
            type = "DIR";
        else if (entries[i].type == UFS_TYPE_FILE)
            type = "FILE";
        else
            type = "UNKNOWN";

        printf("  %-5s %-31s %zu bytes\n",
               type,
               entries[i].name,
               entries[i].size);
    }
}


static void command_create(char **args, int argc)
{
    int result;

    if (argc != 2)
    {
        printf("Usage: create <path>\n");
        return;
    }

    result = ufs_create(args[1]);

    if (result == 0)
        printf("File created.\n");
    else
        print_error();
}


static void command_unlink(char **args, int argc)
{
    int result;

    if (argc != 2)
    {
        printf("Usage: unlink <path>\n");
        return;
    }

    result = ufs_unlink(args[1]);

    if (result == 0)
        printf("File removed.\n");
    else
        print_error();
}


static void command_open(char **args, int argc)
{
    int flags;
    int fd;

    if (argc != 3)
    {
        printf("Usage: open <path> <flags>\n");
        return;
    }

    flags = parse_flags(args[2]);

    if (flags == -1)
    {
        printf("Invalid flags.\n");
        printf("Use: rdonly, wronly, rdwr, append\n");
        return;
    }

    fd = ufs_open(
        args[1],
        flags
    );

    if (fd < 0)
    {
        print_error();
        return;
    }

    printf("File descriptor: %d\n", fd);
}


static void command_close(char **args, int argc)
{
    char *end;
    long fd;
    int result;

    if (argc != 2)
    {
        printf("Usage: close <fd>\n");
        return;
    }

    fd = strtol(
        args[1],
        &end,
        10
    );

    if (*args[1] == '\0' || *end != '\0')
    {
        printf("Invalid file descriptor.\n");
        return;
    }

    result = ufs_close((int)fd);

    if (result == 0)
        printf("File descriptor closed.\n");
    else
        print_error();
}


static void command_read(char **args, int argc)
{
    char *end;
    long fd;
    unsigned long count;
    char *buffer;
    ssize_t result;

    if (argc != 3)
    {
        printf("Usage: read <fd> <count>\n");
        return;
    }

    fd = strtol(
        args[1],
        &end,
        10
    );

    if (*args[1] == '\0' || *end != '\0')
    {
        printf("Invalid file descriptor.\n");
        return;
    }

    count = strtoul(
        args[2],
        &end,
        10
    );

    if (*args[2] == '\0' || *end != '\0')
    {
        printf("Invalid count.\n");
        return;
    }

    if (count == 0)
    {
        printf("0 bytes read.\n");
        return;
    }

    if (count > READ_BUFFER_SIZE)
    {
        printf("Maximum read size is %d bytes.\n",
               READ_BUFFER_SIZE);
        return;
    }

    buffer = malloc(count + 1);

    if (buffer == NULL)
    {
        perror("malloc");
        return;
    }

    result = ufs_read(
        (int)fd,
        buffer,
        count
    );

    if (result < 0)
    {
        print_error();
        free(buffer);
        return;
    }

    buffer[result] = '\0';

    printf("Bytes read: %ld\n",
           (long)result);

    printf("Data: ");

    fwrite(
        buffer,
        1,
        (size_t)result,
        stdout
    );

    printf("\n");

    free(buffer);
}


static void command_write(const char *input)
{
    const char *data;
    char *end;
    long fd;
    ssize_t result;

    /*
     * input:
     *
     * write <fd> <text>
     *
     * Skip "write".
     */
    data = input + 5;

    /*
     * Skip spaces after "write".
     */
    while (*data == ' ' || *data == '\t')
        data++;

    if (*data == '\0')
    {
        printf("Usage: write <fd> <text>\n");
        return;
    }

    /*
     * Parse the file descriptor.
     */
    fd = strtol(
        data,
        &end,
        10
    );

    if (end == data)
    {
        printf("Invalid file descriptor.\n");
        return;
    }

    /*
     * There must be whitespace after the fd.
     */
    if (*end != ' ' && *end != '\t')
    {
        printf("Usage: write <fd> <text>\n");
        return;
    }

    data = end;

    /*
     * Skip spaces between fd and data.
     */
    while (*data == ' ' || *data == '\t')
        data++;

    if (*data == '\0')
    {
        printf("Usage: write <fd> <text>\n");
        return;
    }

    result = ufs_write(
        (int)fd,
        data,
        strlen(data)
    );

    if (result < 0)
    {
        print_error();
        return;
    }

    printf("Bytes written: %ld\n",
           (long)result);
}


static void command_seek(char **args, int argc)
{
    char *end;
    long fd;
    long long offset;
    int whence;
    off_t result;

    if (argc != 4)
    {
        printf("Usage: seek <fd> <offset> <set|cur|end>\n");
        return;
    }

    fd = strtol(
        args[1],
        &end,
        10
    );

    if (*args[1] == '\0' || *end != '\0')
    {
        printf("Invalid file descriptor.\n");
        return;
    }

    offset = strtoll(
        args[2],
        &end,
        10
    );

    if (*args[2] == '\0' || *end != '\0')
    {
        printf("Invalid offset.\n");
        return;
    }

    whence = parse_whence(args[3]);

    if (whence == -1)
    {
        printf("Invalid whence.\n");
        printf("Use: set, cur, or end\n");
        return;
    }

    result = ufs_seek(
        (int)fd,
        (off_t)offset,
        whence
    );

    if (result < 0)
        print_error();
    else
        printf("New position: %ld\n",
               (long)result);
}


static void command_truncate(char **args, int argc)
{
    char *end;
    unsigned long long size;
    int result;

    if (argc != 3)
    {
        printf("Usage: truncate <path> <size>\n");
        return;
    }

    size = strtoull(
        args[2],
        &end,
        10
    );

    if (*args[2] == '\0' || *end != '\0')
    {
        printf("Invalid size.\n");
        return;
    }

    result = ufs_truncate(
        args[1],
        (size_t)size
    );

    if (result == 0)
        printf("File truncated.\n");
    else
        print_error();
}


static void command_stat(char **args, int argc)
{
    struct ufs_stat st;
    int result;

    if (argc != 2)
    {
        printf("Usage: stat <path>\n");
        return;
    }

    result = ufs_stat(
        args[1],
        &st
    );

    if (result != 0)
    {
        print_error();
        return;
    }

    printf("Type: ");

    if (st.type == UFS_TYPE_FILE)
        printf("FILE\n");
    else if (st.type == UFS_TYPE_DIR)
        printf("DIRECTORY\n");
    else
        printf("UNKNOWN (%d)\n", st.type);

    printf("Size: %zu bytes\n",
           st.size);
}


static void command_fsck(int argc)
{
    int result;

    if (argc != 1)
    {
        printf("Usage: fsck\n");
        return;
    }

    result = ufs_fsck();

    if (result == 0)
        printf("fsck completed successfully.\n");
    else
        print_error();
}


int main(void)
{
    char input[INPUT_SIZE];

    printf("=====================================\n");
    printf("             UserFS Shell\n");
    printf("=====================================\n");
    printf("Type 'help' for available commands.\n");
    printf("\n");

    while (1)
    {
        char *args[8];
        char *token;
        int argc = 0;

        printf("ufs> ");
        fflush(stdout);

        if (fgets(
                input,
                sizeof(input),
                stdin) == NULL)
        {
            printf("\n");
            break;
        }

        /*
         * Remove newline.
         */
        input[strcspn(input, "\n")] = '\0';

        if (input[0] == '\0')
            continue;

        /*
         * write is handled separately because
         * the text after the fd may contain spaces.
         */
        if (strncmp(input, "write ", 6) == 0)
        {
            command_write(input);
            continue;
        }

        /*
         * Split normal commands into arguments.
         */
        token = strtok(
            input,
            " \t"
        );

        while (token != NULL && argc < 8)
        {
            args[argc++] = token;

            token = strtok(
                NULL,
                " \t"
            );
        }

        if (argc == 0)
            continue;

        if (strcmp(args[0], "format") == 0)
        {
            command_format(args, argc);
        }
        else if (strcmp(args[0], "mount") == 0)
        {
            command_mount(args, argc);
        }
        else if (strcmp(args[0], "unmount") == 0)
        {
            command_unmount(argc);
        }
        else if (strcmp(args[0], "mkdir") == 0)
        {
            command_mkdir(args, argc);
        }
        else if (strcmp(args[0], "rmdir") == 0)
        {
            command_rmdir(args, argc);
        }
        else if (strcmp(args[0], "listdir") == 0)
        {
            command_listdir(args, argc);
        }
        else if (strcmp(args[0], "create") == 0)
        {
            command_create(args, argc);
        }
        else if (strcmp(args[0], "unlink") == 0)
        {
            command_unlink(args, argc);
        }
        else if (strcmp(args[0], "open") == 0)
        {
            command_open(args, argc);
        }
        else if (strcmp(args[0], "close") == 0)
        {
            command_close(args, argc);
        }
        else if (strcmp(args[0], "read") == 0)
        {
            command_read(args, argc);
        }
        else if (strcmp(args[0], "seek") == 0)
        {
            command_seek(args, argc);
        }
        else if (strcmp(args[0], "truncate") == 0)
        {
            command_truncate(args, argc);
        }
        else if (strcmp(args[0], "stat") == 0)
        {
            command_stat(args, argc);
        }
        else if (strcmp(args[0], "fsck") == 0)
        {
            command_fsck(argc);
        }
        else if (strcmp(args[0], "help") == 0)
        {
            print_help();
        }
        else if (strcmp(args[0], "exit") == 0 ||
                 strcmp(args[0], "quit") == 0)
        {
            break;
        }
        else
        {
            printf("Unknown command: %s\n",
                   args[0]);

            printf("Type 'help' for available commands.\n");
        }
    }

    return 0;
}
