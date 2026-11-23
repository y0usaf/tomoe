#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/pidfd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#define listen tomoe_listen
#include "internal.h"
#include "xwayland-shell-v1-protocol.h"
#include "xwm_proto.h"

#define BRIDGE_IN_CAP (XWM_MAX_PAYLOAD + sizeof(struct xwm_hdr) + 1024)
#define BRIDGE_OUT_MAX (512u << 10)
#define BRIDGE_FD_CAP 64
#define BRIDGE_PING_DEADLINE_MS 3000
#define BRIDGE_KEEPALIVE_MS 10000

struct xwm_bridge;

struct xw_pair {
    struct wl_list link;
    uint64_t serial;
    uint32_t xid;
    struct surface *surface;
    struct wl_listener destroy;
};

struct xw_surface {
    struct wl_resource *resource;
    struct xwm_bridge *bridge;
    struct surface *surface;
    uint64_t serial;
    bool committed;
    struct wl_listener destroy;
};

struct xwm_window {
    struct xwm_bridge *bridge;
    struct wl_list link;
    uint32_t xid;
    uint32_t transient;
    bool override;
    bool dialog;
    bool x_mapped;
    bool fullscreen;
    bool maximized;
    bool placed;
    int x, y, width, height;
    int placed_x, placed_y;
    char *title;
    char *appid;
    struct surface *surface;
    struct wl_listener surface_destroy;
    struct wl_listener surface_map;
    struct wl_listener surface_unmap;
    struct xdg_surface *base;
    struct xdg_toplevel *toplevel;
    struct window *window;
    struct node *node;
    struct target target;
};

struct bridge_out_item {
    struct wl_list link;
    uint8_t *data;
    size_t len;
    size_t sent;
    int fd;
};

struct xwm_bridge {
    struct tomoe *server;
    struct wl_global *global;
    struct wl_client *client;
    struct wl_list pairs;
    struct wl_list windows;
    struct wl_list out_queue;
    size_t out_bytes;
    int display_num;
    bool started_once;
    bool stopping;
    bool broken;
    pid_t sidecar_pid;
    pid_t killing_pid;
    pid_t xwayland_pid;
    int pidfd;
    int fd;
    struct wl_event_source *fd_source;
    struct wl_event_source *exit_source;
    struct wl_event_source *retry_source;
    struct wl_event_source *disconnect_idle;
    struct wl_event_source *kill_source;
    struct wl_event_source *ping_deadline;
    struct wl_event_source *keepalive;
    uint32_t ping_serial;
    bool ping_pending;
    struct timespec started_at;
    int failures;
    uint8_t *in_buf;
    size_t in_len;
    size_t in_cap;
    int in_fds[BRIDGE_FD_CAP];
    size_t in_fd_head;
    size_t in_fd_count;
};

static int bridge_spawn(struct xwm_bridge *b);
static void bridge_disconnect(struct xwm_bridge *b, bool schedule_retry);
static bool bridge_drain_input(struct xwm_bridge *b);

static int take_in_fd(struct xwm_bridge *b) {
    if (!b->in_fd_count) return -1;
    int fd = b->in_fds[b->in_fd_head];
    b->in_fd_head = (b->in_fd_head + 1) % BRIDGE_FD_CAP;
    b->in_fd_count--;
    return fd;
}

static void clear_in_fds(struct xwm_bridge *b) {
    while (b->in_fd_count) close(take_in_fd(b));
}

static void out_queue_clear(struct xwm_bridge *b) {
    struct bridge_out_item *it, *next;
    wl_list_for_each_safe(it, next, &b->out_queue, link) {
        wl_list_remove(&it->link);
        if (it->fd >= 0) close(it->fd);
        free(it->data);
        free(it);
    }
    b->out_bytes = 0;
}

static void bridge_update_events(struct xwm_bridge *b) {
    if (!b->fd_source) return;
    uint32_t mask = WL_EVENT_READABLE;
    if (!wl_list_empty(&b->out_queue)) mask |= WL_EVENT_WRITABLE;
    wl_event_source_fd_update(b->fd_source, mask);
}

static bool bridge_flush(struct xwm_bridge *b) {
    if (b->fd < 0) return false;
    while (!wl_list_empty(&b->out_queue)) {
        struct bridge_out_item *it = wl_container_of(b->out_queue.next, it, link);
        struct iovec iov = { it->data + it->sent, it->len - it->sent };
        struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
        char cbuf[CMSG_SPACE(sizeof(int))];
        if (it->fd >= 0 && it->sent == 0) {
            memset(cbuf, 0, sizeof(cbuf));
            msg.msg_control = cbuf;
            msg.msg_controllen = sizeof(cbuf);
            struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(c), &it->fd, sizeof(int));
        }
        ssize_t n;
        do n = sendmsg(b->fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        while (n < 0 && errno == EINTR);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                bridge_update_events(b);
                return true;
            }
            return false;
        }
        if (it->fd >= 0 && it->sent == 0) {
            close(it->fd);
            it->fd = -1;
        }
        it->sent += (size_t)n;
        b->out_bytes -= (size_t)n;
        if (it->sent >= it->len) {
            wl_list_remove(&it->link);
            free(it->data);
            free(it);
        }
    }
    bridge_update_events(b);
    return true;
}

static void bridge_disconnect_idle(void *data) {
    struct xwm_bridge *b = data;
    b->disconnect_idle = NULL;
    if (!b->stopping) bridge_disconnect(b, true);
}

static void bridge_schedule_disconnect(struct xwm_bridge *b) {
    b->broken = true;
    if (!b->disconnect_idle && !b->stopping)
        b->disconnect_idle = wl_event_loop_add_idle(
            wl_display_get_event_loop(b->server->display), bridge_disconnect_idle, b);
}

static bool bridge_send_raw(struct xwm_bridge *b, uint16_t type, const void *payload, uint32_t len, int fd) {
    if (!b || b->fd < 0 || b->broken || len > XWM_MAX_PAYLOAD) {
        if (fd >= 0) close(fd);
        return false;
    }
    size_t total = sizeof(struct xwm_hdr) + len;
    if (b->out_bytes + total > BRIDGE_OUT_MAX) {
        if (fd >= 0) close(fd);
        bridge_schedule_disconnect(b);
        return false;
    }
    struct bridge_out_item *it = calloc(1, sizeof(*it));
    if (!it) {
        if (fd >= 0) close(fd);
        return false;
    }
    it->data = malloc(total);
    if (!it->data) {
        free(it);
        if (fd >= 0) close(fd);
        return false;
    }
    struct xwm_hdr hdr = {
        .magic = XWM_PROTO_MAGIC,
        .version = XWM_PROTO_VERSION,
        .type = type,
        .len = len,
        .flags = fd >= 0 ? XWM_FLAG_HAS_FD : 0u,
    };
    memcpy(it->data, &hdr, sizeof(hdr));
    if (len && payload) memcpy(it->data + sizeof(hdr), payload, len);
    it->len = total;
    it->fd = fd;
    wl_list_insert(b->out_queue.prev, &it->link);
    b->out_bytes += total;
    if (!bridge_flush(b)) {
        bridge_schedule_disconnect(b);
        return false;
    }
    return true;
}

