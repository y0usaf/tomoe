#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <poll.h>
#include <sys/pidfd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define listen tomoe_listen
#include "internal.h"

enum {
    LINK_OUT_MAX = 2 * TPL_MAX_PAYLOAD + (1 << 20),
    LINK_PING_MS = 2000,
    LINK_DEADLINE_MS = 10000,
    LINK_BACKLOG_MS = 2000,
    LINK_WATCHES = 256,
    LINK_FDS = 16,
    LINK_LOADING_MS = 120000,
    LINK_BACKOFF_MAX_MS = 30000,
    LINK_QUICK_MS = 5000,
    LINK_RESUME_MS = 2000,
    LINK_BATCH = 16,
    LINK_OPEN_MS = 3000,
    LINK_REPLAYS = 1,
    LINK_REAP_MS = 2000
};

struct snapshot_entry {
    char *name;
    uint8_t *data;
    size_t length;
};

struct policy_link {
    struct tomoe *s;
    char *config;
    bool bare, watch;
    int control_fd, json_fd;
    pid_t pid;
    int pidfd, fd;
    struct wl_event_source *fd_source, *exit_source, *retry_source, *ping_source, *restart_idle;
    uint8_t *out;
    size_t out_length, out_offset, out_capacity;
    uint8_t *in;
    size_t in_length, in_capacity;
    bool ready, batch, loading, stopping, broken, poisoned;
    char poison[512];
    uint32_t seq, batch_seq, generation;
    bool ping_pending;
    uint64_t ping_sent_ms, last_read_ms;
    uint64_t started_ms, ready_ms;
    int failures, resume_deaths;
    bool died_ready, died_external, death_pending, core_kill;
    bool resumed_snapshot;
    struct snapshot_entry *snapshots, *staged;
    size_t snapshot_count, staged_count;
    uint64_t seed;
    bool clients_open, ping_armed, backlog, client_wait, hook_active;
    struct wl_event_source *listeners[2];
    struct { uint32_t id; int fd; struct wl_event_source *source; } watches[LINK_WATCHES];
    size_t watch_count;
    int in_fds[LINK_FDS];
    size_t in_fd_count;
    uint64_t *accepted;
    size_t accepted_count, accepted_capacity;
    uint8_t *inflight;
    size_t inflight_length;
    uint64_t rng;
    bool has_rng, discard_pending;
    int replays;
    pid_t old_pid;
    uint64_t closed_ms, died_ms;
    struct wl_event_source *open_source;
};

static void link_disconnect(struct policy_link *link, const char *reason);
static void link_settle(struct policy_link *link);
static void link_watch(struct policy_link *link);
static void listeners_watch(struct policy_link *link);
static void watch_add(struct policy_link *link, uint32_t id);
static void watch_remove(struct policy_link *link, uint32_t id);

static void clients_open(struct policy_link *link) {
    if (link->clients_open) return;
    link->clients_open = true;
    wl_display_set_accepting(link->s->display, true);
    if (link->open_source) wl_event_source_timer_update(link->open_source, 0);
}

static int clients_open_late(void *data) {
    struct policy_link *link = data;
    if (!link->clients_open)
        tomoe_log(LOG_ERROR, "tomoe: tomoe-policy is not ready after %d ms; accepting clients anyway",
            LINK_OPEN_MS);
    clients_open(link);
    return 0;
}
static int link_spawn(struct policy_link *link);

static uint64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

void policy_state(struct tomoe *s, struct tpl_state *state) {
    memset(state, 0, sizeof(*state));
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    state->mono_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
    clock_gettime(CLOCK_REALTIME, &now);
    state->real_ns = (int64_t)now.tv_sec * 1000000000ll + (int64_t)now.tv_nsec;
    state->seed = s->policy_link ? s->policy_link->seed : 0;
    state->keyboard_focus = tomoe_keyboard_focus(s);
    state->grab_id = tomoe_grab_id(s);
    state->grab_mode = tomoe_grab_mode(s);
    state->outputs_revision = tomoe_outputs_revision(s);
    state->activity_serial = tomoe_activity_serial(s);
}

static void link_update_events(struct policy_link *link) {
    if (!link->fd_source) return;
    uint32_t mask = WL_EVENT_READABLE;
    if (link->out_offset < link->out_length) mask |= WL_EVENT_WRITABLE;
    wl_event_source_fd_update(link->fd_source, mask);
}

static bool link_flush(struct policy_link *link) {
    while (link->out_offset < link->out_length) {
        ssize_t n = send(link->fd, link->out + link->out_offset, link->out_length - link->out_offset,
            MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return false;
        }
        link->out_offset += (size_t)n;
    }
    if (link->out_offset == link->out_length) {
        link->out_offset = 0;
        link->out_length = 0;
        if (link->out_capacity > 32768) {
            uint8_t *shrunk = realloc(link->out, 16384);
            if (shrunk) {
                link->out = shrunk;
                link->out_capacity = 16384;
            }
        }
    } else if (link->out_offset > (1u << 20)) {
        memmove(link->out, link->out + link->out_offset, link->out_length - link->out_offset);
        link->out_length -= link->out_offset;
        link->out_offset = 0;
    }
    link_update_events(link);
    return true;
}

static void link_restart_idle(void *data) {
    struct policy_link *link = data;
    link->restart_idle = NULL;
    if (link->broken && !link->stopping) link_disconnect(link, "its link broke");
}

static void link_break(struct policy_link *link) {
    if (link->broken || link->stopping) return;
    link->broken = true;
    link->restart_idle = wl_event_loop_add_idle(wl_display_get_event_loop(link->s->display),
        link_restart_idle, link);
}

