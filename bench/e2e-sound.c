#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define listen tomoe_internal_listen
#include "internal.h"
#undef listen
#include "pipewire.h"
#include "xdg-system-bell-v1-protocol.h"

enum {
    MEM_SIZE = 65536,
    OFF_ACT_CLIENT = 0x0000,
    OFF_ACT_TARGET = 0x0400,
    OFF_POS = 0x0840,
    OFF_CLOCK = 0x0940,
    OFF_P0_IO = 0x0a40,
    OFF_P0_RM = 0x0a80,
    OFF_P1_IO = 0x0b40,
    OFF_P1_RM = 0x0b80,
    OFF_P0_B0 = 0x1040,
    OFF_P0_B1 = 0x2080,
    OFF_P1_B0 = 0x30c0,
    OFF_P1_B1 = 0x4100,
    OFF_UNKNOWN_IO = 0x5140,
    BUF_DATA_OFF = 64,
    BUF_MAXSIZE = 4096,
    BUF_REGION = 4160,
    QUANTUM = 128,
};

struct port_io_wire {
    uint32_t status;
    uint32_t buffer_id;
};

struct ratematch_wire {
    uint32_t delay;
    uint32_t size;
    double rate;
    uint32_t flags;
    int32_t delay_frac;
};

struct mock_pw {
    int listen_fd;
    int memfd;
    uint8_t *mem;
    struct pww wire;
    int process_fd;
    int complete_fd;
    int target_fd;
    int accepts;
    int saw_sync;
    uint32_t ping_seq;
    uint32_t pong_seq;
    int active_events[256];
    int n_active_events;
    int last_active;
};

const struct wl_interface wl_surface_interface = { 0 };

struct wl_global *wl_global_create(struct wl_display *display, const struct wl_interface *interface,
        int version, void *data, wl_global_bind_func_t bind) {
    (void)display;
    (void)interface;
    (void)version;
    (void)data;
    (void)bind;
    return NULL;
}

void wl_client_post_no_memory(struct wl_client *client) {
    (void)client;
}

struct wl_resource *wl_resource_create(struct wl_client *client, const struct wl_interface *interface,
        int version, uint32_t id) {
    (void)client;
    (void)interface;
    (void)version;
    (void)id;
    return NULL;
}

void wl_resource_set_implementation(struct wl_resource *resource, const void *implementation,
        void *data, wl_resource_destroy_func_t destroy) {
    (void)resource;
    (void)implementation;
    (void)data;
    (void)destroy;
}

void wl_resource_destroy(struct wl_resource *resource) {
    (void)resource;
}

void *wl_resource_get_user_data(struct wl_resource *resource) {
    (void)resource;
    return NULL;
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int count_open_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        n++;
    }
    closedir(d);
    return n;
}

static int count_vma_maps(void) {
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) return -1;
    int n = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF)
        if (ch == '\n') n++;
    fclose(f);
    return n;
}

static void die(const char *msg) {
    fprintf(stderr, "e2e-sound: FAIL: %s\n", msg);
    exit(1);
}

static void write_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void write_test_wav(const char *path) {
    uint32_t frames = 64;
    uint32_t data_size = frames * 2 * sizeof(int16_t);
    uint32_t total = 44 + data_size;
    uint8_t *buf = calloc(1, total);
    if (!buf) die("calloc wav");
    memcpy(buf, "RIFF", 4);
    write_u32_le(buf + 4, total - 8);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    write_u32_le(buf + 16, 16);
    write_u16_le(buf + 20, 1);
    write_u16_le(buf + 22, 2);
    write_u32_le(buf + 24, 48000);
    write_u32_le(buf + 28, 48000 * 4);
    write_u16_le(buf + 32, 4);
    write_u16_le(buf + 34, 16);
    memcpy(buf + 36, "data", 4);
    write_u32_le(buf + 40, data_size);
    for (uint32_t i = 0; i < frames; i++) {
        int16_t v = (i & 1) ? 16000 : -16000;
        write_u16_le(buf + 44 + i * 4, (uint16_t)v);
        write_u16_le(buf + 44 + i * 4 + 2, (uint16_t)v);
    }
    FILE *f = fopen(path, "wbe");
    if (!f || fwrite(buf, 1, total, f) != total) die("write wav");
    fclose(f);
    free(buf);
}