static int bridge_ping_expired(void *data) {
    struct xwm_bridge *b = data;
    if (!b->ping_pending || b->stopping) return 0;
    uint32_t expected = b->ping_serial;
    if (b->fd >= 0 && !b->broken && !bridge_drain_input(b)) {
        bridge_disconnect(b, true);
        return 0;
    }
    if (!b->ping_pending || b->ping_serial != expected || b->stopping || b->fd < 0 || b->broken)
        return 0;
    tomoe_log(LOG_ERROR, "tomoe: tomoe-xwm %d did not answer a liveness ping in %d ms; restarting it",
        (int)b->sidecar_pid, BRIDGE_PING_DEADLINE_MS);
    b->ping_pending = false;
    if (b->sidecar_pid > 0) kill(b->sidecar_pid, SIGKILL);
    bridge_disconnect(b, true);
    return 0;
}

static void bridge_ping(struct xwm_bridge *b) {
    if (b->fd < 0 || b->broken || b->stopping || b->ping_pending) return;
    if (!b->ping_deadline)
        b->ping_deadline = wl_event_loop_add_timer(wl_display_get_event_loop(b->server->display),
            bridge_ping_expired, b);
    if (!b->ping_deadline) return;
    struct xwm_msg_ping ping = { .serial = ++b->ping_serial };
    if (!bridge_send_raw(b, XWM_CMD_PING, &ping, sizeof(ping), -1)) return;
    b->ping_pending = true;
    wl_event_source_timer_update(b->ping_deadline, BRIDGE_PING_DEADLINE_MS);
}

static int bridge_keepalive(void *data) {
    struct xwm_bridge *b = data;
    if (b->stopping || b->xwayland_pid <= 0 || b->fd < 0) return 0;
    bridge_ping(b);
    wl_event_source_timer_update(b->keepalive, BRIDGE_KEEPALIVE_MS);
    return 0;
}

static bool bridge_send(struct xwm_bridge *b, uint16_t type, const void *payload, uint32_t len, int fd) {
    bool sent = bridge_send_raw(b, type, payload, len, fd);
    if (sent) bridge_ping(b);
    return sent;
}

static void logical_to_world(struct tomoe *s, int lx, int ly, int *ox, int *oy, double *scale) {
    struct output *best = NULL;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (!output_is_active(o)) continue;
        if (!best) best = o;
        double sc = snapped_scale(o->screen->scale);
        int width = logical_size(o->screen->width, sc);
        int height = logical_size(o->screen->height, sc);
        if (lx >= o->screen->lx && ly >= o->screen->ly &&
                lx < o->screen->lx + width && ly < o->screen->ly + height) {
            best = o;
            break;
        }
    }
    if (!best) {
        *ox = lx;
        *oy = ly;
        *scale = reference_scale(s);
        return;
    }
    double sc = snapped_scale(best->screen->scale);
    *ox = best->x + pixel_round((lx - best->screen->lx) * sc);
    *oy = best->y + pixel_round((ly - best->screen->ly) * sc);
    *scale = sc;
}

static void world_to_logical(struct tomoe *s, int wx, int wy, int *lx, int *ly) {
    struct output *best = NULL;
    struct output *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (!output_is_active(o)) continue;
        if (!best) best = o;
        if (wx >= o->x && wy >= o->y && wx < o->x + o->screen->width &&
                wy < o->y + o->screen->height) {
            best = o;
            break;
        }
    }
    if (!best) {
        *lx = wx;
        *ly = wy;
        return;
    }
    double sc = snapped_scale(best->screen->scale);
    *lx = best->screen->lx + pixel_round((wx - best->x) / sc);
    *ly = best->screen->ly + pixel_round((wy - best->y) / sc);
}

static struct xwm_window *window_for(struct xwm_bridge *b, uint32_t xid) {
    struct xwm_window *w;
    wl_list_for_each(w, &b->windows, link)
        if (w->xid == xid) return w;
    return NULL;
}

static void sync_parent(struct xwm_window *w) {
    if (!w || !w->toplevel) return;
    struct xwm_window *parent = w->transient ? window_for(w->bridge, w->transient) : NULL;
    w->toplevel->parent = parent && parent != w ? parent->toplevel : NULL;
    wl_signal_emit_mutable(&w->toplevel->events.metadata, NULL);
}

static void replace_string(char **slot, const char *value) {
    char *copy = value ? strdup(value) : NULL;
    if (value && !copy) return;
    free(*slot);
    *slot = copy;
}

static void push_metadata(struct xwm_window *w) {
    if (!w->toplevel) return;
    replace_string(&w->toplevel->title, w->title ? w->title : "");
    replace_string(&w->toplevel->app_id, w->appid ? w->appid : "");
    w->toplevel->external_dialog = w->dialog;
    w->toplevel->current.fullscreen = w->fullscreen;
    w->toplevel->current.maximized = w->maximized;
    w->toplevel->scheduled.fullscreen = w->fullscreen;
    w->toplevel->scheduled.maximized = w->maximized;
    wl_signal_emit_mutable(&w->toplevel->events.set_title, NULL);
    wl_signal_emit_mutable(&w->toplevel->events.set_app_id, NULL);
    wl_signal_emit_mutable(&w->toplevel->events.metadata, NULL);
}

static void override_place(struct xwm_window *w) {
    if (!w->node) return;
    int x = 0, y = 0;
    double scale = 1;
    logical_to_world(w->bridge->server, w->x, w->y, &x, &y, &scale);
    w->target.x = x;
    w->target.y = y;
    w->target.scale = scale;
    w->target.client_width = w->width;
    w->target.client_height = w->height;
    if (w->surface) surface_set_scale(w->surface, scale);
    node_set_enabled(w->node, w->surface && w->surface->mapped && w->x_mapped);
    if (w->x_mapped) node_raise_to_top(w->node);
    schedule_scene(w->bridge->server);
}