static bool link_send(struct policy_link *link, uint16_t type, uint32_t seq,
        const void *a, size_t a_length, const void *b, size_t b_length) {
    if (link->fd < 0 || link->broken) return false;
    size_t length = a_length + b_length;
    if (length > TPL_MAX_PAYLOAD || link->out_length - link->out_offset + length +
            sizeof(struct tpl_hdr) > LINK_OUT_MAX) {
        tomoe_log(LOG_ERROR, "tomoe: tomoe-policy %d stopped reading; restarting it",
            (int)link->pid);
        link_break(link);
        return false;
    }
    size_t need = link->out_length + sizeof(struct tpl_hdr) + length;
    if (need > link->out_capacity) {
        size_t capacity = link->out_capacity ? link->out_capacity : 16384;
        while (capacity < need) capacity *= 2;
        uint8_t *grown = realloc(link->out, capacity);
        if (!grown) {
            link_break(link);
            return false;
        }
        link->out = grown;
        link->out_capacity = capacity;
    }
    struct tpl_hdr hdr = { TPL_MAGIC, TPL_VERSION, type, (uint32_t)length, seq };
    memcpy(link->out + link->out_length, &hdr, sizeof(hdr));
    link->out_length += sizeof(hdr);
    if (a_length) memcpy(link->out + link->out_length, a, a_length);
    link->out_length += a_length;
    if (b_length) memcpy(link->out + link->out_length, b, b_length);
    link->out_length += b_length;
    if (!link_flush(link)) {
        link_break(link);
        return false;
    }
    return true;
}

static void reply(struct policy_link *link, uint32_t seq, bool ok, pv value) {
    struct tpl_reply head = { .status = ok ? TPL_OK : TPL_ERROR };
    policy_state(link->s, &head.state);
    struct pv_buf body = { 0 };
    pv_encode(&body, value);
    if (body.failed) {
        pv_buf_free(&body);
        head.status = TPL_ERROR;
        pv value_error = pv_cstring("Native reply is too large.");
        pv_encode(&body, value_error);
    }
    link_send(link, TPL_REPLY, seq, &head, sizeof(head), body.data, body.length);
    pv_buf_free(&body);
}

static bool effect_seen(struct policy_link *link, uint64_t effect) {
    for (size_t i = 0; i < link->accepted_count; i++)
        if (link->accepted[i] == effect) return true;
    if (link->accepted_count == link->accepted_capacity) {
        size_t capacity = link->accepted_capacity ? link->accepted_capacity * 2 : 32;
        uint64_t *grown = realloc(link->accepted, capacity * sizeof(*grown));
        if (!grown) return false;
        link->accepted = grown;
        link->accepted_capacity = capacity;
    }
    link->accepted[link->accepted_count++] = effect;
    return false;
}

static void inflight_drop(struct policy_link *link) {
    link->replays = 0;
    free(link->inflight);
    link->inflight = NULL;
    link->inflight_length = 0;
    if (!tomoe_test_flag("policy-sticky-dedup")) link->accepted_count = 0;
}

static void handle_call(struct policy_link *link, uint32_t seq, const uint8_t *data, size_t length) {
    struct tpl_call call;
    if (length < sizeof(call)) {
        link_break(link);
        return;
    }
    memcpy(&call, data, sizeof(call));
    bool sync = call.flags & TPL_CALL_SYNC;
    if (link->poisoned) {
        if (!sync) return;
        link->poisoned = false;
        reply(link, seq, false, pv_cstring(link->poison));
        return;
    }
    jmp_buf target;
    jmp_buf *saved = pv_error_target;
    pv_error_target = &target;
    if (setjmp(target)) {
        pv_error_target = saved;
        if (sync) {
            reply(link, seq, false, pv_cstring(pv_error_message()));
        } else {
            link->poisoned = true;
            snprintf(link->poison, sizeof(link->poison), "%s", pv_error_message());
        }
        return;
    }
    size_t offset = sizeof(call);
    pv args[16];
    if (call.argc > 16) pv_error("Too many arguments.");
    for (uint32_t i = 0; i < call.argc; i++) {
        if (pv_decode(data, length, &offset, &args[i])) {
            pv_error_target = saved;
            link_break(link);
            return;
        }
    }
    if (call.effect && !tomoe_test_flag("policy-no-dedup") && effect_seen(link, call.effect)) {
        pv_error_target = saved;
        if (sync) reply(link, seq, true, pv_nil());
        return;
    }
    pv result = policy_call_primitive(call.prim, (int)call.argc, args);
    pv_error_target = saved;
    if (sync) reply(link, seq, true, result);
}

static void snapshot_apply(struct policy_link *link, const uint8_t *data, size_t length);

static void staged_clear(struct policy_link *link) {
    for (size_t i = 0; i < link->staged_count; i++) free(link->staged[i].data);
    free(link->staged);
    link->staged = NULL;
    link->staged_count = 0;
}

static void staged_publish(struct policy_link *link) {
    for (size_t i = 0; i < link->staged_count; i++)
        snapshot_apply(link, link->staged[i].data, link->staged[i].length);
    staged_clear(link);
}

static void handle_snapshot(struct policy_link *link, const uint8_t *data, size_t length) {
    if (tomoe_test_flag("policy-snapshot-unstaged")) {
        snapshot_apply(link, data, length);
        return;
    }
    struct snapshot_entry *grown = realloc(link->staged, (link->staged_count + 1) * sizeof(*grown));
    uint8_t *copy = malloc(length ? length : 1);
    if (!grown || !copy) {
        if (grown) link->staged = grown;
        free(copy);
        return;
    }
    link->staged = grown;
    memcpy(copy, data, length);
    link->staged[link->staged_count++] = (struct snapshot_entry){ NULL, copy, length };
}

static struct snapshot_entry *snapshot_find(struct policy_link *link, const char *name) {
    for (size_t i = 0; i < link->snapshot_count; i++)
        if (!strcmp(link->snapshots[i].name, name)) return &link->snapshots[i];
    return NULL;
}