static int mock_dispatch(void *data, uint32_t id, uint32_t opcode, struct pww_parse *p) {
    struct mock_pw *m = data;
    if (id == 0 && opcode == PW_CORE_METHOD_SYNC) {
        m->saw_sync = 1;
        return 0;
    }
    if (id == 0 && opcode == PW_CORE_METHOD_PONG) {
        uint32_t obj_id = 0, seq = 0;
        if (pww_parse_int(p, &obj_id) || pww_parse_int(p, &seq)) return -EPROTO;
        m->pong_seq = seq;
        return 0;
    }
    if (id == 2 && opcode == PW_CLIENT_NODE_METHOD_SET_ACTIVE) {
        uint32_t v = 0;
        if (pww_parse_typed(p, SPA_TYPE_BOOL, &v, 4) < 0) return -EPROTO;
        m->last_active = v ? 1 : 0;
        if (m->n_active_events < 256)
            m->active_events[m->n_active_events++] = m->last_active;
        return 0;
    }
    return 0;
}

static void mock_close_client(struct mock_pw *m) {
    if (m->wire.fd >= 0) close(m->wire.fd);
    m->wire.fd = -1;
    for (size_t i = 0; i < m->wire.n_fds; i++)
        if (m->wire.fds[i] >= 0) close(m->wire.fds[i]);
    m->wire.n_fds = 0;
    m->wire.in_len = 0;
    m->wire.seq = 0;
    if (m->process_fd >= 0) close(m->process_fd);
    if (m->complete_fd >= 0) close(m->complete_fd);
    if (m->target_fd >= 0) close(m->target_fd);
    m->process_fd = -1;
    m->complete_fd = -1;
    m->target_fd = -1;
    m->saw_sync = 0;
    m->pong_seq = 0;
    m->n_active_events = 0;
    m->last_active = -1;
}

static void mock_init(struct mock_pw *m, const char *sock_path) {
    memset(m, 0, sizeof(*m));
    m->listen_fd = -1;
    m->memfd = -1;
    m->wire.fd = -1;
    m->process_fd = -1;
    m->complete_fd = -1;
    m->target_fd = -1;
    m->last_active = -1;

    m->memfd = memfd_create("pw-sound-test", MFD_CLOEXEC);
    if (m->memfd < 0 || ftruncate(m->memfd, MEM_SIZE) < 0) die("memfd_create");
    m->mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m->memfd, 0);
    if (m->mem == MAP_FAILED) die("mmap memfd");

    uint32_t clock_id = 7, num = 1, denom = 48000, duration = QUANTUM;
    memcpy(m->mem + OFF_POS + 4, &clock_id, 4);
    memcpy(m->mem + OFF_POS + SPA_CLOCK_RATE, &num, 4);
    memcpy(m->mem + OFF_POS + SPA_CLOCK_RATE + 4, &denom, 4);
    memcpy(m->mem + OFF_POS + SPA_CLOCK_DURATION, &duration, 4);

    struct ratematch_wire rm = { 0, QUANTUM, 1.0, 0, 0 };
    memcpy(m->mem + OFF_P0_RM, &rm, sizeof(rm));
    memcpy(m->mem + OFF_P1_RM, &rm, sizeof(rm));

    m->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (m->listen_fd < 0) die("socket");
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) die("sock_path too long");
    strcpy(addr.sun_path, sock_path);
    unlink(sock_path);
    if (bind(m->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(m->listen_fd, 4) < 0) die("listen");
}