static void surface_gone(struct wl_listener *listener, void *data) {
    struct xwm_window *w = wl_container_of(listener, w, surface_destroy);
    detach(&w->surface_destroy);
    detach(&w->surface_map);
    detach(&w->surface_unmap);
    w->surface = NULL;
    if (w->override && w->node) {
        node_destroy(w->node);
        w->node = NULL;
    }
}

static void override_mapped(struct wl_listener *listener, void *data) {
    struct xwm_window *w = wl_container_of(listener, w, surface_map);
    if (!w->node) return;
    node_set_enabled(w->node, w->x_mapped);
    if (w->x_mapped) node_raise_to_top(w->node);
    schedule_scene(w->bridge->server);
}

static void override_unmapped(struct wl_listener *listener, void *data) {
    struct xwm_window *w = wl_container_of(listener, w, surface_unmap);
    if (w->node) node_set_enabled(w->node, false);
    schedule_scene(w->bridge->server);
}

static bool attach_override(struct xwm_window *w, struct surface *surface) {
    detach(&w->surface_map);
    detach(&w->surface_unmap);
    if (w->node && w->surface != surface) {
        node_destroy(w->node);
        w->node = NULL;
    }
    if (!w->node) {
        w->node = node_create(w->bridge->server->popup_tree);
        if (!w->node || !node_surface_create(w->node, surface)) {
            if (w->node) node_destroy(w->node);
            w->node = NULL;
            return false;
        }
        w->node->data = &w->target;
        w->target.id = 0;
        w->target.kind = TARGET_UNMANAGED;
        w->target.style = (struct window_style){ -1, -1, -1, -1, -1 };
        w->target.alpha = 1;
    }
    w->surface = surface;
    listen(&w->surface_destroy, &surface->events.destroy, surface_gone);
    listen(&w->surface_map, &surface->events.map, override_mapped);
    listen(&w->surface_unmap, &surface->events.unmap, override_unmapped);
    override_place(w);
    return true;
}

static bool attach_managed(struct xwm_window *w, struct surface *surface) {
    if (!w->toplevel) {
        struct xdg_surface *base = calloc(1, sizeof(*base));
        struct xdg_toplevel *toplevel = calloc(1, sizeof(*toplevel));
        if (!base || !toplevel) {
            free(base);
            free(toplevel);
            return false;
        }
        base->server = w->bridge->server;
        base->surface = surface;
        base->role = 1;
        base->initialized = true;
        base->initial_commit = false;
        base->geometry = (struct box){ 0, 0, w->width > 0 ? w->width : 1,
            w->height > 0 ? w->height : 1 };
        wl_signal_init(&toplevel->events.destroy);
        wl_signal_init(&toplevel->events.set_title);
        wl_signal_init(&toplevel->events.set_app_id);
        wl_signal_init(&toplevel->events.request_maximize);
        wl_signal_init(&toplevel->events.request_fullscreen);
        wl_signal_init(&toplevel->events.request_minimize);
        wl_signal_init(&toplevel->events.request_move);
        wl_signal_init(&toplevel->events.request_resize);
        wl_signal_init(&toplevel->events.metadata);
        toplevel->base = base;
        toplevel->xwm = w;
        toplevel->external_dialog = w->dialog;
        toplevel->title = strdup(w->title ? w->title : "");
        toplevel->app_id = strdup(w->appid ? w->appid : "");
        toplevel->current.fullscreen = w->fullscreen;
        toplevel->current.maximized = w->maximized;
        toplevel->scheduled = toplevel->current;
        base->toplevel = toplevel;
        w->base = base;
        w->toplevel = toplevel;
        w->window = xdg_toplevel_created(w->bridge->server, toplevel);
        if (!w->window) {
            free(toplevel->title);
            free(toplevel->app_id);
            free(toplevel);
            free(base);
            w->base = NULL;
            w->toplevel = NULL;
            return false;
        }
        w->surface = surface;
        listen(&w->surface_destroy, &surface->events.destroy, surface_gone);
        sync_parent(w);
        struct xwm_window *child;
        wl_list_for_each(child, &w->bridge->windows, link)
            if (child->transient == w->xid && child != w) sync_parent(child);
        return true;
    }
    w->surface = surface;
    listen(&w->surface_destroy, &surface->events.destroy, surface_gone);
    if (!window_attach_surface(w->window, surface)) return false;
    push_metadata(w);
    return true;
}

static void release_managed(struct xwm_window *w) {
    struct xdg_toplevel *toplevel = w->toplevel;
    struct xdg_surface *base = w->base;
    if (!toplevel) return;
    detach(&w->surface_destroy);
    if (w->surface && w->surface->external_toplevel == toplevel)
        w->surface->external_toplevel = NULL;
    struct xwm_window *other;
    wl_list_for_each(other, &w->bridge->windows, link)
        if (other->toplevel && other->toplevel->parent == toplevel) other->toplevel->parent = NULL;
    w->toplevel = NULL;
    w->base = NULL;
    w->window = NULL;
    w->surface = NULL;
    w->placed = false;
    wl_signal_emit_mutable(&toplevel->events.destroy, NULL);
    free(toplevel->title);
    free(toplevel->app_id);
    free(toplevel->tag);
    free(toplevel->description);
    free(toplevel);
    if (base && base->scene) node_destroy(base->scene);
    free(base);
}

static void window_free(struct xwm_window *w) {
    detach(&w->surface_destroy);
    detach(&w->surface_map);
    detach(&w->surface_unmap);
    char *title = w->title;
    char *appid = w->appid;
    struct node *node = w->node;
    if (w->override) {
        if (node) node_destroy(node);
    } else {
        release_managed(w);
    }
    wl_list_remove(&w->link);
    free(w);
    free(title);
    free(appid);
}

static struct xwm_window *window_new(struct xwm_bridge *b, uint32_t xid, bool override,
        int x, int y, int width, int height) {
    struct xwm_window *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->bridge = b;
    w->xid = xid;
    w->override = override;
    w->x = x;
    w->y = y;
    w->width = width;
    w->height = height;
    wl_list_init(&w->surface_destroy.link);
    wl_list_init(&w->surface_map.link);
    wl_list_init(&w->surface_unmap.link);
    wl_list_insert(&b->windows, &w->link);
    return w;
}

