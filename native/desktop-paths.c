#include "desktop-session.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void set_error(char *error, size_t size, const char *format, ...) {
    if (!error || !size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

int desktop_runtime_directory(char *out, size_t size, char *error, size_t error_size) {
    const char *path = getenv("XDG_RUNTIME_DIR");
    if (!path || !*path) {
        set_error(error, error_size, "XDG_RUNTIME_DIR is required.");
        return -1;
    }
    struct stat status;
    if (stat(path, &status) < 0 || !S_ISDIR(status.st_mode) || status.st_uid != getuid() ||
            (status.st_mode & 077)) {
        set_error(error, error_size,
            "XDG_RUNTIME_DIR must be an owned directory with mode 0700.");
        return -1;
    }
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') length--;
    if (length + 1 > size) {
        set_error(error, error_size, "XDG_RUNTIME_DIR is too long.");
        return -1;
    }
    memcpy(out, path, length);
    out[length] = '\0';
    return 0;
}

bool desktop_socket_name_valid(const char *name) {
    if (!name) return false;
    size_t length = strlen(name);
    if (length < 1 || length > 60) return false;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.') continue;
        return false;
    }
    return true;
}

static int runtime_checked(char *out, size_t size, char *error, size_t error_size) {
    return desktop_runtime_directory(out, size, error, error_size);
}

int desktop_control_path(const char *name, char *out, size_t size, char *error,
        size_t error_size) {
    if (!desktop_socket_name_valid(name)) {
        set_error(error, error_size, "Invalid socket name: %s", name ? name : "");
        return -1;
    }
    char directory[PATH_MAX];
    if (runtime_checked(directory, sizeof(directory), error, error_size)) return -1;
    int written = snprintf(out, size, "%s/%s.ctl", directory, name);
    if (written < 0 || (size_t)written >= size) {
        set_error(error, error_size, "Socket path is too long.");
        return -1;
    }
    return 0;
}

int desktop_json_path(const char *name, char *out, size_t size, char *error, size_t error_size) {
    if (!desktop_socket_name_valid(name)) {
        set_error(error, error_size, "Invalid socket name: %s", name ? name : "");
        return -1;
    }
    char directory[PATH_MAX];
    if (runtime_checked(directory, sizeof(directory), error, error_size)) return -1;
    int written = snprintf(out, size, "%s/tomoe.%s.sock", directory, name);
    if (written < 0 || (size_t)written >= size) {
        set_error(error, error_size, "Socket path is too long.");
        return -1;
    }
    return 0;
}

int desktop_json_client_path(const char *explicit_name, char *out, size_t size, char *error,
        size_t error_size) {
    if (explicit_name && *explicit_name) return desktop_json_path(explicit_name, out, size,
        error, error_size);
    const char *socket = getenv("TOMOE_SOCKET");
    if (socket && *socket) {
        if (strlen(socket) + 1 > size) {
            set_error(error, error_size, "Socket path is too long.");
            return -1;
        }
        strcpy(out, socket);
        return 0;
    }
    const char *display = getenv("WAYLAND_DISPLAY");
    if (display && *display) return desktop_json_path(display, out, size, error, error_size);
    set_error(error, error_size, "Set TOMOE_SOCKET or WAYLAND_DISPLAY, or pass --socket NAME.");
    return -1;
}

static bool unix_address(const char *path, struct sockaddr_un *address) {
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address->sun_path)) return false;
    strcpy(address->sun_path, path);
    return true;
}

bool desktop_socket_answering(const char *path) {
    struct sockaddr_un address;
    if (!unix_address(path, &address)) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    bool answering = connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0;
    close(fd);
    return answering;
}

int desktop_connect_unix(const char *path, int timeout_seconds) {
    struct sockaddr_un address;
    if (!unix_address(path, &address)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    int result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (result < 0 && errno == EINPROGRESS) {
        struct timeval timeout = { .tv_sec = timeout_seconds, .tv_usec = 0 };
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(fd, &writable);
        FD_SET(fd, &failed);
        result = select(fd + 1, NULL, &writable, &failed, &timeout);
        if (result <= 0) {
            int error = result < 0 ? errno : ETIMEDOUT;
            close(fd);
            errno = error;
            return -1;
        }
        int status = 0;
        socklen_t length = sizeof(status);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &status, &length) < 0) {
            int error = errno;
            close(fd);
            errno = error;
            return -1;
        }
        if (status) {
            close(fd);
            errno = status;
            return -1;
        }
        result = 0;
    }
    if (result < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

int desktop_listen_unix(const char *path, int backlog, char *error, size_t error_size) {
    if (desktop_socket_answering(path)) {
        set_error(error, error_size, "Another session is listening on %s.", path);
        return -1;
    }
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        set_error(error, error_size, "Cannot open %s: %s.", path, strerror(errno));
        return -1;
    }
    struct sockaddr_un address;
    if (!unix_address(path, &address)) {
        set_error(error, error_size, "Socket path is too long.");
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        set_error(error, error_size, "Cannot bind %s: %s.", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (chmod(path, 0600) < 0 || listen(fd, backlog) < 0) {
        set_error(error, error_size, "Cannot listen on %s: %s.", path, strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}