static void snapshots_clear(struct policy_link *link);
static void snapshot_apply(struct policy_link *link, const uint8_t *data, size_t length) {
    uint32_t name_length;
    if (length < 4) return;
    memcpy(&name_length, data, 4);
    if (name_length == 0) {
        snapshots_clear(link);
        return;
    }
    if (name_length > 255 || length - 4 < name_length) return;
    char name[256];
    memcpy(name, data + 4, name_length);
    name[name_length] = 0;
    const uint8_t *blob = data + 4 + name_length;
    size_t blob_length = length - 4 - name_length;
    struct snapshot_entry *entry = snapshot_find(link, name);
    if (!blob_length) {
        if (!entry) return;
        free(entry->name);
        free(entry->data);
        *entry = link->snapshots[--link->snapshot_count];
        return;
    }
    uint8_t *copy = malloc(blob_length);
    if (!copy) return;
    memcpy(copy, blob, blob_length);
    if (!entry) {
        struct snapshot_entry *grown = realloc(link->snapshots,
            (link->snapshot_count + 1) * sizeof(*grown));
        char *dup = strdup(name);
        if (!grown || !dup) {
            if (grown) link->snapshots = grown;
            free(dup);
            free(copy);
            return;
        }
        link->snapshots = grown;
        entry = &link->snapshots[link->snapshot_count++];
        entry->name = dup;
        entry->data = NULL;
    }
    free(entry->data);
    entry->data = copy;
    entry->length = blob_length;
}

static void snapshots_clear(struct policy_link *link) {
    for (size_t i = 0; i < link->snapshot_count; i++) {
        free(link->snapshots[i].name);
        free(link->snapshots[i].data);
    }
    free(link->snapshots);
    link->snapshots = NULL;
    link->snapshot_count = 0;
}

static void handle_message(struct policy_link *link, const struct tpl_hdr *hdr,
        const uint8_t *data) {
    struct tomoe *s = link->s;
    switch (hdr->type) {
    case TPL_READY: {
        uint32_t version = 0;
        if (hdr->len >= 4) memcpy(&version, data, 4);
        if (version != TPL_VERSION) {
            tomoe_log(LOG_ERROR, "tomoe: tomoe-policy speaks protocol %u, not %u; rejecting it",
                version, TPL_VERSION);
            link_break(link);
            return;
        }
        link->ready = true;
        link->ready_ms = now_ms();
        clients_open(link);
        listeners_watch(link);
        ui_assets_shader_resend(s);
        if (link->inflight && link->replays >= LINK_REPLAYS) {
            tomoe_log(LOG_ERROR, "tomoe: an event batch stopped tomoe-policy %d times; dropping it",
                link->replays);
            inflight_drop(link);
        }
        if (link->inflight) {
            link->replays++;
            link->batch_seq = ++link->seq ? link->seq : ++link->seq;
            if (link_send(link, TPL_EVENTS, link->batch_seq, link->inflight, link->inflight_length,
                    NULL, 0)) {
                link->batch = true;
                link_watch(link);
            }
        }
        link->loading = false;
        tomoe_log(LOG_WARNING, "tomoe: tomoe-policy %d ready (generation %u)", (int)link->pid,
            link->generation);
        schedule_scene(s);
        return;
    }
    case TPL_CALL:
        handle_call(link, hdr->seq, data, hdr->len);
        pv_arena_reset();
        return;
    case TPL_DONE:
        if (link->poisoned) {
            tomoe_log(LOG_ERROR, "tomoe: dispatch failed: %s", link->poison);
            link->poisoned = false;
        }
        staged_publish(link);
        link->hook_active = false;
        if (hdr->len >= sizeof(struct tpl_done)) {
            struct tpl_done done;
            memcpy(&done, data, sizeof(done));
            link->rng = done.rng;
            link->has_rng = true;
        }
        if (hdr->seq && hdr->seq == link->batch_seq) {
            link->batch = false;
            inflight_drop(link);
        }
        link_settle(link);
        return;
    case TPL_SNAPSHOT:
        if (!tomoe_test_flag("policy-snapshot-hold")) handle_snapshot(link, data, hdr->len);
        return;
    case TPL_LOADING: {
        uint32_t on = 0;
        if (hdr->len >= 4) memcpy(&on, data, 4);
        link->loading = on != 0;
        return;
    }
    case TPL_WATCH: {
        uint32_t id = 0;
        if (hdr->len >= 4) memcpy(&id, data, 4);
        watch_add(link, id);
        link_settle(link);
        return;
    }
    case TPL_UNWATCH: {
        uint32_t id = 0;
        if (hdr->len >= 4) memcpy(&id, data, 4);
        watch_remove(link, id);
        return;
    }
    case TPL_HOOK_BEGIN:
        link->hook_active = true;
        link_watch(link);
        return;
    case TPL_PONG:
        link->ping_pending = false;
        link_settle(link);
        return;
    case TPL_SHADER_SPIRV: {
        struct tpl_shader_spirv head;
        if (hdr->len < sizeof(head)) return;
        memcpy(&head, data, sizeof(head));
        size_t length = hdr->len - sizeof(head);
        if ((uint64_t)head.words * 4 != length || length > TPL_SHADER_SPIRV_MAX) return;
        ui_asset_shader_built(s, head.build, length ? data + sizeof(head) : NULL, length,
            head.flags);
        return;
    }
    default:
        return;
    }
}