static void associate_surface(struct xwm_bridge *b, uint32_t xid, struct surface *surface) {
    if (!b || !surface) return;
    struct xwm_window *w = window_for(b, xid);
    if (!w || w->surface == surface) return;
    detach(&w->surface_destroy);
    if (w->override) attach_override(w, surface);
    else attach_managed(w, surface);
}

static void pair_free(struct xw_pair *pair) {
    detach(&pair->destroy);
    wl_list_remove(&pair->link);
    free(pair);
}

static void pair_surface_gone(struct wl_listener *listener, void *data) {
    struct xw_pair *pair = wl_container_of(listener, pair, destroy);
    pair->surface = NULL;
    if (pair->xid) return;
    pair_free(pair);
}

static struct xw_pair *pair_for(struct xwm_bridge *b, uint64_t serial) {
    struct xw_pair *pair;
    wl_list_for_each(pair, &b->pairs, link)
        if (pair->serial == serial) return pair;
    return NULL;
}

static struct xw_pair *pair_new(struct xwm_bridge *b, uint64_t serial) {
    struct xw_pair *pair = calloc(1, sizeof(*pair));
    if (!pair) return NULL;
    pair->serial = serial;
    wl_list_init(&pair->destroy.link);
    wl_list_insert(&b->pairs, &pair->link);
    return pair;
}

static void pair_ready(struct xwm_bridge *b, struct xw_pair *pair) {
    if (!pair->xid || !pair->surface) return;
    struct surface *surface = pair->surface;
    uint32_t xid = pair->xid;
    pair_free(pair);
    associate_surface(b, xid, surface);
}

static void pair_surface(struct xwm_bridge *b, uint64_t serial, struct surface *surface) {
    if (!b || !serial || !surface) return;
    struct xw_pair *pair = pair_for(b, serial);
    if (!pair) pair = pair_new(b, serial);
    if (!pair) return;
    if (pair->surface && pair->surface != surface) return;
    pair->surface = surface;
    listen(&pair->destroy, &surface->events.destroy, pair_surface_gone);
    pair_ready(b, pair);
}

static void pair_window(struct xwm_bridge *b, uint32_t xid, uint64_t serial) {
    if (!b || !xid || !serial) return;
    struct xw_pair *pair = pair_for(b, serial);
    if (!pair) pair = pair_new(b, serial);
    if (!pair) return;
    pair->xid = xid;
    pair_ready(b, pair);
}

static void x11_state_clear(struct xwm_bridge *b) {
    struct xwm_window *w, *wn;
    wl_list_for_each_safe(w, wn, &b->windows, link) window_free(w);
    struct xw_pair *p, *pn;
    wl_list_for_each_safe(p, pn, &b->pairs, link) pair_free(p);
    b->xwayland_pid = 0;
    b->client = NULL;
    bool x11 = false;
    if (b->server && b->server->seat) {
        if (selection_source_info(b->server, false, &x11, NULL, NULL) && x11)
            selection_x11_clear(b->server, false);
        if (selection_source_info(b->server, true, &x11, NULL, NULL) && x11)
            selection_x11_clear(b->server, true);
    }
}

static void xw_surface_destroyed(struct wl_resource *resource) {
    struct xw_surface *xws = wl_resource_get_user_data(resource);
    if (!xws) return;
    detach(&xws->destroy);
    free(xws);
}

static void xw_surface_surface_gone(struct wl_listener *listener, void *data) {
    struct xw_surface *xws = wl_container_of(listener, xws, destroy);
    detach(&xws->destroy);
    xws->surface = NULL;
}

static const struct surface_role xwayland_surface_role;

static void xw_surface_commit(struct surface *surface) {
    if (!surface->role_resource) return;
    struct xw_surface *xws = wl_resource_get_user_data(surface->role_resource);
    if (!xws) return;
    if (!xws->committed && xws->serial) {
        xws->committed = true;
        pair_surface(xws->bridge, xws->serial, surface);
    }
    if (surface_has_buffer(surface) && !surface->mapped) surface_map(surface);
}

static void xw_surface_destroy_role(struct surface *surface) {
    if (!surface->role_resource) return;
    struct xw_surface *xws = wl_resource_get_user_data(surface->role_resource);
    if (xws) {
        xws->committed = false;
        xws->serial = 0;
    }
}

static const struct surface_role xwayland_surface_role = {
    .name = "xwayland_surface",
    .keep_buffer = true,
    .commit = xw_surface_commit,
    .destroy = xw_surface_destroy_role,
};

static void set_serial(struct wl_client *client, struct wl_resource *resource,
        uint32_t serial_lo, uint32_t serial_hi) {
    struct xw_surface *xws = wl_resource_get_user_data(resource);
    if (!xws) return;
    uint64_t serial = ((uint64_t)serial_hi << 32) | serial_lo;
    if (!serial) {
        wl_resource_post_error(resource, XWAYLAND_SURFACE_V1_ERROR_INVALID_SERIAL,
            "the serial must be non-zero");
        return;
    }
    if (xws->committed) {
        wl_resource_post_error(resource, XWAYLAND_SURFACE_V1_ERROR_ALREADY_ASSOCIATED,
            "the surface was already associated with a window");
        return;
    }
    xws->serial = serial;
}