static void mock_destroy(struct mock_pw *m) {
    mock_close_client(m);
    free(m->wire.in);
    m->wire.in = NULL;
    m->wire.in_cap = 0;
    if (m->listen_fd >= 0) close(m->listen_fd);
    if (m->mem && m->mem != MAP_FAILED) munmap(m->mem, MEM_SIZE);
    if (m->memfd >= 0) close(m->memfd);
}

static void mock_pump(struct mock_pw *m, int timeout_ms) {
    struct pollfd p = { .fd = m->wire.fd, .events = POLLIN };
    int r = poll(&p, 1, timeout_ms);
    if (r < 0 && errno != EINTR) die("poll wire");
    if (r > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
        int res = pww_recv(&m->wire, mock_dispatch, m);
        if (res < 0) die("pww_recv client");
    }
}

static void mock_accept(struct mock_pw *m, int timeout_ms) {
    mock_close_client(m);
    uint64_t deadline = monotonic_ms() + (uint64_t)timeout_ms;
    for (;;) {
        uint64_t now = monotonic_ms();
        int rem = now < deadline ? (int)(deadline - now) : 0;
        struct pollfd p = { .fd = m->listen_fd, .events = POLLIN };
        int r = poll(&p, 1, rem);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) die("timeout waiting for pipewire client accept");
        int fd = accept4(m->listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            die("accept4");
        }
        m->wire.fd = fd;
        m->accepts++;
        break;
    }
    while (!m->saw_sync) {
        if (monotonic_ms() >= deadline) die("timeout waiting for initial SYNC");
        mock_pump(m, 20);
    }
    if (m->last_active != 1) die("initial client_setup did not send SET_ACTIVE(1)");
    m->n_active_events = 0;
}

static void mock_send_bound(struct mock_pw *m) {
    uint8_t storage[256];
    struct pww_build b;
    struct pww_frame root;
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, 2);
    pww_pod_int(&b, 96);
    if (pww_message_send(&m->wire, &b, &root, 0, PW_CORE_EVENT_BOUND_ID) < 0) die("send BOUND_ID");
}

static void mock_send_add_mem(struct mock_pw *m, uint32_t mem_id) {
    uint8_t storage[256];
    struct pww_build b;
    struct pww_frame root;
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, (int32_t)mem_id);
    pww_pod_id(&b, 1);
    pww_pod_fd(&b, 0);
    pww_pod_int(&b, 0);
    pww_pod_struct_end(&b, &root);
    if (pww_send_message_fds(&m->wire, 0, PW_CORE_EVENT_ADD_MEM, &b, &m->memfd, 1) < 0)
        die("send ADD_MEM");
}

static void mock_send_transport(struct mock_pw *m, uint32_t mem_id) {
    if (m->process_fd >= 0) close(m->process_fd);
    if (m->complete_fd >= 0) close(m->complete_fd);
    m->process_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    m->complete_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m->process_fd < 0 || m->complete_fd < 0) die("eventfd transport");
    int fds[2] = { m->process_fd, m->complete_fd };
    uint8_t storage[256];
    struct pww_build b;
    struct pww_frame root;
    pww_message_begin(&b, &root, storage);
    pww_pod_fd(&b, 0);
    pww_pod_fd(&b, 1);
    pww_pod_int(&b, (int32_t)mem_id);
    pww_pod_int(&b, OFF_ACT_CLIENT);
    pww_pod_int(&b, 1024);
    pww_pod_struct_end(&b, &root);
    if (pww_send_message_fds(&m->wire, 2, PW_CLIENT_NODE_EVENT_TRANSPORT, &b, fds, 2) < 0)
        die("send TRANSPORT");
}