static bool link_read(struct policy_link *link) {
    for (;;) {
        if (link->in_capacity - link->in_length < 4096) {
            size_t capacity = link->in_capacity ? link->in_capacity * 2 : 16384;
            if (capacity > TPL_MAX_PAYLOAD + 65536 + sizeof(struct tpl_hdr))
                capacity = TPL_MAX_PAYLOAD + 65536 + sizeof(struct tpl_hdr);
            if (capacity > link->in_capacity) {
                uint8_t *grown = realloc(link->in, capacity);
                if (!grown) {
                    link->core_kill = true;
                    return false;
                }
                link->in = grown;
                link->in_capacity = capacity;
            }
            if (link->in_capacity == link->in_length) {
                link->core_kill = true;
                return false;
            }
        }
        char control[CMSG_SPACE(sizeof(int) * 8)];
        struct iovec iov = { link->in + link->in_length, link->in_capacity - link->in_length };
        struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
            .msg_controllen = sizeof(control) };
        ssize_t n = recvmsg(link->fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0 && errno == EINTR) continue;
        if (n > 0) {
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
                if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
                size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (size_t i = 0; i < count; i++) {
                    int fd;
                    memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                    if (link->in_fd_count < LINK_FDS) link->in_fds[link->in_fd_count++] = fd;
                    else close(fd);
                }
            }
        }
        if (n == 0) return false;
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK;
        link->in_length += (size_t)n;
        link->last_read_ms = now_ms();
        size_t offset = 0;
        while (link->in_length - offset >= sizeof(struct tpl_hdr)) {
            struct tpl_hdr hdr;
            memcpy(&hdr, link->in + offset, sizeof(hdr));
            if (hdr.magic != TPL_MAGIC || hdr.version != TPL_VERSION ||
                    hdr.len > TPL_MAX_PAYLOAD) {
                tomoe_log(LOG_ERROR, "tomoe: tomoe-policy sent a malformed message");
                link->core_kill = true;
                return false;
            }
            size_t frame = sizeof(hdr) + hdr.len;
            if (link->in_length - offset < frame) break;
            handle_message(link, &hdr, link->in + offset + sizeof(hdr));
            if (link->fd < 0 || link->broken) return true;
            offset += frame;
        }
        if (offset) {
            link->in_length -= offset;
            if (link->in_length) {
                memmove(link->in, link->in + offset, link->in_length);
            } else if (link->in_capacity > 32768) {
                uint8_t *shrunk = realloc(link->in, 16384);
                if (shrunk) {
                    link->in = shrunk;
                    link->in_capacity = 16384;
                }
            }
        }
    }
}

static int link_fd_ready(int fd, uint32_t mask, void *data) {
    struct policy_link *link = data;
    if (link->broken) return 0;
    if (mask & WL_EVENT_READABLE) {
        if (!link_read(link)) {
            link_disconnect(link, "it closed its link");
            return 0;
        }
        if (link->fd < 0) return 0;
    }
    if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
        link_disconnect(link, "its link hung up");
        return 0;
    }
    if ((mask & WL_EVENT_WRITABLE) && !link_flush(link)) link_disconnect(link, "its link failed");
    return 0;
}

static int link_exited(int fd, uint32_t mask, void *data) {
    struct policy_link *link = data;
    link_disconnect(link, "it exited");
    return 0;
}

static bool listener_pending(struct policy_link *link) {
    struct pollfd fds[2];
    nfds_t count = 0;
    if (link->control_fd >= 0) fds[count++] = (struct pollfd){ link->control_fd, POLLIN, 0 };
    if (link->json_fd >= 0) fds[count++] = (struct pollfd){ link->json_fd, POLLIN, 0 };
    return count && poll(fds, count, 0) > 0;
}

static uint64_t link_limit(struct policy_link *link) {
    if (link->loading || !link->ready) return LINK_LOADING_MS;
    const char *deadline = tomoe_test_flag_value("policy-deadline-ms");
    if (deadline) return strtoull(deadline, NULL, 10);
    if (link->backlog && !link->batch && !link->hook_active && !link->client_wait &&
            link->out_offset == link->out_length && listener_pending(link))
        return LINK_BACKLOG_MS;
    return LINK_DEADLINE_MS;
}

static bool link_busy(struct policy_link *link) {
    return link->fd >= 0 && (!link->ready || link->loading || link->batch || link->ping_pending ||
        link->backlog || link->client_wait || link->hook_active || link->out_offset < link->out_length);
}

static void listeners_set(struct policy_link *link, uint32_t mask) {
    for (int i = 0; i < 2; i++)
        if (link->listeners[i]) wl_event_source_fd_update(link->listeners[i], mask);
    for (size_t i = 0; i < link->watch_count; i++)
        wl_event_source_fd_update(link->watches[i].source, mask);
}

static void listeners_watch(struct policy_link *link) {
    link->backlog = false;
    link->client_wait = false;
    listeners_set(link, WL_EVENT_READABLE);
}

static void link_settle(struct policy_link *link) {
    if (link->backlog || link->client_wait) listeners_watch(link);
    if (link->ping_armed && link->ping_source && !link_busy(link)) {
        wl_event_source_timer_update(link->ping_source, 0);
        link->ping_armed = false;
    }
}

static int listener_ready(int fd, uint32_t mask, void *data) {
    struct policy_link *link = data;
    listeners_set(link, 0);
    link->backlog = true;
    link_watch(link);
    return 0;
}

static int client_ready(int fd, uint32_t mask, void *data) {
    struct policy_link *link = data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        for (size_t i = 0; i < link->watch_count; i++) {
            if (link->watches[i].fd != fd) continue;
            wl_event_source_remove(link->watches[i].source);
            link->watches[i] = link->watches[--link->watch_count];
            break;
        }
    } else {
        listeners_set(link, 0);
    }
    link->client_wait = true;
    link_watch(link);
    return 0;
}

static void watch_add(struct policy_link *link, uint32_t id) {
    if (!link->in_fd_count) return;
    int fd = link->in_fds[0];
    memmove(link->in_fds, link->in_fds + 1, --link->in_fd_count * sizeof(int));
    if (link->watch_count == LINK_WATCHES) {
        close(fd);
        return;
    }
    struct wl_event_source *source = wl_event_loop_add_fd(wl_display_get_event_loop(link->s->display),
        fd, link->backlog || link->client_wait ? 0 : WL_EVENT_READABLE, client_ready, link);
    close(fd);
    if (!source) return;
    link->watches[link->watch_count].id = id;
    link->watches[link->watch_count].fd = wl_event_source_get_fd(source);
    link->watches[link->watch_count].source = source;
    link->watch_count++;
}

static void watch_remove(struct policy_link *link, uint32_t id) {
    for (size_t i = 0; i < link->watch_count; i++) {
        if (link->watches[i].id != id) continue;
        wl_event_source_remove(link->watches[i].source);
        link->watches[i] = link->watches[--link->watch_count];
        return;
    }
}