static void destroy_surface(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct xwayland_surface_v1_interface xwayland_surface_impl = {
    .set_serial = set_serial, .destroy = destroy_surface,
};

static void get_xwayland_surface(struct wl_client *client, struct wl_resource *resource,
        uint32_t id, struct wl_resource *surface_resource) {
    struct xwm_bridge *b = wl_resource_get_user_data(resource);
    struct surface *surface = surface_from_resource(surface_resource);
    if (!b || !surface || client != b->client) return;
    if (!surface_set_role(surface, &xwayland_surface_role, surface_resource,
            XWAYLAND_SHELL_V1_ERROR_ROLE)) return;
    struct xw_surface *xws = calloc(1, sizeof(*xws));
    struct wl_resource *child = xws ? wl_resource_create(client,
        &xwayland_surface_v1_interface, 1, id) : NULL;
    if (!child) {
        free(xws);
        wl_client_post_no_memory(client);
        return;
    }
    xws->resource = child;
    xws->bridge = b;
    xws->surface = surface;
    wl_list_init(&xws->destroy.link);
    listen(&xws->destroy, &surface->events.destroy, xw_surface_surface_gone);
    wl_resource_set_implementation(child, &xwayland_surface_impl, xws, xw_surface_destroyed);
    surface_set_role_object(surface, child);
}

static void destroy_shell(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct xwayland_shell_v1_interface xwayland_shell_impl = {
    .destroy = destroy_shell, .get_xwayland_surface = get_xwayland_surface,
};

static void bind_shell(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct xwm_bridge *b = data;
    uid_t uid; gid_t gid; pid_t pid = 0;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    if ((b->xwayland_pid <= 0 || pid != b->xwayland_pid) && b->fd >= 0) {
        if (!bridge_drain_input(b)) {
            bridge_disconnect(b, true);
            return;
        }
    }
    if (b->xwayland_pid <= 0 || pid != b->xwayland_pid) return;
    b->client = client;
    struct wl_resource *resource = wl_resource_create(client, &xwayland_shell_v1_interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &xwayland_shell_impl, b, NULL);
}

static void handle_message(struct xwm_bridge *b, const struct xwm_hdr *hdr,
        const uint8_t *payload, int fd) {
    switch (hdr->type) {
    case XWM_EVT_HELLO:
        break;
    case XWM_EVT_PONG: {
        if (hdr->len < sizeof(struct xwm_msg_ping)) break;
        struct xwm_msg_ping pong;
        memcpy(&pong, payload, sizeof(pong));
        if (b->ping_pending && pong.serial == b->ping_serial) {
            b->ping_pending = false;
            if (b->ping_deadline) wl_event_source_timer_update(b->ping_deadline, 0);
        }
        break;
    }
    case XWM_EVT_XWAYLAND_PID: {
        if (hdr->len < sizeof(struct xwm_msg_pid)) break;
        struct xwm_msg_pid msg;
        memcpy(&msg, payload, sizeof(msg));
        if (msg.pid <= 0) {
            if (b->keepalive) wl_event_source_timer_update(b->keepalive, 0);
            x11_state_clear(b);
        } else {
            b->xwayland_pid = (pid_t)msg.pid;
            if (!b->keepalive)
                b->keepalive = wl_event_loop_add_timer(wl_display_get_event_loop(b->server->display),
                    bridge_keepalive, b);
            if (b->keepalive) wl_event_source_timer_update(b->keepalive, BRIDGE_KEEPALIVE_MS);
            xwm_wayland_selection(b->server, false, true);
            xwm_wayland_selection(b->server, true, true);
        }
        break;
    }
    case XWM_EVT_WINDOW_CREATE: {
        if (hdr->len < sizeof(struct xwm_msg_window_create)) break;
        struct xwm_msg_window_create msg;
        memcpy(&msg, payload, sizeof(msg));
        struct xwm_window *w = window_for(b, msg.xid);
        if (!w) w = window_new(b, msg.xid, msg.override != 0, msg.x, msg.y, msg.width, msg.height);
        if (w) {
            w->transient = msg.transient;
            w->dialog = msg.dialog != 0;
        }
        break;
    }
    case XWM_EVT_WINDOW_DESTROY: {
        if (hdr->len < sizeof(struct xwm_msg_window_id)) break;
        struct xwm_msg_window_id msg;
        memcpy(&msg, payload, sizeof(msg));
        struct xwm_window *w = window_for(b, msg.xid);
        if (w) window_free(w);
        break;
    }
    case XWM_EVT_WINDOW_PAIR: {
        if (hdr->len < sizeof(struct xwm_msg_window_pair)) break;
        struct xwm_msg_window_pair msg;
        memcpy(&msg, payload, sizeof(msg));
        pair_window(b, msg.xid, msg.serial);
        break;
    }
    case XWM_EVT_WINDOW_MAP: {
        if (hdr->len < sizeof(struct xwm_msg_window_map)) break;
        struct xwm_msg_window_map msg;
        memcpy(&msg, payload, sizeof(msg));
        struct xwm_window *w = window_for(b, msg.xid);
        if (!w) w = window_new(b, msg.xid, msg.override != 0, msg.x, msg.y, msg.width, msg.height);
        if (!w) break;
        w->x = msg.x;
        w->y = msg.y;
        w->width = msg.width;
        w->height = msg.height;
        w->override = msg.override != 0;
        w->dialog = msg.dialog != 0;
        w->fullscreen = msg.fullscreen != 0;
        w->maximized = msg.maximized != 0;
        w->x_mapped = true;
        if (w->override) override_place(w);
        break;
    }
    case XWM_EVT_WINDOW_UNMAP: {
        if (hdr->len < sizeof(struct xwm_msg_window_id)) break;
        struct xwm_msg_window_id msg;
        memcpy(&msg, payload, sizeof(msg));
        struct xwm_window *w = window_for(b, msg.xid);
        if (!w) break;
        w->x_mapped = false;
        if (w->override) {
            if (w->node) node_set_enabled(w->node, false);
            schedule_scene(b->server);
        } else {
            release_managed(w);
        }
        break;
    }
    case XWM_EVT_WINDOW_GEOMETRY: {
        if (hdr->len < sizeof(struct xwm_msg_window_geom)) break;
        struct xwm_msg_window_geom msg;
        memcpy(&msg, payload, sizeof(msg));
        struct xwm_window *w = window_for(b, msg.xid);
        if (!w) break;
        w->x = msg.x;
        w->y = msg.y;
        w->width = msg.width;
        w->height = msg.height;
        if (w->base) {
            w->base->geometry.width = w->width;
            w->base->geometry.height = w->height;
        }
        if (w->override) override_place(w);
        else if (w->window) window_client_resized(w->window);
        schedule_scene(b->server);
        break;
    }
    case XWM_EVT_WINDOW_META: {
        if (hdr->len < sizeof(struct xwm_msg_window_meta)) break;
        struct xwm_msg_window_meta msg;
        memcpy(&msg, payload, sizeof(msg));
        if (sizeof(msg) + msg.title_len + msg.appid_len > hdr->len) break;
        struct xwm_window *w = window_for(b, msg.xid);
        if (!w) break;
        w->transient = msg.transient;
        w->dialog = msg.dialog != 0;
        w->fullscreen = msg.fullscreen != 0;
        w->maximized = msg.maximized != 0;
        const char *strings = (const char *)(payload + sizeof(msg));
        if (msg.title_len > 0) {
            char *t = strndup(strings, msg.title_len);
            if (t) { free(w->title); w->title = t; }
        } else {
            replace_string(&w->title, "");
        }
        if (msg.appid_len > 0) {
            char *a = strndup(strings + msg.title_len, msg.appid_len);
            if (a) { free(w->appid); w->appid = a; }
        } else {
            replace_string(&w->appid, "");
        }
        push_metadata(w);
        sync_parent(w);
        break;
    }
    case XWM_EVT_WINDOW_STATE_REQ: {
        if (hdr->len < sizeof(struct xwm_msg_state_req)) break;
        struct xwm_msg_state_req msg;
        memcpy(&msg, payload, sizeof(msg));
        struct xwm_window *w = window_for(b, msg.xid);
        if (!w || !w->toplevel) break;
        if (msg.kind == XWM_STATE_REQ_FULLSCREEN) {
            bool want = msg.action == 1 ? true : (msg.action == 0 ? false : !w->fullscreen);
            w->toplevel->requested.fullscreen = want;
            wl_signal_emit_mutable(&w->toplevel->events.request_fullscreen, NULL);
        } else if (msg.kind == XWM_STATE_REQ_MAXIMIZED) {
            bool want = msg.action == 1 ? true : (msg.action == 0 ? false : !w->maximized);
            w->toplevel->requested.maximized = want;
            wl_signal_emit_mutable(&w->toplevel->events.request_maximize, NULL);
        } else if (msg.kind == XWM_STATE_REQ_ACTIVATE) {
            if (w->window) window_external_activate(w->window);
        } else if (msg.kind == XWM_STATE_REQ_MINIMIZE) {
            wl_signal_emit_mutable(&w->toplevel->events.request_minimize, NULL);
        } else if (msg.kind == XWM_STATE_REQ_CLOSE) {
            xwm_close(w->toplevel);
        }
        break;
    }
    case XWM_EVT_SELECTION_OFFER: {
        if (hdr->len < sizeof(struct xwm_msg_sel_offer)) break;
        struct xwm_msg_sel_offer msg;
        memcpy(&msg, payload, sizeof(msg));
        size_t max_count = msg.count;
        if (max_count > 128) max_count = 128;
        const char *mimes[128];
        size_t count = 0;
        const char *p = (const char *)(payload + sizeof(msg));
        const char *end = (const char *)(payload + hdr->len);
        while (p < end && count < max_count) {
            const char *nul = memchr(p, '\0', (size_t)(end - p));
            if (!nul) break;
            if (nul > p) mimes[count++] = p;
            p = nul + 1;
        }
        if (count > 0) selection_x11_offer(b->server, msg.primary != 0, mimes, count);
        break;
    }
    case XWM_EVT_SELECTION_CLEAR: {
        if (hdr->len < sizeof(struct xwm_msg_sel_clear)) break;
        struct xwm_msg_sel_clear msg;
        memcpy(&msg, payload, sizeof(msg));
        bool x11 = false;
        if (selection_source_info(b->server, msg.primary != 0, &x11, NULL, NULL) && x11)
            selection_x11_clear(b->server, msg.primary != 0);
        break;
    }
    case XWM_EVT_SELECTION_REQUEST: {
        if (hdr->len < sizeof(struct xwm_msg_sel_req)) break;
        struct xwm_msg_sel_req msg;
        memcpy(&msg, payload, sizeof(msg));
        if (sizeof(msg) + msg.mime_len > hdr->len || fd < 0) break;
        char mime[256];
        size_t copy = msg.mime_len < sizeof(mime) - 1 ? msg.mime_len : sizeof(mime) - 1;
        memcpy(mime, payload + sizeof(msg), copy);
        mime[copy] = '\0';
        int send_fd = fd;
        fd = -1;
        if (!selection_source_send(b->server, msg.primary != 0, mime, send_fd)) close(send_fd);
        break;
    }
    default:
        break;
    }
    if (fd >= 0) close(fd);
}

static bool bridge_drain_input(struct xwm_bridge *b) {
    for (;;) {
        if (b->in_len == b->in_cap) break;
        char cbuf[CMSG_SPACE(sizeof(int) * 8)];
        struct iovec iov = { b->in_buf + b->in_len, b->in_cap - b->in_len };
        struct msghdr msg = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = cbuf, .msg_controllen = sizeof(cbuf),
        };
        ssize_t n;
        do n = recvmsg(b->fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        while (n < 0 && errno == EINTR);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return false;
        }
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int fds[8];
            if (count > 8) count = 8;
            memcpy(fds, CMSG_DATA(c), count * sizeof(int));
            for (size_t i = 0; i < count; i++) {
                if (b->in_fd_count < BRIDGE_FD_CAP) {
                    b->in_fds[(b->in_fd_head + b->in_fd_count++) % BRIDGE_FD_CAP] = fds[i];
                } else {
                    close(fds[i]);
                }
            }
        }
        b->in_len += (size_t)n;
        size_t offset = 0;
        while (b->in_len - offset >= sizeof(struct xwm_hdr)) {
            struct xwm_hdr hdr;
            memcpy(&hdr, b->in_buf + offset, sizeof(hdr));
            if (hdr.magic != XWM_PROTO_MAGIC || hdr.version != XWM_PROTO_VERSION ||
                    hdr.len > XWM_MAX_PAYLOAD) {
                return false;
            }
            size_t frame = sizeof(hdr) + hdr.len;
            if (b->in_len - offset < frame) break;
            int fd = (hdr.flags & XWM_FLAG_HAS_FD) ? take_in_fd(b) : -1;
            handle_message(b, &hdr, b->in_buf + offset + sizeof(hdr), fd);
            if (b->fd < 0 || b->broken) return true;
            offset += frame;
        }
        if (offset > 0) {
            b->in_len -= offset;
            if (b->in_len) memmove(b->in_buf, b->in_buf + offset, b->in_len);
        }
    }
    return true;
}