static void mock_send_io(struct mock_pw *m, uint32_t io_id, uint32_t mem_id, uint32_t off, uint32_t size) {
    uint8_t storage[256];
    struct pww_build b;
    struct pww_frame root;
    pww_message_begin(&b, &root, storage);
    pww_pod_id(&b, io_id);
    pww_pod_int(&b, (int32_t)mem_id);
    pww_pod_int(&b, (int32_t)off);
    pww_pod_int(&b, (int32_t)size);
    if (pww_message_send(&m->wire, &b, &root, 2, PW_CLIENT_NODE_EVENT_SET_IO) < 0)
        die("send SET_IO");
}

static void mock_send_port_io(struct mock_pw *m, uint32_t port, uint32_t io_id, uint32_t mem_id,
        uint32_t off, uint32_t size) {
    uint8_t storage[256];
    struct pww_build b;
    struct pww_frame root;
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, SPA_DIRECTION_OUTPUT);
    pww_pod_int(&b, (int32_t)port);
    pww_pod_int(&b, 0);
    pww_pod_id(&b, io_id);
    pww_pod_int(&b, (int32_t)mem_id);
    pww_pod_int(&b, (int32_t)off);
    pww_pod_int(&b, (int32_t)size);
    if (pww_message_send(&m->wire, &b, &root, 2, PW_CLIENT_NODE_EVENT_PORT_SET_IO) < 0)
        die("send PORT_SET_IO");
}

static void mock_send_activation(struct mock_pw *m, uint32_t node_id, uint32_t mem_id,
        uint32_t off, uint32_t size, int keep_fd) {
    uint8_t storage[256];
    struct pww_build b;
    struct pww_frame root;
    int fd = -1;
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, (int32_t)node_id);
    if (mem_id != SPA_ID_INVALID) {
        fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (fd < 0) die("eventfd activation");
        pww_pod_fd(&b, 0);
    } else {
        pww_pod_fd(&b, -1);
    }
    pww_pod_int(&b, (int32_t)mem_id);
    pww_pod_int(&b, (int32_t)off);
    pww_pod_int(&b, (int32_t)size);
    pww_pod_struct_end(&b, &root);
    if (mem_id != SPA_ID_INVALID) {
        if (pww_send_message_fds(&m->wire, 2, PW_CLIENT_NODE_EVENT_SET_ACTIVATION, &b, &fd, 1) < 0)
            die("send SET_ACTIVATION");
        if (keep_fd) {
            if (m->target_fd >= 0) close(m->target_fd);
            m->target_fd = fd;
        } else {
            close(fd);
        }
    } else {
        if (pww_send_message(&m->wire, 2, PW_CLIENT_NODE_EVENT_SET_ACTIVATION, &b) < 0)
            die("send SET_ACTIVATION clear");
        if (keep_fd && m->target_fd >= 0) {
            close(m->target_fd);
            m->target_fd = -1;
        }
    }
}

static void mock_send_buffers(struct mock_pw *m, uint32_t port, uint32_t mem_id, uint32_t n) {
    uint8_t storage[PWW_BUILD];
    struct pww_build b;
    struct pww_frame root;
    uint32_t offs[2][2] = { { OFF_P0_B0, OFF_P0_B1 }, { OFF_P1_B0, OFF_P1_B1 } };
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, SPA_DIRECTION_OUTPUT);
    pww_pod_int(&b, (int32_t)port);
    pww_pod_int(&b, 0);
    pww_pod_int(&b, 0);
    pww_pod_int(&b, (int32_t)n);
    for (uint32_t i = 0; i < n; i++) {
        pww_pod_int(&b, (int32_t)mem_id);
        pww_pod_int(&b, (int32_t)offs[port][i]);
        pww_pod_int(&b, BUF_REGION);
        pww_pod_int(&b, 0);
        pww_pod_int(&b, 1);
        pww_pod_id(&b, SPA_DATA_MEMPTR);
        pww_pod_int(&b, BUF_DATA_OFF);
        pww_pod_int(&b, 0);
        pww_pod_int(&b, 0);
        pww_pod_int(&b, BUF_MAXSIZE);
    }
    if (pww_message_send(&m->wire, &b, &root, 2, PW_CLIENT_NODE_EVENT_PORT_USE_BUFFERS) < 0)
        die("send PORT_USE_BUFFERS");
}