static void watches_clear(struct policy_link *link) {
    for (size_t i = 0; i < link->watch_count; i++) wl_event_source_remove(link->watches[i].source);
    link->watch_count = 0;
    for (size_t i = 0; i < link->in_fd_count; i++) close(link->in_fds[i]);
    link->in_fd_count = 0;
}

static void link_watch(struct policy_link *link) {
    if (!link->ping_source || link->ping_armed || !link_busy(link)) return;
    uint64_t every = link_limit(link) / 4;
    if (every > LINK_PING_MS) every = LINK_PING_MS;
    link->ping_armed = true;
    wl_event_source_timer_update(link->ping_source, (int)(every ? every : 1));
}

static int link_ping(void *data) {
    struct policy_link *link = data;
    link->ping_armed = false;
    if (link->stopping || link->fd < 0) return 0;
    uint64_t now = now_ms();
    uint64_t limit = link_limit(link);
    bool stuck_out = link->out_offset < link->out_length && now - link->last_read_ms > limit;
    if ((link->ping_pending && now - link->ping_sent_ms > limit) || stuck_out) {
        tomoe_log(LOG_ERROR, "tomoe: tomoe-policy %d did not answer for %llu ms; restarting it",
            (int)link->pid, (unsigned long long)limit);
        link->core_kill = true;
        link_disconnect(link, "it stopped answering");
        return 0;
    }
    if (!link->ping_pending && link_busy(link)) {
        link->ping_pending = true;
        link->ping_sent_ms = now;
        link_send(link, TPL_PING, ++link->seq, NULL, 0, NULL, 0);
    }
    link_watch(link);
    return 0;
}

static void floor_enter(struct tomoe *s) {
    free(s->grab_owner);
    free(s->grab_otherwise);
    s->grab_owner = NULL;
    s->grab_otherwise = NULL;
    s->grab_source = 0;
    s->grab_held = false;
    if (s->presentation) tomoe_present_abort(s);
    grab_clear(s);
    update_keyboard_focus(s);
    schedule_scene(s);
}

static void link_account_death(struct policy_link *link) {
    bool early = !link->died_ready || link->died_ms - link->ready_ms < LINK_RESUME_MS;
    if (link->resumed_snapshot && early && !link->died_external) link->resume_deaths++;
    else link->resume_deaths = 0;
    if (link->resume_deaths >= 2) {
        tomoe_log(LOG_ERROR, "tomoe: tomoe-policy died twice after resuming its saved state; "
            "the next one starts fresh");
        snapshots_clear(link);
        link->resume_deaths = 0;
    }
}

static int link_retry(void *data) {
    struct policy_link *link = data;
    if (link->stopping || link->pid > 0) return 0;
    if (link->old_pid && reap_pending(link->old_pid) && now_ms() - link->closed_ms < LINK_REAP_MS) {
        wl_event_source_timer_update(link->retry_source, 50);
        return 0;
    }
    if (link->death_pending) {
        int status = 0;
        link->died_external = reap_status(link->old_pid, &status) &&
            WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
        link->death_pending = false;
        link_account_death(link);
    }
    link->old_pid = 0;
    link_spawn(link);
    return 0;
}

static void link_schedule_retry(struct policy_link *link) {
    uint64_t lived = now_ms() - link->started_ms;
    link->failures = lived < LINK_QUICK_MS ? link->failures + 1 : 0;
    if (!link->death_pending) link_account_death(link);
    uint64_t delay = link->failures == 0 ? 50 : 100ull << (link->failures > 9 ? 9 : link->failures - 1);
    if (delay > LINK_BACKOFF_MAX_MS) delay = LINK_BACKOFF_MAX_MS;
    const char *forced = tomoe_test_flag_value("policy-restart-delay");
    if (forced) delay = strtoull(forced, NULL, 10);
    if (!link->retry_source)
        link->retry_source = wl_event_loop_add_timer(wl_display_get_event_loop(link->s->display),
            link_retry, link);
    if (link->retry_source) wl_event_source_timer_update(link->retry_source, (int)delay);
}

static void link_close(struct policy_link *link) {
    if (link->restart_idle) {
        wl_event_source_remove(link->restart_idle);
        link->restart_idle = NULL;
    }
    if (link->fd_source) {
        wl_event_source_remove(link->fd_source);
        link->fd_source = NULL;
    }
    if (link->exit_source) {
        wl_event_source_remove(link->exit_source);
        link->exit_source = NULL;
    }
    watches_clear(link);
    staged_clear(link);
    if (link->fd >= 0) close(link->fd);
    link->fd = -1;
    link->died_ready = link->ready;
    link->died_ms = now_ms();
    link->died_external = link->death_pending = false;
    if (link->pid > 0) {
        int status = 0;
        if (waitpid(link->pid, &status, WNOHANG) == link->pid) {
            link->died_external = !link->core_kill && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
        } else {
            link->old_pid = link->pid;
            link->closed_ms = now_ms();
            link->death_pending = !link->core_kill && !link->broken && !link->stopping;
            reap_later(link->s, link->pid, link->pidfd);
            link->pidfd = -1;
        }
        link->pid = 0;
    }
    link->core_kill = false;
    if (link->pidfd >= 0) close(link->pidfd);
    link->pidfd = -1;
    link->out_length = link->out_offset = 0;
    link->in_length = 0;
    link->ready = link->batch = link->loading = link->broken = link->poisoned = false;
    link->hook_active = link->client_wait = link->backlog = false;
    link->ping_pending = false;
}

static void link_disconnect(struct policy_link *link, const char *reason) {
    struct tomoe *s = link->s;
    if (link->fd < 0 && link->pid <= 0) return;
    if (!link->stopping)
        tomoe_log(LOG_ERROR, "tomoe: tomoe-policy %d stopped because %s; the desktop keeps its "
            "last scene and restarts it", (int)link->pid, reason);
    link_close(link);
    s->policy_accepting = false;
    if (tomoe_test_flag("policy-no-replay")) {
        inflight_drop(link);
        policy_ring_consume(s, s->event_count);
    }
    if (!link->stopping) {
        floor_enter(s);
        link_schedule_retry(link);
    }
}

