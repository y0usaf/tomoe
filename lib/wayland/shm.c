#include <errno.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "wayland.h"
#include "wayland-server-protocol.h"
#include "server.h"

struct wl_shm_pool {
    struct wl_resource *resource;
    int internal_refcount;
    int external_refcount;
    char *data;
    ssize_t size;
    ssize_t new_size;
    bool sigbus_is_impossible;
    int fd;
    struct wl_shm_view *view;
};

struct wl_shm_view {
    char *data;
    size_t size;
    int refs;
};

struct wl_shm_buffer {
    struct wl_resource *resource;
    struct wl_client *client;
    struct wl_listener client_destroy_listener;
    int32_t width, height;
    int32_t stride;
    uint32_t format;
    int offset;
    struct wl_shm_pool *pool;
};

static _Thread_local struct {
    struct wl_shm_pool *pool;
    int count;
    bool fallback;
} sigbus;
static _Thread_local struct {
    sigjmp_buf env;
    const char *low, *high;
    volatile bool on;
} guard;
static struct sigaction previous_sigbus;
static bool sigbus_installed;

static void finish_resize(struct wl_shm_pool *pool) {
    if (pool->size == pool->new_size) return;
    void *data = mremap(pool->data, (size_t)pool->size, (size_t)pool->new_size, MREMAP_MAYMOVE);
    if (data == MAP_FAILED) {
        if (pool->resource) wl_resource_post_error(pool->resource, WL_SHM_ERROR_INVALID_FD, "failed mremap");
        return;
    }
    pool->data = data;
    pool->size = pool->new_size;
}

static void pool_unref(struct wl_shm_pool *pool, bool external) {
    if (external) {
        if (--pool->external_refcount < 0) abort();
        if (pool->external_refcount == 0) finish_resize(pool);
    } else if (--pool->internal_refcount < 0) {
        abort();
    }
    if (pool->internal_refcount + pool->external_refcount > 0) return;
    munmap(pool->data, (size_t)pool->size);
    wl_shm_view_unref(pool->view);
    if (pool->fd >= 0) close(pool->fd);
    free(pool);
}

static void destroy_buffer(struct wl_resource *resource) {
    struct wl_shm_buffer *buffer = wl_resource_get_user_data(resource);
    if (buffer->client) wl_list_remove(&buffer->client_destroy_listener.link);
    pool_unref(buffer->pool, false);
    free(buffer);
}

static void buffer_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface buffer_implementation = { buffer_destroy };

static bool format_supported(struct wl_client *client, uint32_t format) {
    if (format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888) return true;
    const struct wl_array *formats = server_shm_formats(wl_client_get_display(client));
    const uint32_t *p;
    wl_array_for_each(p, formats)
        if (*p == format) return true;
    return false;
}

static void buffer_client_destroyed(struct wl_listener *listener, void *data) {
    struct wl_shm_buffer *buffer = wl_container_of(listener, buffer, client_destroy_listener);
    (void)data;
    buffer->client = NULL;
    wl_list_remove(&buffer->client_destroy_listener.link);
}

static void pool_create_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id,
        int32_t offset, int32_t width, int32_t height, int32_t stride, uint32_t format) {
    struct wl_shm_pool *pool = wl_resource_get_user_data(resource);
    if (!format_supported(client, format)) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FORMAT, "invalid format 0x%x", format);
        return;
    }
    if (offset < 0 || width <= 0 || height <= 0 || stride < width || INT32_MAX / stride < height ||
            offset > pool->size - (ssize_t)stride * height) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_STRIDE,
            "invalid width, height or stride (%dx%d, %u)", width, height, stride);
        return;
    }
    struct wl_shm_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        wl_client_post_no_memory(client);
        return;
    }
    buffer->client = client;
    buffer->client_destroy_listener.notify = buffer_client_destroyed;
    wl_client_add_destroy_listener(client, &buffer->client_destroy_listener);
    buffer->width = width;
    buffer->height = height;
    buffer->format = format;
    buffer->stride = stride;
    buffer->offset = offset;
    buffer->pool = pool;
    pool->internal_refcount++;
    buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer->resource) {
        wl_client_post_no_memory(client);
        wl_list_remove(&buffer->client_destroy_listener.link);
        pool_unref(pool, false);
        free(buffer);
        return;
    }
    wl_resource_set_implementation(buffer->resource, &buffer_implementation, buffer, destroy_buffer);
}