static int bridge_fd_ready(int fd, uint32_t mask, void *data) {
    struct xwm_bridge *b = data;
    (void)fd;
    if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
        bridge_disconnect(b, true);
        return 0;
    }
    if (mask & WL_EVENT_WRITABLE) {
        if (!bridge_flush(b)) {
            bridge_disconnect(b, true);
            return 0;
        }
    }
    if (mask & WL_EVENT_READABLE) {
        if (!bridge_drain_input(b)) {
            bridge_disconnect(b, true);
            return 0;
        }
    }
    return 0;
}

static int bridge_retry(void *data) {
    struct xwm_bridge *b = data;
    if (!b->stopping && b->sidecar_pid <= 0) bridge_spawn(b);
    return 0;
}

static void bridge_schedule_retry(struct xwm_bridge *b) {
    if (b->stopping) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - b->started_at.tv_sec) * 1000L +
        (now.tv_nsec - b->started_at.tv_nsec) / 1000000L;
    b->failures = elapsed_ms < 1000 ? b->failures + 1 : 0;
    int delay = b->failures == 0 ? 50 : (b->failures > 7 ? 10000 : (100 << (b->failures - 1)));
    if (delay > 10000) delay = 10000;
    if (!b->retry_source)
        b->retry_source = wl_event_loop_add_timer(wl_display_get_event_loop(b->server->display),
            bridge_retry, b);
    if (b->retry_source) wl_event_source_timer_update(b->retry_source, delay);
}