void policy_link_overflow(struct tomoe *s) {
    struct policy_link *link = s ? s->policy_link : NULL;
    if (!link || link->fd < 0 || link->broken) return;
    tomoe_log(LOG_ERROR, "tomoe: the event ring filled while tomoe-policy %d was busy; "
        "restarting it with a full resync and dropping queued status events", (int)link->pid);
    link->discard_pending = true;
    link_break(link);
}

struct capture {
    struct pv_buf buf;
    uint32_t count;
    size_t base;
};

static void capture_emit(struct tomoe *s, void *data) {
    struct capture *capture = data;
    if (s->event_count) {
        const struct tomoe_event *event = policy_ring_at(s, s->event_count - 1);
        pv aux;
        bool delivers = event->type == EV_OUTPUTS || event->type == EV_LAYER ||
            event->type == EV_MAP || event->type == EV_GEOMETRY;
        pv value = delivers ? policy_event_value(s, event, &aux) : pv_nil();
        if (!pv_nilp(value)) {
            uint64_t serial = 0;
            pv_buf_u8(&capture->buf, TPL_ITEM_DISPATCH);
            pv_buf_put(&capture->buf, &serial, sizeof(serial));
            pv_encode(&capture->buf, value);
            pv_encode(&capture->buf, aux);
            capture->count++;
        }
    }
    if (s->event_count > capture->base) policy_ring_pop(s);
    pv_arena_reset();
}

static bool link_resume(struct policy_link *link) {
    struct tomoe *s = link->s;
    struct capture capture = { 0 };
    uint64_t saved_mask = s->event_read_mask;
    if (tomoe_test_flag("policy-drop-pending")) policy_ring_consume(s, s->event_count);
    if (link->discard_pending) {
        policy_ring_drop_coalescible(s);
        link->discard_pending = false;
    }
    policy_ring_keep_pending(s);
    struct tomoe_event scratch[2];
    struct policy_ring_saved pending;
    uint64_t serial = s->event_serial;
    policy_ring_stash(s, &pending, scratch, 2);
    capture.base = 0;
    s->policy_capturing = true;
    s->event_read_mask = ~0ull;
    jmp_buf target;
    jmp_buf *saved = pv_error_target;
    pv_error_target = &target;
    bool ok = true;
    if (setjmp(target)) {
        ok = false;
    } else {
        outputs_fact(s);
        capture_emit(s, &capture);
        layer_facts(s, capture_emit, &capture);
        window_facts(s, capture_emit, &capture);
    }
    pv_error_target = saved;
    pv_arena_reset();
    s->policy_capturing = false;
    s->event_read_mask = saved_mask;
    policy_ring_unstash(s, &pending);
    s->event_serial = serial;
    s->policy_accepting = true;
    if (!ok || capture.buf.failed) {
        pv_buf_free(&capture.buf);
        return false;
    }
    struct tpl_resume head = { .cutoff = s->event_serial, .facts = capture.count,
        .rng = link->rng, .has_rng = link->has_rng && !tomoe_test_flag("policy-rng-reset") };
    policy_state(s, &head.state);
    struct pv_buf snapshots = { 0 };
    link->resumed_snapshot = link->snapshot_count > 0;
    for (size_t i = 0; i < link->snapshot_count; i++)
        pv_buf_put(&snapshots, link->snapshots[i].data, link->snapshots[i].length);
    head.snapshots = (uint32_t)link->snapshot_count;
    pv_buf_put(&capture.buf, snapshots.data, snapshots.length);
    bool sent = !snapshots.failed && !capture.buf.failed &&
        link_send(link, TPL_RESUME, ++link->seq, &head, sizeof(head),
            capture.buf.data, capture.buf.length);
    pv_buf_free(&snapshots);
    pv_buf_free(&capture.buf);
    return sent;
}

void policy_binary_path(char *out, size_t size) {
    const char *env = getenv("TOMOE_POLICY");
    if (env && *env && access(env, X_OK) == 0) {
        snprintf(out, size, "%s", env);
        return;
    }
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            int length = snprintf(out, size, "%s/../libexec/tomoe-policy", exe);
            if (length > 0 && (size_t)length < size && access(out, X_OK) == 0) return;
            length = snprintf(out, size, "%s/tomoe-policy", exe);
            if (length > 0 && (size_t)length < size && access(out, X_OK) == 0) return;
        }
    }
    snprintf(out, size, "tomoe-policy");
}