static void destroy_pool(struct wl_resource *resource) {
    struct wl_shm_pool *pool = wl_resource_get_user_data(resource);
    pool->resource = NULL;
    pool_unref(pool, false);
}

static void pool_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void pool_resize(struct wl_client *client, struct wl_resource *resource, int32_t size) {
    struct wl_shm_pool *pool = wl_resource_get_user_data(resource);
    (void)client;
    if (size < pool->size) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD, "shrinking pool invalid");
        return;
    }
    pool->new_size = size;
    if (pool->external_refcount == 0) finish_resize(pool);
}

static const struct wl_shm_pool_interface pool_implementation = {
    pool_create_buffer, pool_destroy, pool_resize,
};

static void shm_create_pool(struct wl_client *client, struct wl_resource *resource, uint32_t id, int fd,
        int32_t size) {
    if (size <= 0) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_STRIDE, "invalid size (%d)", size);
        close(fd);
        return;
    }
    struct wl_shm_pool *pool = calloc(1, sizeof(*pool));
    if (!pool) {
        wl_client_post_no_memory(client);
        close(fd);
        return;
    }
    int seals = fcntl(fd, F_GET_SEALS);
    if (seals == -1) seals = 0;
    struct stat st;
    pool->sigbus_is_impossible = (seals & F_SEAL_SHRINK) && fstat(fd, &st) >= 0 && st.st_size >= size;
    pool->internal_refcount = 1;
    pool->fd = -1;
    pool->size = pool->new_size = size;
    pool->data = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pool->data == MAP_FAILED) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD, "failed mmap fd %d: %s", fd, strerror(errno));
        free(pool);
        close(fd);
        return;
    }
    pool->fd = fd;
    pool->resource = wl_resource_create(client, &wl_shm_pool_interface, wl_resource_get_version(resource), id);
    if (!pool->resource) {
        wl_client_post_no_memory(client);
        munmap(pool->data, (size_t)pool->size);
        close(fd);
        free(pool);
        return;
    }
    wl_resource_set_implementation(pool->resource, &pool_implementation, pool, destroy_pool);
}

static void shm_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_shm_interface shm_implementation = { shm_create_pool, shm_release };

static void bind_shm(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &wl_shm_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &shm_implementation, data, NULL);
    wl_shm_send_format(resource, WL_SHM_FORMAT_ARGB8888);
    wl_shm_send_format(resource, WL_SHM_FORMAT_XRGB8888);
    const uint32_t *p;
    const struct wl_array *formats = server_shm_formats(wl_client_get_display(client));
    wl_array_for_each(p, formats) wl_shm_send_format(resource, *p);
}

int wl_display_init_shm(struct wl_display *display) {
    return wl_global_create(display, &wl_shm_interface, 2, NULL, bind_shm) ? 0 : -1;
}

struct wl_shm_buffer *wl_shm_buffer_get(struct wl_resource *resource) {
    if (resource && wl_resource_instance_of(resource, &wl_buffer_interface, &buffer_implementation))
        return wl_resource_get_user_data(resource);
    return NULL;
}

int32_t wl_shm_buffer_get_stride(const struct wl_shm_buffer *buffer) { return buffer->stride; }
uint32_t wl_shm_buffer_get_format(const struct wl_shm_buffer *buffer) { return buffer->format; }
int32_t wl_shm_buffer_get_width(const struct wl_shm_buffer *buffer) { return buffer->width; }
int32_t wl_shm_buffer_get_height(const struct wl_shm_buffer *buffer) { return buffer->height; }

void *wl_shm_buffer_get_data(struct wl_shm_buffer *buffer) {
    return buffer->pool->data + buffer->offset;
}

struct wl_shm_pool *wl_shm_buffer_ref_pool(struct wl_shm_buffer *buffer) {
    buffer->pool->external_refcount++;
    return buffer->pool;
}

void wl_shm_pool_unref(struct wl_shm_pool *pool) {
    pool_unref(pool, true);
}

static void reraise_sigbus(void) {
    sigaction(SIGBUS, &previous_sigbus, NULL);
    raise(SIGBUS);
}