static void mock_send_command(struct mock_pw *m, uint32_t cmd) {
    uint8_t storage[256];
    struct pww_build b;
    struct pww_frame root, obj;
    pww_message_begin(&b, &root, storage);
    pww_pod_object_begin(&b, &obj, SPA_TYPE_OBJECT_COMMANDNODE, cmd);
    pww_pod_object_end(&b, &obj);
    if (pww_message_send(&m->wire, &b, &root, 2, PW_CLIENT_NODE_EVENT_COMMAND) < 0)
        die("send COMMAND");
}

static void mock_attach_all(struct mock_pw *m, uint32_t mem_id, uint32_t extra_node_id) {
    struct port_io_wire *p0 = (struct port_io_wire *)(m->mem + OFF_P0_IO);
    struct port_io_wire *p1 = (struct port_io_wire *)(m->mem + OFF_P1_IO);
    p0->status = SPA_STATUS_NEED_DATA;
    p0->buffer_id = SPA_ID_INVALID;
    p1->status = SPA_STATUS_NEED_DATA;
    p1->buffer_id = SPA_ID_INVALID;
    mock_send_io(m, SPA_IO_POSITION, mem_id, OFF_POS, 256);
    mock_send_io(m, SPA_IO_CLOCK, mem_id, OFF_CLOCK, 128);
    mock_send_io(m, 99, mem_id, OFF_UNKNOWN_IO, 64);
    mock_send_port_io(m, 0, SPA_IO_BUFFERS, mem_id, OFF_P0_IO, 16);
    mock_send_port_io(m, 0, SPA_IO_RATEMATCH, mem_id, OFF_P0_RM, 32);
    mock_send_port_io(m, 0, 99, mem_id, OFF_UNKNOWN_IO, 32);
    mock_send_port_io(m, 1, SPA_IO_BUFFERS, mem_id, OFF_P1_IO, 16);
    mock_send_port_io(m, 1, SPA_IO_RATEMATCH, mem_id, OFF_P1_RM, 32);
    mock_send_port_io(m, 1, 99, mem_id, OFF_UNKNOWN_IO, 32);
    mock_send_activation(m, 42, mem_id, OFF_ACT_TARGET, 1024, 0);
    mock_send_activation(m, 42, mem_id, OFF_ACT_TARGET, 1024, 1);
    if (extra_node_id) {
        mock_send_activation(m, extra_node_id, mem_id, OFF_ACT_TARGET, 1024, 0);
        mock_send_activation(m, extra_node_id, SPA_ID_INVALID, 0, 0, 0);
    }
    mock_send_buffers(m, 0, mem_id, 2);
    mock_send_buffers(m, 1, mem_id, 2);
}

static void mock_detach_all(struct mock_pw *m) {
    mock_send_command(m, SPA_NODE_COMMAND_SUSPEND);
    mock_send_io(m, SPA_IO_POSITION, SPA_ID_INVALID, 0, 0);
    mock_send_io(m, SPA_IO_CLOCK, SPA_ID_INVALID, 0, 0);
    mock_send_port_io(m, 0, SPA_IO_BUFFERS, SPA_ID_INVALID, 0, 0);
    mock_send_port_io(m, 0, SPA_IO_RATEMATCH, SPA_ID_INVALID, 0, 0);
    mock_send_port_io(m, 1, SPA_IO_BUFFERS, SPA_ID_INVALID, 0, 0);
    mock_send_port_io(m, 1, SPA_IO_RATEMATCH, SPA_ID_INVALID, 0, 0);
    mock_send_activation(m, 42, SPA_ID_INVALID, 0, 0, 1);
    mock_send_buffers(m, 0, 1, 0);
    mock_send_buffers(m, 1, 1, 0);
}