static int bridge_kill_escalate(void *data) {
    struct xwm_bridge *b = data;
    if (b->sidecar_pid > 0 && b->sidecar_pid == b->killing_pid)
        kill(b->sidecar_pid, SIGKILL);
    return 0;
}

static void bridge_disconnect(struct xwm_bridge *b, bool schedule_retry) {
    b->broken = false;
    b->ping_pending = false;
    if (b->ping_deadline) wl_event_source_timer_update(b->ping_deadline, 0);
    if (b->keepalive) wl_event_source_timer_update(b->keepalive, 0);
    if (b->disconnect_idle) {
        wl_event_source_remove(b->disconnect_idle);
        b->disconnect_idle = NULL;
    }
    if (b->fd_source) {
        wl_event_source_remove(b->fd_source);
        b->fd_source = NULL;
    }
    if (b->fd >= 0) {
        close(b->fd);
        b->fd = -1;
    }
    out_queue_clear(b);
    clear_in_fds(b);
    b->in_len = 0;
    x11_state_clear(b);
    if (b->sidecar_pid > 0) {
        if (waitpid(b->sidecar_pid, NULL, WNOHANG) == b->sidecar_pid) {
            b->sidecar_pid = 0;
            b->killing_pid = 0;
            if (b->kill_source) wl_event_source_timer_update(b->kill_source, 0);
        } else if (b->exit_source && !b->stopping) {
            kill(b->sidecar_pid, SIGTERM);
            b->killing_pid = b->sidecar_pid;
            if (!b->kill_source)
                b->kill_source = wl_event_loop_add_timer(
                    wl_display_get_event_loop(b->server->display), bridge_kill_escalate, b);
            if (b->kill_source) wl_event_source_timer_update(b->kill_source, 2000);
            return;
        } else if (!b->stopping) {
            reap_later(b->server, b->sidecar_pid, -1);
            b->sidecar_pid = 0;
            b->killing_pid = 0;
        }
    }
    if (b->kill_source) wl_event_source_timer_update(b->kill_source, 0);
    b->killing_pid = 0;
    if (b->exit_source) {
        wl_event_source_remove(b->exit_source);
        b->exit_source = NULL;
    }
    if (b->pidfd >= 0) {
        close(b->pidfd);
        b->pidfd = -1;
    }
    if (schedule_retry && !b->stopping) bridge_schedule_retry(b);
}

static int sidecar_exited(int fd, uint32_t mask, void *data) {
    struct xwm_bridge *b = data;
    (void)fd;
    (void)mask;
    if (b->sidecar_pid > 0) {
        while (waitpid(b->sidecar_pid, NULL, 0) < 0 && errno == EINTR) {}
        b->sidecar_pid = 0;
    }
    bridge_disconnect(b, true);
    return 0;
}

static void resolve_sidecar_path(char *out, size_t size) {
    const char *env = getenv("TOMOE_XWM");
    if (env && *env && access(env, X_OK) == 0) {
        snprintf(out, size, "%s", env);
        return;
    }
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            int len = snprintf(out, size, "%s/../libexec/tomoe-xwm", exe);
            if (len > 0 && (size_t)len < size && access(out, X_OK) == 0) return;
            len = snprintf(out, size, "%s/tomoe-xwm", exe);
            if (len > 0 && (size_t)len < size && access(out, X_OK) == 0) return;
        }
    }
    snprintf(out, size, "tomoe-xwm");
}

static int bridge_spawn(struct xwm_bridge *b) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sv) < 0) return -1;
    char binary[PATH_MAX];
    resolve_sidecar_path(binary, sizeof(binary));
    char fd_arg[16], disp_arg[16];
    snprintf(fd_arg, sizeof(fd_arg), "%d", sv[1]);
    snprintf(disp_arg, sizeof(disp_arg), "%d", b->display_num);
    fcntl(sv[1], F_SETFD, 0);
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        for (int s = 1; s < NSIG; s++) signal(s, SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        for (int i = 3; i < 1024; i++)
            if (i != sv[1]) close(i);
        char *argv[] = { binary, "--fd", fd_arg, "--display", disp_arg, NULL };
        execvp(binary, argv);
        _exit(127);
    }
    close(sv[1]);
    if (pid < 0) {
        close(sv[0]);
        return -1;
    }
    b->fd = sv[0];
    b->sidecar_pid = pid;
    clock_gettime(CLOCK_MONOTONIC, &b->started_at);
    struct wl_event_loop *loop = wl_display_get_event_loop(b->server->display);
    b->fd_source = wl_event_loop_add_fd(loop, b->fd, WL_EVENT_READABLE, bridge_fd_ready, b);
    b->pidfd = (int)pidfd_open(pid, 0);
    if (b->pidfd >= 0)
        b->exit_source = wl_event_loop_add_fd(loop, b->pidfd, WL_EVENT_READABLE, sidecar_exited, b);
    if (!b->fd_source) {
        bridge_disconnect(b, false);
        return -1;
    }
    return 0;
}

bool xwayland_listen(struct tomoe *s) {
    if (!s->xwayland) {
        struct xwm_bridge *b = calloc(1, sizeof(*b));
        if (!b) return false;
        b->in_buf = malloc(BRIDGE_IN_CAP);
        if (!b->in_buf) {
            free(b);
            return false;
        }
        b->in_cap = BRIDGE_IN_CAP;
        b->server = s;
        b->fd = -1;
        b->pidfd = -1;
        wl_list_init(&b->pairs);
        wl_list_init(&b->windows);
        wl_list_init(&b->out_queue);
        s->xwayland = (struct xwayland *)b;
    }
    struct xwm_bridge *b = (struct xwm_bridge *)s->xwayland;
    if (!b->global)
        b->global = wl_global_create(s->display, &xwayland_shell_v1_interface, 1, b, bind_shell);
    return b->global != NULL;
}

