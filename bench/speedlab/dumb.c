#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/sync_file.h>
#include <time.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#define OBJECTS 4096
#define HANDLE_BASE 0x5d000000u
#define OFFSET_BASE 0x7e0000000000ull
#define OFFSET_STRIDE 0x100000000ull

struct object {
    bool used;
    uint32_t handle;
    int fd;
    ino_t ino;
    uint64_t size;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct object objects[OBJECTS];
static int (*real_ioctl)(int, unsigned long, ...);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);

static bool drm_fd(int fd) {
    struct stat st;
    return fd >= 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 226;
}

static struct object *by_handle(uint32_t handle) {
    if (handle < HANDLE_BASE || handle >= HANDLE_BASE + OBJECTS) return NULL;
    struct object *o = &objects[handle - HANDLE_BASE];
    return o->used ? o : NULL;
}

static struct object *fresh(void) {
    for (int i = 0; i < OBJECTS; i++)
        if (!objects[i].used) {
            objects[i] = (struct object){ .used = true, .handle = HANDLE_BASE + (uint32_t)i, .fd = -1 };
            return &objects[i];
        }
    return NULL;
}

static int create_dumb(struct drm_mode_create_dumb *args) {
    uint64_t pitch = ((uint64_t)args->width * ((args->bpp + 7) / 8) + 63) & ~63ull;
    uint64_t size = (pitch * args->height + 4095) & ~4095ull;
    int fd = (int)syscall(SYS_memfd_create, "speedlab-dumb", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    pthread_mutex_lock(&lock);
    struct object *o = fresh();
    if (!o) {
        pthread_mutex_unlock(&lock);
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    struct stat st;
    fstat(fd, &st);
    o->fd = fd;
    o->ino = st.st_ino;
    o->size = size;
    args->handle = o->handle;
    args->pitch = (uint32_t)pitch;
    args->size = size;
    pthread_mutex_unlock(&lock);
    return 0;
}

static int release(uint32_t handle) {
    pthread_mutex_lock(&lock);
    struct object *o = by_handle(handle);
    if (o) {
        close(o->fd);
        o->used = false;
    }
    pthread_mutex_unlock(&lock);
    if (!o) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int map_dumb(struct drm_mode_map_dumb *args) {
    pthread_mutex_lock(&lock);
    struct object *o = by_handle(args->handle);
    pthread_mutex_unlock(&lock);
    if (!o) {
        errno = EINVAL;
        return -1;
    }
    args->offset = OFFSET_BASE + (uint64_t)(args->handle - HANDLE_BASE) * OFFSET_STRIDE;
    return 0;
}

static int handle_to_fd(struct drm_prime_handle *args) {
    pthread_mutex_lock(&lock);
    struct object *o = by_handle(args->handle);
    int fd = o ? fcntl(o->fd, (args->flags & DRM_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 0) : -1;
    pthread_mutex_unlock(&lock);
    if (!o) {
        errno = ENOENT;
        return -1;
    }
    if (fd < 0) return -1;
    args->fd = fd;
    return 0;
}

static int fd_to_handle(struct drm_prime_handle *args) {
    struct stat st;
    if (fstat(args->fd, &st) < 0) return -1;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < OBJECTS; i++)
        if (objects[i].used && objects[i].ino == st.st_ino) {
            args->handle = objects[i].handle;
            pthread_mutex_unlock(&lock);
            return 0;
        }
    struct object *o = fresh();
    int fd = o ? fcntl(args->fd, F_DUPFD_CLOEXEC, 0) : -1;
    if (fd < 0) {
        if (o) o->used = false;
        pthread_mutex_unlock(&lock);
        return -1;
    }
    o->fd = fd;
    o->ino = st.st_ino;
    off_t end = lseek(fd, 0, SEEK_END);
    o->size = end > 0 ? (uint64_t)end : 0;
    args->handle = o->handle;
    pthread_mutex_unlock(&lock);
    return 0;
}

int ioctl(int fd, unsigned long request, ...) {
    va_list list;
    va_start(list, request);
    void *arg = va_arg(list, void *);
    va_end(list);
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    if (_IOC_TYPE(request) == DRM_IOCTL_BASE) {
        switch (request) {
        case DRM_IOCTL_MODE_CREATE_DUMB:
            if (drm_fd(fd)) return create_dumb(arg);
            break;
        case DRM_IOCTL_MODE_MAP_DUMB:
            if (by_handle(((struct drm_mode_map_dumb *)arg)->handle)) return map_dumb(arg);
            break;
        case DRM_IOCTL_MODE_DESTROY_DUMB:
            if (by_handle(((struct drm_mode_destroy_dumb *)arg)->handle))
                return release(((struct drm_mode_destroy_dumb *)arg)->handle);
            break;
        case DRM_IOCTL_GEM_CLOSE:
            if (by_handle(((struct drm_gem_close *)arg)->handle))
                return release(((struct drm_gem_close *)arg)->handle);
            break;
        case DRM_IOCTL_PRIME_HANDLE_TO_FD:
            if (by_handle(((struct drm_prime_handle *)arg)->handle)) return handle_to_fd(arg);
            break;
        case DRM_IOCTL_PRIME_FD_TO_HANDLE:
            if (drm_fd(fd)) return fd_to_handle(arg);
            break;
        case DRM_IOCTL_GET_CAP: {
            struct drm_get_cap *cap = arg;
            if (cap->capability == DRM_CAP_DUMB_BUFFER) {
                cap->value = 1;
                return 0;
            }
            if (cap->capability == DRM_CAP_PRIME) {
                cap->value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
                return 0;
            }
            break;
        }
        default:
            break;
        }
    }
    if (request == SYNC_IOC_FILE_INFO) {
        struct sync_file_info *info = arg;
        uint32_t cap = info ? info->num_fences : 0;
        int ret = real_ioctl(fd, request, arg);
        if (ret == 0 && info && info->status == 1 && info->sync_fence_info && cap) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
            struct sync_fence_info *fences = (struct sync_fence_info *)(uintptr_t)info->sync_fence_info;
            uint32_t count = info->num_fences < cap ? info->num_fences : cap;
            for (uint32_t i = 0; i < count; i++)
                if (fences[i].status == 1) fences[i].timestamp_ns = now;
        }
        return ret;
    }
    return real_ioctl(fd, request, arg);
}

void *mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset) {
    if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");
    uint64_t at = (uint64_t)offset;
    if (fd >= 0 && at >= OFFSET_BASE && (at - OFFSET_BASE) % OFFSET_STRIDE == 0) {
        uint32_t handle = HANDLE_BASE + (uint32_t)((at - OFFSET_BASE) / OFFSET_STRIDE);
        pthread_mutex_lock(&lock);
        struct object *o = by_handle(handle);
        int memfd = o ? o->fd : -1;
        pthread_mutex_unlock(&lock);
        if (memfd >= 0 && drm_fd(fd)) return real_mmap(address, length, prot, flags, memfd, 0);
    }
    return real_mmap(address, length, prot, flags, fd, offset);
}

void *mmap64(void *address, size_t length, int prot, int flags, int fd, off_t offset) {
    return mmap(address, length, prot, flags, fd, offset);
}