static int mock_consume_active(struct mock_pw *m, int want) {
    for (int i = 0; i < m->n_active_events; i++) {
        if (m->active_events[i] == want) {
            memmove(m->active_events, m->active_events + i + 1,
                    (size_t)(m->n_active_events - i - 1) * sizeof(int));
            m->n_active_events -= i + 1;
            return 1;
        }
    }
    return 0;
}

static void mock_sync(struct mock_pw *m, int timeout_ms) {
    uint32_t seq = ++m->ping_seq;
    uint8_t storage[256];
    struct pww_build b;
    struct pww_frame root;
    pww_message_begin(&b, &root, storage);
    pww_pod_int(&b, 0);
    pww_pod_int(&b, (int32_t)seq);
    if (pww_message_send(&m->wire, &b, &root, 0, PW_CORE_EVENT_PING) < 0) die("send PING");
    uint64_t deadline = monotonic_ms() + (uint64_t)timeout_ms;
    while (m->pong_seq != seq) {
        if (monotonic_ms() >= deadline) die("timeout waiting for PONG");
        mock_pump(m, 10);
    }
}

static void mock_wait_active(struct mock_pw *m, int want, int timeout_ms) {
    uint64_t deadline = monotonic_ms() + (uint64_t)timeout_ms;
    while (!mock_consume_active(m, want)) {
        if (monotonic_ms() >= deadline) die(want ? "timeout waiting SET_ACTIVE(1)" : "timeout waiting SET_ACTIVE(0)");
        mock_pump(m, 10);
    }
}

static void mock_assert_no_pause(struct mock_pw *m, int duration_ms) {
    uint64_t deadline = monotonic_ms() + (uint64_t)duration_ms;
    while (monotonic_ms() < deadline) {
        mock_pump(m, 10);
        for (int i = 0; i < m->n_active_events; i++)
            if (m->active_events[i] == 0) die("client sent SET_ACTIVE(0) while not streaming");
    }
}

static void wait_stats_contains(struct tomoe *s, const char *needle, int timeout_ms) {
    uint64_t deadline = monotonic_ms() + (uint64_t)timeout_ms;
    for (;;) {
        const char *st = tomoe_sound_stats(s);
        if (strstr(st, needle)) return;
        if (monotonic_ms() >= deadline) {
            fprintf(stderr, "e2e-sound: FAIL: stats '%s' never contained '%s'\n", st, needle);
            exit(1);
        }
        struct timespec ts = { 0, 2000000 };
        nanosleep(&ts, NULL);
    }
}