int xwayland_start(struct tomoe *s) {
    struct xwm_bridge *b = (struct xwm_bridge *)s->xwayland;
    if (!b) return -1;
    if (b->started_once) return 0;
    const char *display = getenv("DISPLAY");
    if (!display || display[0] != ':') {
        tomoe_log(LOG_ERROR, "tomoe: DISPLAY is not an X display");
        return -1;
    }
    b->display_num = atoi(display + 1);
    b->started_once = true;
    return bridge_spawn(b);
}

void xwayland_finish(struct tomoe *s) {
    struct xwm_bridge *b = s ? (struct xwm_bridge *)s->xwayland : NULL;
    if (!b) return;
    b->stopping = true;
    if (b->retry_source) {
        wl_event_source_remove(b->retry_source);
        b->retry_source = NULL;
    }
    if (b->kill_source) {
        wl_event_source_remove(b->kill_source);
        b->kill_source = NULL;
    }
    if (b->ping_deadline) {
        wl_event_source_remove(b->ping_deadline);
        b->ping_deadline = NULL;
    }
    if (b->keepalive) {
        wl_event_source_remove(b->keepalive);
        b->keepalive = NULL;
    }
    pid_t pid = b->sidecar_pid;
    b->sidecar_pid = 0;
    bridge_disconnect(b, false);
    if (pid > 0) {
        kill(pid, SIGTERM);
        for (int tries = 0; tries < 50; tries++) {
            if (waitpid(pid, NULL, WNOHANG) == pid) { pid = 0; break; }
            usleep(10000);
        }
        if (pid > 0) {
            kill(pid, SIGKILL);
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    if (b->global) wl_global_destroy(b->global);
    free(b->in_buf);
    free(b);
    s->xwayland = NULL;
}

void xwm_place(struct xdg_toplevel *toplevel, int x, int y) {
    struct xwm_window *w = toplevel->xwm;
    if (!w || !w->xid || w->override) return;
    int lx, ly;
    world_to_logical(w->bridge->server, x, y, &lx, &ly);
    if (w->placed && w->placed_x == lx && w->placed_y == ly) return;
    struct xwm_cmd_configure cmd = {
        .xid = w->xid, .x = lx, .y = ly, .has_pos = 1,
    };
    bridge_send(w->bridge, XWM_CMD_CONFIGURE, &cmd, sizeof(cmd), -1);
    w->placed = true;
    w->placed_x = lx;
    w->placed_y = ly;
}

void xwm_configure(struct xdg_toplevel *toplevel, int width, int height) {
    struct xwm_window *w = toplevel->xwm;
    if (!w || !w->xid) return;
    toplevel->scheduled.width = width;
    toplevel->scheduled.height = height;
    if (width <= 0 || height <= 0) return;
    struct xwm_cmd_configure cmd = {
        .xid = w->xid, .width = width, .height = height, .has_size = 1,
    };
    bridge_send(w->bridge, XWM_CMD_CONFIGURE, &cmd, sizeof(cmd), -1);
}

void xwm_schedule(struct xdg_toplevel *toplevel) {
    (void)toplevel;
}

void xwm_activate(struct xdg_toplevel *toplevel, bool activated) {
    struct xwm_window *w = toplevel->xwm;
    if (!w) return;
    toplevel->scheduled.activated = activated;
    struct xwm_cmd_activate cmd = { .xid = w->xid, .activated = activated ? 1 : 0 };
    bridge_send(w->bridge, XWM_CMD_ACTIVATE, &cmd, sizeof(cmd), -1);
}

void xwm_state(struct xdg_toplevel *toplevel, int fullscreen, int maximized) {
    struct xwm_window *w = toplevel->xwm;
    if (!w) return;
    struct xwm_cmd_state cmd = {
        .xid = w->xid,
        .fullscreen = fullscreen ? 1 : 0,
        .maximized = maximized ? 1 : 0,
    };
    bridge_send(w->bridge, XWM_CMD_STATE, &cmd, sizeof(cmd), -1);
    toplevel->scheduled.fullscreen = fullscreen != 0;
    toplevel->scheduled.maximized = maximized != 0;
    toplevel->current.fullscreen = fullscreen != 0;
    toplevel->current.maximized = maximized != 0;
    w->fullscreen = fullscreen != 0;
    w->maximized = maximized != 0;
    wl_signal_emit_mutable(&toplevel->events.metadata, NULL);
}

void xwm_close(struct xdg_toplevel *toplevel) {
    struct xwm_window *w = toplevel->xwm;
    if (!w) return;
    struct xwm_msg_window_id cmd = { .xid = w->xid };
    bridge_send(w->bridge, XWM_CMD_CLOSE, &cmd, sizeof(cmd), -1);
}

void xwm_wayland_selection(struct tomoe *s, bool primary, bool present) {
    struct xwm_bridge *b = s ? (struct xwm_bridge *)s->xwayland : NULL;
    if (!b || b->fd < 0) return;
    bool x11 = false;
    const char *const *mimes = NULL;
    size_t count = 0;
    bool has = selection_source_info(s, primary, &x11, &mimes, &count);
    if (has && x11) return;
    uint8_t buf[4096];
    struct xwm_cmd_wayland_sel hdr = {
        .primary = primary ? 1 : 0,
        .present = (has && present) ? 1 : 0,
        .count = 0,
    };
    size_t pos = sizeof(hdr);
    if (hdr.present && mimes) {
        for (size_t i = 0; i < count; i++) {
            size_t mlen = strlen(mimes[i]) + 1;
            if (pos + mlen > sizeof(buf)) break;
            memcpy(buf + pos, mimes[i], mlen);
            pos += mlen;
            hdr.count++;
        }
    }
    memcpy(buf, &hdr, sizeof(hdr));
    bridge_send(b, XWM_CMD_WAYLAND_SELECTION, buf, (uint32_t)pos, -1);
}

void xwm_source_send(struct tomoe *s, bool primary, const char *mime, int fd) {
    struct xwm_bridge *b = s ? (struct xwm_bridge *)s->xwayland : NULL;
    if (!b || b->fd < 0 || !mime) {
        if (fd >= 0) close(fd);
        return;
    }
    size_t mlen = strlen(mime);
    if (mlen > 255) mlen = 255;
    uint8_t buf[sizeof(struct xwm_msg_sel_req) + 256];
    struct xwm_msg_sel_req hdr = {
        .primary = primary ? 1 : 0,
        .mime_len = (uint16_t)mlen,
    };
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), mime, mlen);
    bridge_send(b, XWM_CMD_SEND_SELECTION, buf, (uint32_t)(sizeof(hdr) + mlen), fd);
}
