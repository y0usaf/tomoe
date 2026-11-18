#define _GNU_SOURCE
#include <errno.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int make_paths(const char *path) {
    char *copy = strdup(path);
    if (!copy) return -1;
    for (char *at = copy + 1; *at; at++) {
        if (*at != '/') continue;
        *at = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
            free(copy);
            return -1;
        }
        *at = '/';
    }
    int result = mkdir(copy, 0755);
    free(copy);
    if (result != 0 && errno != EEXIST) return -1;
    return 0;
}

static void write_number(const char *path, long value) {
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "service-probe: cannot write %s\n", path);
        exit(1);
    }
    fprintf(file, "%ld", value);
    if (fclose(file) != 0) exit(1);
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s SOCKET PID FORK CHILD\n", argv[0]);
        return 2;
    }
    char composed[4096];
    const char *socket_path = argv[1];
    const char *pid_path = argv[2];
    const char *forked = argv[3];
    const char *child_path = argv[4];
    if (strcmp(socket_path, "-") == 0) {
        const char *root = getenv("TOMOE_SERVICE_RUNTIME_DIR");
        if (!root) {
            fprintf(stderr, "TOMOE_SERVICE_RUNTIME_DIR is not set\n");
            return 1;
        }
        snprintf(composed, sizeof composed, "%s/probe.sock", root);
        socket_path = composed;
    }
    char directory[4096];
    snprintf(directory, sizeof directory, "%s", socket_path);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        if (directory[0] && make_paths(directory) != 0) {
            fprintf(stderr, "service-probe: cannot create %s: %s\n", directory, strerror(errno));
            return 1;
        }
    }
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
        fprintf(stderr, "service-probe: socket: %s\n", strerror(errno));
        return 1;
    }
    unlink(socket_path);
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    size_t path_length = strlen(socket_path);
    if (path_length >= sizeof address.sun_path) {
        fprintf(stderr, "socket path is too long\n");
        return 1;
    }
    memcpy(address.sun_path, socket_path, path_length + 1);
    if (bind(server, (struct sockaddr *)&address, sizeof address) != 0) {
        fprintf(stderr, "service-probe: bind %s: %s\n", socket_path, strerror(errno));
        return 1;
    }
    if (listen(server, 8) != 0) {
        fprintf(stderr, "service-probe: listen: %s\n", strerror(errno));
        return 1;
    }
    write_number(pid_path, (long)getpid());
    if (strcmp(forked, "1") == 0) {
        pid_t child = fork();
        if (child < 0) {
            fprintf(stderr, "service-probe: fork: %s\n", strerror(errno));
            return 1;
        }
        if (child == 0) {
            write_number(child_path, (long)getpid());
            for (;;) sleep(3600);
        }
    }
    for (;;) sleep(3600);
}