static void mock_run_cycle(struct mock_pw *m, int expect_nonzero) {
    struct port_io_wire *p0 = (struct port_io_wire *)(m->mem + OFF_P0_IO);
    struct port_io_wire *p1 = (struct port_io_wire *)(m->mem + OFF_P1_IO);
    p0->status = SPA_STATUS_NEED_DATA;
    p1->status = SPA_STATUS_NEED_DATA;

    memset(m->mem + OFF_P0_B0 + BUF_DATA_OFF, 0, QUANTUM * sizeof(float));
    memset(m->mem + OFF_P0_B1 + BUF_DATA_OFF, 0, QUANTUM * sizeof(float));
    memset(m->mem + OFF_P1_B0 + BUF_DATA_OFF, 0, QUANTUM * sizeof(float));
    memset(m->mem + OFF_P1_B1 + BUF_DATA_OFF, 0, QUANTUM * sizeof(float));

    uint8_t *tact = m->mem + OFF_ACT_TARGET;
    uint32_t ver = 1;
    memcpy(tact + PW_ACT_SERVER_VERSION, &ver, 4);
    __atomic_store_n((int32_t *)(tact + PW_ACT_STATE + PW_STATE_PENDING), 1, __ATOMIC_RELEASE);
    __atomic_store_n((uint32_t *)(tact + PW_ACT_STATUS), PW_NODE_ACTIVATION_NOT_TRIGGERED, __ATOMIC_RELEASE);

    uint8_t *cact = m->mem + OFF_ACT_CLIENT;
    __atomic_store_n((uint32_t *)(cact + PW_ACT_STATUS), PW_NODE_ACTIVATION_TRIGGERED, __ATOMIC_RELEASE);

    uint64_t one = 1;
    if (write(m->process_fd, &one, sizeof(one)) != (ssize_t)sizeof(one)) die("write process_fd");

    struct pollfd p = { .fd = m->target_fd, .events = POLLIN };
    int r = poll(&p, 1, 2000);
    if (r <= 0 || !(p.revents & POLLIN)) die("timeout waiting for target activation eventfd");
    uint64_t cnt = 0;
    if (read(m->target_fd, &cnt, sizeof(cnt)) != (ssize_t)sizeof(cnt)) die("read target_fd");

    if (expect_nonzero) {
        uint32_t id0 = p0->buffer_id, id1 = p1->buffer_id;
        if (id0 >= 2 || id1 >= 2) die("invalid buffer_id after process");
        uint32_t off0 = id0 == 0 ? OFF_P0_B0 : OFF_P0_B1;
        uint32_t off1 = id1 == 0 ? OFF_P1_B0 : OFF_P1_B1;
        const float *f0 = (const float *)(m->mem + off0 + BUF_DATA_OFF);
        const float *f1 = (const float *)(m->mem + off1 + BUF_DATA_OFF);
        float peak0 = 0.0f, peak1 = 0.0f;
        for (uint32_t i = 0; i < 64; i++) {
            float a0 = fabsf(f0[i]), a1 = fabsf(f1[i]);
            if (a0 > peak0) peak0 = a0;
            if (a1 > peak1) peak1 = a1;
        }
        if (peak0 < 0.1f || peak1 < 0.1f) die("expected non-zero PCM output");
    }
}

static void park_stream(struct mock_pw *m) {
    for (int i = 0; i < 16; i++) {
        mock_run_cycle(m, 0);
        mock_pump(m, 5);
        if (mock_consume_active(m, 0)) return;
    }
    mock_wait_active(m, 0, 2000);
}