static int link_spawn(struct policy_link *link) {
    struct tomoe *s = link->s;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sv) < 0) {
        link_schedule_retry(link);
        return -1;
    }
    char binary[PATH_MAX];
    policy_binary_path(binary, sizeof(binary));
    char control_arg[16], json_arg[16];
    snprintf(control_arg, sizeof(control_arg), "%d", link->control_fd >= 0 ? 4 : -1);
    snprintf(json_arg, sizeof(json_arg), "%d", link->json_fd >= 0 ? 5 : -1);
    link->started_ms = now_ms();
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        int fds[3] = { sv[1], link->control_fd, link->json_fd };
        for (int i = 0; i < 3; i++)
            if (fds[i] >= 0) fds[i] = fcntl(fds[i], F_DUPFD, 64);
        for (int i = 0; i < 3; i++)
            if (fds[i] >= 0) dup2(fds[i], 3 + i);
        syscall(SYS_close_range, 6, ~0u, 0);
        char *argv[] = { binary, "--fd", "3", "--control-fd", control_arg, "--json-fd", json_arg,
            NULL };
        execv(binary, argv);
        execvp("tomoe-policy", argv);
        _exit(127);
    }
    close(sv[1]);
    if (pid < 0) {
        close(sv[0]);
        link_schedule_retry(link);
        return -1;
    }
    link->fd = sv[0];
    link->pid = pid;
    link->generation++;
    link->last_read_ms = now_ms();
    struct wl_event_loop *loop = wl_display_get_event_loop(s->display);
    link->fd_source = wl_event_loop_add_fd(loop, link->fd, WL_EVENT_READABLE, link_fd_ready, link);
    link->pidfd = (int)pidfd_open(pid, 0);
    if (link->pidfd >= 0)
        link->exit_source = wl_event_loop_add_fd(loop, link->pidfd, WL_EVENT_READABLE,
            link_exited, link);
    if (!link->fd_source) {
        link->core_kill = true;
        link_disconnect(link, "its link could not be watched");
        return -1;
    }
    struct tpl_hello hello = { .version = TPL_VERSION, .generation = link->generation };
    if (link->bare) hello.flags |= TPL_HELLO_BARE;
    if (link->watch) hello.flags |= TPL_HELLO_WATCH;
    if (link->config) hello.flags |= TPL_HELLO_CONFIG;
    if (link->generation == 1 && tomoe_test_flag("policy-crash-before-flush"))
        hello.flags |= TPL_HELLO_TEST_CRASH_BEFORE_FLUSH;
    if (link->generation == 1 && tomoe_test_flag("policy-crash-before-done"))
        hello.flags |= TPL_HELLO_TEST_CRASH_BEFORE_DONE;
    if (tomoe_test_flag("policy-sticky-dedup")) hello.flags |= TPL_HELLO_TEST_LOCAL_IDS;
    if (link->generation == 1 && tomoe_test_flag("policy-crash-after-snapshot"))
        hello.flags |= TPL_HELLO_TEST_CRASH_AFTER_SNAPSHOT;
    if (link->generation == 1 && tomoe_test_flag("policy-crash-after-done"))
        hello.flags |= TPL_HELLO_TEST_CRASH_AFTER_DONE;
    if (tomoe_test_flag("policy-snapshot-late")) hello.flags |= TPL_HELLO_TEST_SNAPSHOT_LATE;
    const char *delay = tomoe_test_flag_value("policy-boot-delay");
    if (delay && link->generation > 1) hello.delay_ms = (uint32_t)strtoul(delay, NULL, 10);
    size_t config_length = link->config ? strlen(link->config) : 0;
    if (!link_send(link, TPL_HELLO, ++link->seq, &hello, sizeof(hello), link->config,
            config_length) || !link_resume(link)) {
        link->core_kill = true;
        link_disconnect(link, "it could not be handed the desktop's state");
        return -1;
    }
    link_watch(link);
    return 0;
}

struct policy_link *policy_link_start(struct tomoe *s, const struct policy_link_options *options) {
    struct policy_link *link = calloc(1, sizeof(*link));
    if (!link) return NULL;
    link->s = s;
    link->fd = -1;
    link->pidfd = -1;
    link->config = options->config ? strdup(options->config) : NULL;
    link->bare = options->bare;
    link->watch = options->watch;
    link->control_fd = options->control_fd;
    link->json_fd = options->json_fd;
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    link->seed = ((uint64_t)wall.tv_sec * 1000000000ull + (uint64_t)wall.tv_nsec) ^
        ((uint64_t)getpid() << 32) ^ 0x9e3779b97f4a7c15ull;
    const char *seed = tomoe_test_flag_value("policy-seed");
    if (seed) link->seed = strtoull(seed, NULL, 10);
    if (!link->seed) link->seed = 0x9e3779b97f4a7c15ull;
    s->policy_link = link;
    wl_display_set_accepting(s->display, false);
    int listen_fds[2] = { options->control_fd, options->json_fd };
    for (int i = 0; i < 2; i++)
        if (listen_fds[i] >= 0)
            link->listeners[i] = wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
                listen_fds[i], 0, listener_ready, link);
    link->open_source = wl_event_loop_add_timer(wl_display_get_event_loop(s->display),
        clients_open_late, link);
    if (link->open_source) wl_event_source_timer_update(link->open_source, LINK_OPEN_MS);
    else clients_open(link);
    link->ping_source = wl_event_loop_add_timer(wl_display_get_event_loop(s->display),
        link_ping, link);
    link_spawn(link);
    return link;
}

void policy_link_stop(struct policy_link *link) {
    if (!link) return;
    link->stopping = true;
    if (link->pid > 0) {
        kill(link->pid, SIGTERM);
        for (int i = 0; i < 100; i++) {
            if (waitpid(link->pid, NULL, WNOHANG) == link->pid) {
                link->pid = 0;
                break;
            }
            struct timespec pause = { 0, 2000000 };
            nanosleep(&pause, NULL);
        }
    }
    link_close(link);
    if (link->retry_source) wl_event_source_remove(link->retry_source);
    if (link->ping_source) wl_event_source_remove(link->ping_source);
    if (link->open_source) wl_event_source_remove(link->open_source);
    for (int i = 0; i < 2; i++)
        if (link->listeners[i]) wl_event_source_remove(link->listeners[i]);
    link->s->policy_link = NULL;
    link->s->policy_accepting = false;
    snapshots_clear(link);
    free(link->inflight);
    free(link->accepted);
    free(link->out);
    free(link->in);
    free(link->config);
    free(link);
}

bool policy_link_shader_build(struct tomoe *s, uint64_t build, const char *name,
        const char *source, size_t length) {
    if (!s || !s->policy_link || !name || !source) return false;
    size_t name_length = strlen(name);
    if (name_length > 4096 || length > TPL_SHADER_SOURCE_MAX) return false;
    struct tpl_shader_build head = { build, (uint32_t)name_length, (uint32_t)length };
    char *body = malloc(name_length + length + 1);
    if (!body) return false;
    memcpy(body, name, name_length);
    memcpy(body + name_length, source, length);
    bool sent = link_send(s->policy_link, TPL_SHADER_BUILD, 0, &head, sizeof(head), body,
        name_length + length);
    free(body);
    return sent;
}

bool policy_link_up(struct tomoe *s) {
    return s && s->policy_link && s->policy_link->ready && !s->policy_link->broken;
}