static void sigbus_handler(int signum, siginfo_t *info, void *context) {
    (void)signum;
    (void)context;
    if (guard.on && (const char *)info->si_addr >= guard.low && (const char *)info->si_addr < guard.high) {
        guard.on = false;
        siglongjmp(guard.env, 1);
    }
    struct wl_shm_pool *pool = sigbus.pool;
    if (!pool || (char *)info->si_addr < pool->data || (char *)info->si_addr >= pool->data + pool->size) {
        reraise_sigbus();
        return;
    }
    sigbus.fallback = true;
    if (mmap(pool->data, (size_t)pool->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS,
            -1, 0) == MAP_FAILED) reraise_sigbus();
}

static void install_sigbus(void) {
    if (sigbus_installed) return;
    struct sigaction action = { .sa_sigaction = sigbus_handler, .sa_flags = SA_SIGINFO | SA_NODEFER };
    sigemptyset(&action.sa_mask);
    sigaction(SIGBUS, &action, &previous_sigbus);
    sigbus_installed = true;
}

void wl_shm_prepare_threads(void) {
    install_sigbus();
}

void wl_shm_view_unref(struct wl_shm_view *view) {
    if (!view || --view->refs > 0) return;
    munmap(view->data, view->size);
    free(view);
}

struct wl_shm_view *wl_shm_buffer_view(struct wl_shm_buffer *buffer, size_t *offset) {
    struct wl_shm_pool *pool = buffer->pool;
    size_t end = (size_t)buffer->offset + (size_t)buffer->stride * (size_t)buffer->height;
    if (pool->fd < 0) return NULL;
    if (!pool->view || pool->view->size < end) {
        struct wl_shm_view *view = calloc(1, sizeof(*view));
        if (!view) return NULL;
        view->size = (size_t)(pool->new_size > pool->size ? pool->new_size : pool->size);
        if (view->size < end) {
            free(view);
            return NULL;
        }
        view->data = mmap(NULL, view->size, PROT_READ, MAP_SHARED, pool->fd, 0);
        if (view->data == MAP_FAILED) {
            free(view);
            return NULL;
        }
        view->refs = 1;
        wl_shm_view_unref(pool->view);
        pool->view = view;
    }
    pool->view->refs++;
    *offset = (size_t)buffer->offset;
    return pool->view;
}

bool wl_shm_view_copy(const struct wl_shm_view *view, size_t offset, size_t stride, size_t row,
        int rows, void *dst) {
    if (rows <= 0) return true;
    if (offset > view->size || stride * (size_t)(rows - 1) + row > view->size - offset) return false;
    guard.low = view->data;
    guard.high = view->data + view->size;
    if (sigsetjmp(guard.env, 0)) return false;
    guard.on = true;
    atomic_signal_fence(memory_order_seq_cst);
    const char *src = view->data + offset;
    char *out = dst;
    for (int y = 0; y < rows; y++)
        memcpy(out + (size_t)y * row, src + (size_t)y * stride, row);
    atomic_signal_fence(memory_order_seq_cst);
    guard.on = false;
    return true;
}

void wl_shm_buffer_post_access_error(struct wl_shm_buffer *buffer) {
    if (buffer->resource)
        wl_resource_post_error(buffer->resource, WL_SHM_ERROR_INVALID_FD, "error accessing SHM buffer");
    else if (buffer->client)
        wl_client_post_implementation_error(buffer->client,
            "Error accessing SHM buffer of a wl_buffer resource which has already been destroyed");
}

void wl_shm_buffer_begin_access(struct wl_shm_buffer *buffer) {
    struct wl_shm_pool *pool = buffer->pool;
    if (pool->sigbus_is_impossible) return;
    install_sigbus();
    if (sigbus.pool && sigbus.pool != pool) abort();
    sigbus.pool = pool;
    sigbus.count++;
}

void wl_shm_buffer_end_access(struct wl_shm_buffer *buffer) {
    struct wl_shm_pool *pool = buffer->pool;
    if (pool->sigbus_is_impossible) return;
    if (sigbus.count < 1) abort();
    if (--sigbus.count == 0) {
        if (sigbus.fallback) {
            if (buffer->resource)
                wl_resource_post_error(buffer->resource, WL_SHM_ERROR_INVALID_FD, "error accessing SHM buffer");
            else if (buffer->client)
                wl_client_post_implementation_error(buffer->client,
                    "Error accessing SHM buffer of a wl_buffer resource which has already been destroyed");
            sigbus.fallback = false;
        }
        sigbus.pool = NULL;
    }
}