int main(void) {
    char tmpdir[] = "/tmp/tomoe-sound-test-XXXXXX";
    if (!mkdtemp(tmpdir)) die("mkdtemp");
    char sock_path[256], wav_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/pw.sock", tmpdir);
    snprintf(wav_path, sizeof(wav_path), "%s/click.wav", tmpdir);
    write_test_wav(wav_path);

    if (setenv("PIPEWIRE_REMOTE", sock_path, 1) < 0) die("setenv PIPEWIRE_REMOTE");

    int fd_baseline = count_open_fds();
    if (fd_baseline < 0) die("count_open_fds baseline");

    struct mock_pw m;
    mock_init(&m, sock_path);

    struct tomoe s;
    memset(&s, 0, sizeof(s));
    s.settings.sound_pause_after = 0.005;

    struct presentation plan;
    memset(&plan, 0, sizeof(plan));
    s.presentation = &plan;
    if (!tomoe_present_sounds(&s)) die("tomoe_present_sounds");
    const char *err = tomoe_present_sound(&s, SOUND_OPEN, wav_path, 0.0);
    if (err) die(err);
    s.presentation = NULL;
    sound_publish(&s, &plan);

    mock_accept(&m, 3000);
    mock_send_bound(&m);
    mock_send_add_mem(&m, 1);
    mock_send_transport(&m, 1);
    mock_attach_all(&m, 1, 100);
    mock_send_command(&m, SPA_NODE_COMMAND_START);
    wait_stats_contains(&s, ":state :streaming :error nil", 2000);

    int fd_after_warmup = -1;
    int vma_after_warmup = -1;
    int cycles = 320;
    for (int i = 0; i < cycles; i++) {
        park_stream(&m);
        mock_detach_all(&m);
        mock_sync(&m, 2000);
        sound_play(&s, SOUND_OPEN);
        mock_wait_active(&m, 1, 2000);
        mock_attach_all(&m, 1, (uint32_t)(200 + i));
        mock_send_command(&m, SPA_NODE_COMMAND_START);
        mock_sync(&m, 2000);
        mock_run_cycle(&m, 1);
        if (i == 1) {
            fd_after_warmup = count_open_fds();
            vma_after_warmup = count_vma_maps();
        }
    }

    int fd_after_cycles = count_open_fds();
    int vma_after_cycles = count_vma_maps();
    if (m.accepts != 1) die("unexpected reconnect during 320 pause/resume cycles");
    if (fd_after_cycles != fd_after_warmup) {
        fprintf(stderr, "e2e-sound: FAIL: fd leak across 320 cycles (%d -> %d)\n",
                fd_after_warmup, fd_after_cycles);
        return 1;
    }
    if (vma_after_cycles - vma_after_warmup > 4) {
        fprintf(stderr, "e2e-sound: FAIL: vma leak across 320 cycles (%d -> %d)\n",
                vma_after_warmup, vma_after_cycles);
        return 1;
    }
    const char *st1 = tomoe_sound_stats(&s);
    if (!strstr(st1, ":state :streaming :error nil :played 320 :dropped 0")) {
        fprintf(stderr, "e2e-sound: FAIL: unexpected stats after 320 cycles: %s\n", st1);
        return 1;
    }

    park_stream(&m);
    mock_close_client(&m);
    wait_stats_contains(&s, ":state :retrying", 2000);

    uint64_t t0 = monotonic_ms();
    sound_play(&s, SOUND_OPEN);
    mock_accept(&m, 800);
    uint64_t dt = monotonic_ms() - t0;
    if (dt >= 800) die("sound_play while retrying did not wake reconnect promptly");

    mock_send_bound(&m);
    mock_send_add_mem(&m, 1);
    mock_send_transport(&m, 1);
    mock_attach_all(&m, 1, 900);
    wait_stats_contains(&s, ":state :connecting", 2000);
    sound_play(&s, SOUND_OPEN);
    mock_assert_no_pause(&m, 120);

    mock_send_command(&m, SPA_NODE_COMMAND_START);
    mock_sync(&m, 2000);
    wait_stats_contains(&s, ":state :streaming :error nil", 2000);
    sound_play(&s, SOUND_OPEN);
    mock_run_cycle(&m, 1);

    mock_send_command(&m, SPA_NODE_COMMAND_SUSPEND);
    wait_stats_contains(&s, ":state :connecting", 2000);
    sound_play(&s, SOUND_OPEN);
    mock_assert_no_pause(&m, 120);
    mock_send_command(&m, SPA_NODE_COMMAND_START);
    mock_sync(&m, 2000);
    wait_stats_contains(&s, ":state :streaming :error nil", 2000);
    sound_play(&s, SOUND_OPEN);
    mock_run_cycle(&m, 1);

    sound_finish(&s);
    mock_destroy(&m);
    unlink(wav_path);
    unlink(sock_path);
    rmdir(tmpdir);

    int fd_final = count_open_fds();
    if (fd_final != fd_baseline) {
        fprintf(stderr, "e2e-sound: FAIL: fd leak after teardown (%d -> %d)\n",
                fd_baseline, fd_final);
        return 1;
    }

    printf("e2e-sound: PASS (cycles=%d accepts=%d fds=%d->%d->%d vmas=%d->%d wake_ms=%llu)\n",
            cycles, m.accepts, fd_baseline, fd_after_cycles, fd_final,
            vma_after_warmup, vma_after_cycles, (unsigned long long)dt);
    return 0;
}