bool policy_events_pending(struct tomoe *s) {
    if (!s || !s->event_count || s->grab_held) return false;
    struct policy_link *link = s->policy_link;
    return link && link->ready && !link->batch && !link->broken;
}

struct batch {
    struct pv_buf body, replay;
    uint32_t replay_count;
    struct tpl_events head;
    size_t consumed;
    uint32_t dispatches;
    uint16_t last;
    struct { size_t body, replay; uint32_t count, dispatches, replay_count; } mark;
};

static void batch_mark(struct batch *batch) {
    batch->mark.body = batch->body.length;
    batch->mark.replay = batch->replay.length;
    batch->mark.count = batch->head.count;
    batch->mark.dispatches = batch->dispatches;
    batch->mark.replay_count = batch->replay_count;
}

static void batch_rewind(struct batch *batch) {
    batch->body.length = batch->mark.body;
    batch->replay.length = batch->mark.replay;
    batch->body.failed = batch->replay.failed = false;
    batch->head.count = batch->mark.count;
    batch->dispatches = batch->mark.dispatches;
    batch->replay_count = batch->mark.replay_count;
}

static void batch_item(struct pv_buf *body, uint8_t kind, uint64_t serial, pv first, pv second) {
    pv_buf_u8(body, kind);
    if (kind == TPL_ITEM_DISPATCH) pv_buf_put(body, &serial, sizeof(serial));
    pv_encode(body, first);
    pv_encode(body, second);
}

static bool batch_encode(struct tomoe *s, struct batch *batch) {
    jmp_buf target;
    jmp_buf *saved = pv_error_target;
    pv_error_target = &target;
    if (setjmp(target)) {
        pv_error_target = saved;
        batch_rewind(batch);
        return false;
    }
    uint64_t base = s->event_serial - s->event_count + 1;
    for (; batch->consumed < s->event_count; batch->consumed++) {
        batch_mark(batch);
        const struct tomoe_event *event = policy_ring_at(s, batch->consumed);
        uint64_t serial = base + batch->consumed;
        if (event->type == EV_INPUT || event->type == EV_ACTIVITY) {
            const char *name = NULL;
            pv value = policy_broadcast_value(s, event, &name);
            if (name) {
                batch_item(&batch->body, TPL_ITEM_BROADCAST, 0, pv_cstring(name), value);
                batch->head.count++;
            }
        }
        pv aux;
        pv value = policy_event_value(s, event, &aux);
        if (!pv_nilp(value)) {
            batch_item(&batch->body, TPL_ITEM_DISPATCH, serial, value, aux);
            if (!(policy_event_flags(event->type) & EVF_FACT)) {
                batch_item(&batch->replay, TPL_ITEM_DISPATCH, serial, value, aux);
                batch->replay_count++;
            }
            batch->head.count++;
            batch->dispatches++;
        }
        if (batch->body.failed || batch->replay.failed) {
            batch_rewind(batch);
            break;
        }
        batch->last = event->type;
        if (event->type == EV_GRAB || batch->dispatches >= LINK_BATCH) {
            batch->consumed++;
            break;
        }
    }
    pv_error_target = saved;
    return true;
}

void policy_drain(struct tomoe *s) {
    if (!policy_events_pending(s)) return;
    struct policy_link *link = s->policy_link;
    struct batch batch = { 0 };
    timing_event_taken(s);
    bool ok = batch_encode(s, &batch);
    pv_arena_reset();
    if (!ok) {
        tomoe_log(LOG_ERROR, "tomoe: cannot encode events: %s", pv_error_message());
        batch.consumed++;
    } else if (!batch.consumed && s->event_count) {
        tomoe_log(LOG_ERROR, "tomoe: out of memory encoding an event; dropping it");
        batch.consumed = 1;
    }
    batch.head.first_serial = s->event_serial - s->event_count + 1;
    policy_ring_consume(s, batch.consumed);
    if (batch.head.count && !batch.body.failed) {
        policy_state(s, &batch.head.state);
        link->batch_seq = ++link->seq ? link->seq : ++link->seq;
        if (batch.dispatches) {
            inflight_drop(link);
            struct tpl_events replay = batch.head;
            replay.count = batch.replay_count;
            link->inflight = batch.replay.failed || !batch.replay_count ? NULL :
                malloc(sizeof(replay) + batch.replay.length);
            if (link->inflight) {
                memcpy(link->inflight, &replay, sizeof(replay));
                memcpy(link->inflight + sizeof(replay), batch.replay.data, batch.replay.length);
                link->inflight_length = sizeof(replay) + batch.replay.length;
            }
        }
        if (link_send(link, TPL_EVENTS, link->batch_seq, &batch.head, sizeof(batch.head),
                batch.body.data, batch.body.length) && batch.dispatches) {
            link->batch = true;
            link_watch(link);
        }
    }
    pv_buf_free(&batch.body);
    pv_buf_free(&batch.replay);
    timing_event_done(s);
    if (batch.last == EV_GRAB) policy_event_grab_hold(s);
}

bool policy_link_floor(struct tomoe *s, const char *command) {
    if (!s || policy_link_up(s) || !command) return false;
    tomoe_log(LOG_WARNING, "tomoe: tomoe-policy is down; key binding %s runs on the floor", command);
    if (!strcmp(command, "close-window")) {
        uint32_t focus = tomoe_keyboard_focus(s);
        if (focus) tomoe_close(s, focus);
        return true;
    }
    if (!strcmp(command, "focus-next-window") || !strcmp(command, "next")) {
        window_floor_cycle(s, 1);
        return true;
    }
    if (!strcmp(command, "focus-previous-window") || !strcmp(command, "previous")) {
        window_floor_cycle(s, -1);
        return true;
    }
    return false;
}

uint32_t policy_link_generation(struct tomoe *s) {
    return s && s->policy_link ? s->policy_link->generation : 0;
}

bool policy_accepting(struct tomoe *s) {
    return s && s->policy_accepting;
}

bool policy_loading(struct tomoe *s) {
    struct policy_link *link = s ? s->policy_link : NULL;
    return !link || link->fd < 0 || !link->ready;
}
