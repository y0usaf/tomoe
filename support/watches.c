#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define TOMOE_WATCH_MAX_PATH 65536U
#define TOMOE_WATCH_MAX_CONTENT 65536U
#define TOMOE_WATCH_POLL_BYTES 32768U

enum watch_event {
    WATCH_CHANGED = 1,
    WATCH_OVERFLOW = 2,
    WATCH_LOST = 4,
    WATCH_RESTORED = 8
};

struct file_watch {
    int fd, wd;
    char *parent, *name, *path;
};

int tomoe_watch_abi(void) { return 1; }

static void deactivate(struct file_watch *watch) {
    if (watch->fd >= 0) close(watch->fd);
    watch->fd = watch->wd = -1;
}

void tomoe_watch_close(struct file_watch *watch) {
    if (!watch) return;
    deactivate(watch);
    free(watch->parent);
    free(watch->name);
    free(watch->path);
    free(watch);
}

static int activate(struct file_watch *watch) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return errno;
    int wd = inotify_add_watch(fd, watch->parent,
        IN_ONLYDIR | IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE_SELF |
        IN_MOVE_SELF | IN_UNMOUNT);
    if (wd < 0) {
        int error = errno;
        close(fd);
        return error;
    }
    watch->fd = fd;
    watch->wd = wd;
    return 0;
}

struct file_watch *tomoe_watch_open(const char *absolute_path, int *error) {
    struct file_watch *watch = NULL;
    char *parent = NULL;
    int result = EINVAL;
    if (!error) return NULL;
    if (!absolute_path || absolute_path[0] != '/') goto failed;
    size_t length = strnlen(absolute_path, TOMOE_WATCH_MAX_PATH + 1U);
    if (length > TOMOE_WATCH_MAX_PATH) {
        result = ENAMETOOLONG;
        goto failed;
    }
    const char *name = strrchr(absolute_path, '/') + 1;
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) goto failed;
    watch = calloc(1, sizeof(*watch));
    if (!watch) { result = ENOMEM; goto failed; }
    watch->fd = watch->wd = -1;
    size_t parent_length = (size_t)(name - absolute_path - 1);
    parent = strndup(absolute_path, parent_length ? parent_length : 1);
    watch->name = strdup(name);
    if (!parent || !watch->name) { result = ENOMEM; goto failed; }
    watch->parent = realpath(parent, NULL);
    if (!watch->parent) { result = errno; goto failed; }
    free(parent);
    parent = NULL;
    parent_length = strlen(watch->parent);
    size_t name_length = strlen(watch->name);
    size_t separator = parent_length == 1 ? 0 : 1;
    if (parent_length > TOMOE_WATCH_MAX_PATH ||
            name_length + separator > TOMOE_WATCH_MAX_PATH - parent_length) {
        result = ENAMETOOLONG;
        goto failed;
    }
    watch->path = malloc(parent_length + separator + name_length + 1);
    if (!watch->path) { result = ENOMEM; goto failed; }
    memcpy(watch->path, watch->parent, parent_length);
    if (separator) watch->path[parent_length] = '/';
    memcpy(watch->path + parent_length + separator, watch->name, name_length + 1);
    result = activate(watch);
    if (result) goto failed;
    *error = 0;
    return watch;

failed:
    free(parent);
    tomoe_watch_close(watch);
    *error = result;
    return NULL;
}

int tomoe_watch_poll(struct file_watch *watch) {
    if (!watch) return -EINVAL;
    if (watch->fd < 0) {
        int error = activate(watch);
        if (error == ENOENT || error == ENOTDIR) return 0;
        return error ? -error : WATCH_RESTORED;
    }
    _Alignas(struct inotify_event) unsigned char buffer[TOMOE_WATCH_POLL_BYTES];
    ssize_t count = read(watch->fd, buffer, sizeof(buffer));
    if (count < 0) return errno == EAGAIN ? 0 : -errno;
    if (count == 0) return -EIO;
    int result = 0;
    for (size_t offset = 0; offset < (size_t)count;) {
        if ((size_t)count - offset < sizeof(struct inotify_event)) return -EIO;
        const struct inotify_event *event =
            (const struct inotify_event *)(buffer + offset);
        size_t size = sizeof(*event) + (size_t)event->len;
        if (size > (size_t)count - offset) return -EIO;
        if (event->mask & IN_Q_OVERFLOW) result |= WATCH_OVERFLOW;
        if (event->wd == watch->wd) {
            if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_IGNORED))
                result |= WATCH_LOST;
            if (!(event->mask & IN_ISDIR) &&
                    (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) &&
                    event->len && memchr(event->name, '\0', event->len) &&
                    !strcmp(event->name, watch->name))
                result |= WATCH_CHANGED;
        }
        offset += size;
    }
    if (result & WATCH_LOST) {
        deactivate(watch);
    } else if (result & WATCH_OVERFLOW) {
        deactivate(watch);
        if (activate(watch)) result |= WATCH_LOST;
    }
    return result;
}

long tomoe_watch_read(struct file_watch *watch, unsigned char *buffer,
                     unsigned long limit) {
    if (!watch || !buffer || !limit || limit > TOMOE_WATCH_MAX_CONTENT)
        return -EINVAL;
    int fd = open(watch->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return -errno;
    struct stat status;
    int error = 0;
    size_t total = 0;
    if (fstat(fd, &status) < 0) {
        error = errno;
    } else if (!S_ISREG(status.st_mode)) {
        error = EINVAL;
    } else if (status.st_size > (off_t)limit) {
        error = EFBIG;
    } else {
        while (total <= limit) {
            ssize_t count = read(fd, buffer + total, (size_t)limit + 1 - total);
            if (count < 0) { error = errno; break; }
            if (!count) break;
            total += (size_t)count;
        }
        if (!error && total > limit) error = EFBIG;
    }
    close(fd);
    return error ? -(long)error : (long)total;
}
