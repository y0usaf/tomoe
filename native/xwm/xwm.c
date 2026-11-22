#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include "xwm.h"

enum {
    A_WM_PROTOCOLS, A_WM_DELETE_WINDOW, A_WM_TAKE_FOCUS, A_WM_STATE, A_WM_CLASS, A_WM_NAME,
    A_WM_TRANSIENT_FOR, A_WM_NORMAL_HINTS, A_WM_HINTS, A_WM_CHANGE_STATE, A_WM_S0,
    A_NET_WM_NAME, A_NET_WM_STATE, A_NET_WM_STATE_FULLSCREEN, A_NET_WM_STATE_MAXIMIZED_VERT,
    A_NET_WM_STATE_MAXIMIZED_HORZ, A_NET_WM_STATE_MODAL, A_NET_WM_STATE_HIDDEN,
    A_NET_ACTIVE_WINDOW, A_NET_CLIENT_LIST, A_NET_CLIENT_LIST_STACKING, A_NET_SUPPORTED,
    A_NET_SUPPORTING_WM_CHECK, A_NET_WM_WINDOW_TYPE, A_NET_WM_WINDOW_TYPE_DIALOG,
    A_NET_CLOSE_WINDOW, A_UTF8_STRING, A_TEXT, A_TARGETS, A_TIMESTAMP, A_INCR, A_CLIPBOARD,
    A_PRIMARY, A_CLIPBOARD_MANAGER, A_WL_SURFACE_SERIAL, A_WL_SURFACE_ID, A_TOMOE_CONVERT,
    A_COUNT,
};

static const char *const atom_names[A_COUNT] = {
    [A_WM_PROTOCOLS] = "WM_PROTOCOLS",
    [A_WM_DELETE_WINDOW] = "WM_DELETE_WINDOW",
    [A_WM_TAKE_FOCUS] = "WM_TAKE_FOCUS",
    [A_WM_STATE] = "WM_STATE",
    [A_WM_CLASS] = "WM_CLASS",
    [A_WM_NAME] = "WM_NAME",
    [A_WM_TRANSIENT_FOR] = "WM_TRANSIENT_FOR",
    [A_WM_NORMAL_HINTS] = "WM_NORMAL_HINTS",
    [A_WM_HINTS] = "WM_HINTS",
    [A_WM_CHANGE_STATE] = "WM_CHANGE_STATE",
    [A_WM_S0] = "WM_S0",
    [A_NET_WM_NAME] = "_NET_WM_NAME",
    [A_NET_WM_STATE] = "_NET_WM_STATE",
    [A_NET_WM_STATE_FULLSCREEN] = "_NET_WM_STATE_FULLSCREEN",
    [A_NET_WM_STATE_MAXIMIZED_VERT] = "_NET_WM_STATE_MAXIMIZED_VERT",
    [A_NET_WM_STATE_MAXIMIZED_HORZ] = "_NET_WM_STATE_MAXIMIZED_HORZ",
    [A_NET_WM_STATE_MODAL] = "_NET_WM_STATE_MODAL",
    [A_NET_WM_STATE_HIDDEN] = "_NET_WM_STATE_HIDDEN",
    [A_NET_ACTIVE_WINDOW] = "_NET_ACTIVE_WINDOW",
    [A_NET_CLIENT_LIST] = "_NET_CLIENT_LIST",
    [A_NET_CLIENT_LIST_STACKING] = "_NET_CLIENT_LIST_STACKING",
    [A_NET_SUPPORTED] = "_NET_SUPPORTED",
    [A_NET_SUPPORTING_WM_CHECK] = "_NET_SUPPORTING_WM_CHECK",
    [A_NET_WM_WINDOW_TYPE] = "_NET_WM_WINDOW_TYPE",
    [A_NET_WM_WINDOW_TYPE_DIALOG] = "_NET_WM_WINDOW_TYPE_DIALOG",
    [A_NET_CLOSE_WINDOW] = "_NET_CLOSE_WINDOW",
    [A_UTF8_STRING] = "UTF8_STRING",
    [A_TEXT] = "TEXT",
    [A_TARGETS] = "TARGETS",
    [A_TIMESTAMP] = "TIMESTAMP",
    [A_INCR] = "INCR",
    [A_CLIPBOARD] = "CLIPBOARD",
    [A_PRIMARY] = "PRIMARY",
    [A_CLIPBOARD_MANAGER] = "CLIPBOARD_MANAGER",
    [A_WL_SURFACE_SERIAL] = "WL_SURFACE_SERIAL",
    [A_WL_SURFACE_ID] = "WL_SURFACE_ID",
    [A_TOMOE_CONVERT] = "_TOMOE_CONVERT",
};

enum {
    CW_EVENT_MASK = 1 << 11,
    MASK_PROPERTY_CHANGE = 1 << 22,
    MASK_STRUCTURE_NOTIFY = 1 << 17,
    MASK_SUBSTRUCTURE_NOTIFY = 1 << 19,
    MASK_SUBSTRUCTURE_REDIRECT = 1 << 20,
    MASK_FOCUS_CHANGE = 1 << 21,
    CFG_X = 1, CFG_Y = 2, CFG_WIDTH = 4, CFG_HEIGHT = 8, CFG_BORDER = 16,
    CFG_SIBLING = 32, CFG_STACK = 64,
};

#define XA_ATOM 4u
#define XA_INTEGER 19u
#define XA_STRING 31u
#define XA_WINDOW 33u
#define X11_NONE 0u
#define X11_CURRENT_TIME 0u
#define INCR_CHUNK (64 * 1024)
#define CONVERT_TIMEOUT_MS 5000
#define XWM_OUT_LIMIT (1u << 20)



struct xwm_window;

struct deferred {
    struct xlist link;
    size_t len;
    unsigned char data[];
};

struct xwm;

struct wl_sel_cache {
    bool present;
    char *mimes[64];
    size_t count;
};

typedef void (*reply_fn)(struct xwm *x, const unsigned char *reply, size_t len, void *ctx);

struct pending {
    struct xlist link;
    uint16_t seq;
    reply_fn fn;
    void *ctx;
    void (*drop)(void *ctx);
};

struct atom_entry {
    uint32_t atom;
    char *mime;
};

enum { CONVERT_TARGETS, CONVERT_DATA };
enum { CS_ATOM, CS_NOTIFY, CS_PROP, CS_INCR_WAIT, CS_INCR_PROP };

struct convert {
    struct xlist link;
    uint64_t id;
    uint32_t window;
    int kind, state;
    bool primary, active;
    uint32_t target, type, offset;
    uint64_t generation;
    char *mime;
    int fd;
    unsigned char *data;
    size_t len, cap, chunk;
};

struct adopt {
    bool primary;
    uint64_t generation;
    char *mimes[64];
    size_t count, outstanding;
};

struct xwm {
    struct xwm_sidecar *sc;
    int fd;
    struct xev_source *source;
    unsigned char *in;
    size_t in_len, in_cap;
    unsigned char *out;
    size_t out_len, out_cap;
    struct xlist pending;
    uint16_t seq;
    uint32_t rid_base, rid_mask, rid_next;
    uint32_t root, max_request;
    uint32_t wm_window, clipboard_window, primary_window, convert_property;
    uint32_t atoms[A_COUNT];
    int xfixes, xfixes_event;
    int setup_state;
    size_t atom_pending;
    uint16_t atom_seq[A_COUNT];
    bool ready;
    struct xlist windows, writers, transfers, props;
    uint32_t active;
    bool own[2];
    struct wl_sel_cache wl_sel[2];
    struct atom_entry *cache;
    size_t cache_count, cache_cap;
    size_t interning[2];
    struct xlist sel_waiting[2];
    struct xlist converts;
    uint64_t sel_generation[2];
    uint64_t convert_ids;
    int convert_timer_fd;
    struct xev_source *convert_timer;
    int setup_queries;
    bool stopping;
    bool failed;
};

enum { SETUP_NONE, SETUP_WAIT, SETUP_ATOMS, SETUP_READY };

struct xwm_window {
    struct xlist link;
    struct xwm *xwm;
    uint32_t xid;
    int x, y, width, height;
    bool override, x_mapped, delete_window, take_focus, input_hint, has_input_hint;
    bool fullscreen, maximized, dialog;
    char *title, *appid;
    uint32_t transient;
    int min_width, min_height;
    bool net_title, close_pending;
};

struct req {
    unsigned char *data;
    size_t len, cap;
};

static void x_fail(struct xwm *x, const char *reason, int error) {
    if (x->failed || x->stopping) return;
    x->failed = true;
    if (x->source) {
        xev_del(x->sc, x->source);
        x->source = NULL;
    }
    fprintf(stderr, "tomoe-xwm: X11 manager failed on %d: %s%s%s\n",
        x->fd, reason, error ? ": " : "", error ? strerror(error) : "");
    sidecar_wm_failed(x->sc);
}

static void x_update_events(struct xwm *x) {
    if (!x->source) return;
    uint32_t ev = EPOLLIN;
    if (x->out_len > 0) ev |= EPOLLOUT;
    xev_mod(x->sc, x->source, ev);
}

static void x_flush(struct xwm *x) {
    if (x->stopping || x->failed || !x->out_len) return;
    size_t sent = 0;
    while (sent < x->out_len) {
        ssize_t n = send(x->fd, x->out + sent, x->out_len - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        x_fail(x, "send failed", n < 0 ? errno : 0);
        return;
    }
    if (sent > 0) {
        x->out_len -= sent;
        if (x->out_len) memmove(x->out, x->out + sent, x->out_len);
    }
    x_update_events(x);
}

static void x_write(struct xwm *x, const void *data, size_t len) {
    if (x->stopping || x->failed || !len) return;
    if (!x->out_len) {
        ssize_t n;
        do n = send(x->fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        while (n < 0 && errno == EINTR);
        if (n > 0) {
            if ((size_t)n == len) return;
            data = (const unsigned char *)data + n;
            len -= (size_t)n;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            x_fail(x, "send failed", errno);
            return;
        }
    }
    if (x->out_len + len > XWM_OUT_LIMIT) {
        x_fail(x, "send queue overflow", 0);
        return;
    }
    if (x->out_len + len > x->out_cap) {
        size_t cap = x->out_cap ? x->out_cap : 4096;
        while (cap < x->out_len + len) cap *= 2;
        unsigned char *grown = realloc(x->out, cap);
        if (!grown) {
            x_fail(x, "out of memory", ENOMEM);
            return;
        }
        x->out = grown;
        x->out_cap = cap;
    }
    memcpy(x->out + x->out_len, data, len);
    x->out_len += len;
    x_update_events(x);
}
static void r_grow(struct req *r, size_t extra) {
    if (r->len + extra <= r->cap) return;
    size_t cap = r->cap ? r->cap : 64;
    while (cap < r->len + extra) cap *= 2;
    unsigned char *data = realloc(r->data, cap);
    if (!data) return;
    r->data = data;
    r->cap = cap;
}

static void r_begin(struct req *r, uint8_t opcode) {
    r->len = 0;
    r_grow(r, 4);
    memset(r->data, 0, 4);
    r->data[0] = opcode;
    r->len = 4;
}

static void r_flag(struct req *r, int index, uint8_t value) {
    r->data[index] = value;
}

static void r_u8(struct req *r, uint8_t value) {
    r_grow(r, 1);
    r->data[r->len++] = value;
}

static void r_u16(struct req *r, uint16_t value) {
    r_grow(r, 2);
    r->data[r->len++] = (unsigned char)value;
    r->data[r->len++] = (unsigned char)(value >> 8);
}

static void r_i16(struct req *r, int value) {
    r_u16(r, (uint16_t)value);
}

static void r_u32(struct req *r, uint32_t value) {
    r_grow(r, 4);
    for (int i = 0; i < 4; i++) r->data[r->len++] = (unsigned char)(value >> (8 * i));
}

static void r_bytes(struct req *r, const void *data, size_t len) {
    r_grow(r, len);
    memcpy(r->data + r->len, data, len);
    r->len += len;
}

static void r_pad(struct req *r) {
    while (r->len & 3) r_u8(r, 0);
}

static uint16_t r_send(struct xwm *x, struct req *r) {
    r_pad(r);
    uint16_t length = (uint16_t)(r->len / 4);
    r->data[2] = (unsigned char)length;
    r->data[3] = (unsigned char)(length >> 8);
    uint16_t seq = ++x->seq;
    x_write(x, r->data, r->len);
    free(r->data);
    r->data = NULL;
    r->len = r->cap = 0;
    return seq;
}

static uint32_t r_rid(struct xwm *x) {
    return x->rid_base | ((++x->rid_next) & x->rid_mask);
}

static bool x_ensure(struct xwm *x, size_t need) {
    if (x->in_len + need <= x->in_cap) return true;
    size_t cap = x->in_cap ? x->in_cap : 4096;
    while (cap < x->in_len + need) cap *= 2;
    unsigned char *data = realloc(x->in, cap);
    if (!data) return false;
    x->in = data;
    x->in_cap = cap;
    return true;
}

static ssize_t x_recv(struct xwm *x) {
    for (;;) {
        if (!x_ensure(x, 4096)) return -1;
        ssize_t n = recv(x->fd, x->in + x->in_len, x->in_cap - x->in_len, MSG_DONTWAIT);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        return -1;
    }
}

static size_t packet_length(struct xwm *x, const unsigned char *p, size_t available) {
    if (available < 8) return 0;
    uint8_t type = p[0] & 0x7f;
    if (type == 0) return 32;
    if (type == 1 && x->setup_state == SETUP_WAIT)
        return 8 + ((size_t)p[6] | (size_t)p[7] << 8) * 4;
    size_t words = (size_t)p[4] | (size_t)p[5] << 8 | (size_t)p[6] << 16 |
        (size_t)p[7] << 24;
    if (type == 1 || type == 35) return 32 + words * 4;
    return 32;
}



static void expect(struct xwm *x, uint16_t seq, reply_fn fn, void *ctx, void (*drop)(void *ctx)) {
    struct pending *p = calloc(1, sizeof(*p));
    if (!p) {
        if (drop) drop(ctx);
        x_fail(x, "out of memory", ENOMEM);
        return;
    }
    p->seq = seq;
    p->fn = fn;
    p->ctx = ctx;
    p->drop = drop;
    xlist_insert(x->pending.prev, &p->link);
}

static bool pending_dispatch(struct xwm *x, const unsigned char *packet, size_t len) {
    uint16_t seq = (uint16_t)(packet[2] | packet[3] << 8);
    struct pending *p, *next;
    xlist_for_each_safe(p, next, &x->pending, link) {
        if (p->seq != seq) continue;
        xlist_remove(&p->link);
        p->fn(x, packet[0] == 1 ? packet : NULL, packet[0] == 1 ? len : 0, p->ctx);
        free(p);
        return true;
    }
    return false;
}


struct property {
    uint32_t type, after;
    int format;
    size_t length;
    unsigned char *data;
};

enum prop_kind {
    PROP_CLASS = 1, PROP_TITLE_NET, PROP_TITLE_WM, PROP_TRANSIENT, PROP_HINTS, PROP_NORMAL,
    PROP_PROTOCOLS, PROP_STATE, PROP_TYPE,
};

struct prop_request {
    struct xlist link;
    struct xwm_window *window;
    uint16_t seq;
    int kind;
};

static void property_from_reply(const unsigned char *reply, size_t reply_len,
        struct property *out) {
    memset(out, 0, sizeof(*out));
    if (reply_len < 32) return;
    out->format = reply[1];
    out->type = (uint32_t)reply[8] | (uint32_t)reply[9] << 8 |
        (uint32_t)reply[10] << 16 | (uint32_t)reply[11] << 24;
    out->after = (uint32_t)reply[12] | (uint32_t)reply[13] << 8 |
        (uint32_t)reply[14] << 16 | (uint32_t)reply[15] << 24;
    uint32_t items = (uint32_t)reply[16] | (uint32_t)reply[17] << 8 |
        (uint32_t)reply[18] << 16 | (uint32_t)reply[19] << 24;
    size_t bytes = out->format == 32 ? (size_t)items * 4 :
        out->format == 16 ? (size_t)items * 2 : items;
    if (bytes > reply_len - 32) bytes = reply_len - 32;
    out->length = bytes;
    out->data = malloc(bytes ? bytes : 1);
    if (out->data && bytes) memcpy(out->data, reply + 32, bytes);
}


static void property_finish(struct property *property) {
    free(property->data);
    property->data = NULL;
}


static void change_property(struct xwm *x, uint8_t mode, uint32_t window, uint32_t property,
        uint32_t type, int format, const void *data, uint32_t items) {
    struct req r = {0};
    r_begin(&r, 18);
    r_flag(&r, 1, mode);
    r_u32(&r, window);
    r_u32(&r, property);
    r_u32(&r, type);
    r_u8(&r, (uint8_t)format);
    r_u8(&r, 0); r_u8(&r, 0); r_u8(&r, 0);
    r_u32(&r, items);
    if (format == 32)
        for (uint32_t i = 0; i < items; i++)
            r_u32(&r, ((const uint32_t *)data)[i]);
    else if (format == 16)
        for (uint32_t i = 0; i < items; i++)
            r_u16(&r, ((const uint16_t *)data)[i]);
    else
        r_bytes(&r, data, items);
    r_send(x, &r);
}

static void delete_property(struct xwm *x, uint32_t window, uint32_t property) {
    struct req r = {0};
    r_begin(&r, 19);
    r_u32(&r, window);
    r_u32(&r, property);
    r_send(x, &r);
}

static void create_window(struct xwm *x, uint32_t window, uint32_t parent, int depth,
        int kind, int width, int height, uint32_t mask, const uint32_t *values) {
    struct req r = {0};
    r_begin(&r, 1);
    r_flag(&r, 1, (uint8_t)depth);
    r_u32(&r, window);
    r_u32(&r, parent);
    r_i16(&r, 0); r_i16(&r, 0);
    r_u16(&r, (uint16_t)width); r_u16(&r, (uint16_t)height);
    r_u16(&r, 0);
    r_u16(&r, (uint16_t)kind);
    r_u32(&r, 0);
    r_u32(&r, mask);
    for (int i = 0; i < 32; i++)
        if (mask & (1u << i)) r_u32(&r, values[i]);
    r_send(x, &r);
}

static void change_attributes(struct xwm *x, uint32_t window, uint32_t mask, uint32_t value) {
    struct req r = {0};
    r_begin(&r, 2);
    r_u32(&r, window);
    r_u32(&r, mask);
    r_u32(&r, value);
    r_send(x, &r);
}

static void set_event_mask(struct xwm *x, uint32_t window, uint32_t mask) {
    change_attributes(x, window, CW_EVENT_MASK, mask);
}

static void configure_window(struct xwm *x, uint32_t window, uint32_t mask,
        const uint32_t *values) {
    struct req r = {0};
    r_begin(&r, 12);
    r_u32(&r, window);
    r_u16(&r, (uint16_t)mask);
    r_u16(&r, 0);
    for (int i = 0; i < 7; i++)
        if (mask & (1u << i)) r_u32(&r, values[i]);
    r_send(x, &r);
}

static void simple_window(struct xwm *x, uint8_t opcode, uint32_t window) {
    struct req r = {0};
    r_begin(&r, opcode);
    r_u32(&r, window);
    r_send(x, &r);
}

static void set_owner(struct xwm *x, uint32_t window, uint32_t selection, uint32_t time) {
    struct req r = {0};
    r_begin(&r, 22);
    r_u32(&r, window);
    r_u32(&r, selection);
    r_u32(&r, time);
    r_send(x, &r);
}

static void convert_selection(struct xwm *x, uint32_t requestor, uint32_t selection,
        uint32_t target, uint32_t property, uint32_t time) {
    struct req r = {0};
    r_begin(&r, 24);
    r_u32(&r, requestor);
    r_u32(&r, selection);
    r_u32(&r, target);
    r_u32(&r, property);
    r_u32(&r, time);
    r_send(x, &r);
}

static void send_event(struct xwm *x, uint32_t destination, uint32_t mask,
        const unsigned char event[32]) {
    struct req r = {0};
    r_begin(&r, 25);
    r_u32(&r, destination);
    r_u32(&r, mask);
    r_bytes(&r, event, 32);
    r_send(x, &r);
}

static void client_message(struct xwm *x, uint32_t window, uint32_t type, uint32_t mask,
        const uint32_t data[5]) {
    unsigned char event[32] = {0};
    event[0] = 33;
    event[1] = 32;
    event[4] = (unsigned char)window;
    event[5] = (unsigned char)(window >> 8);
    event[6] = (unsigned char)(window >> 16);
    event[7] = (unsigned char)(window >> 24);
    event[8] = (unsigned char)type;
    event[9] = (unsigned char)(type >> 8);
    event[10] = (unsigned char)(type >> 16);
    event[11] = (unsigned char)(type >> 24);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++)
            event[12 + i * 4 + j] = (unsigned char)(data[i] >> (8 * j));
    send_event(x, window, mask, event);
}

static void set_focus(struct xwm *x, uint32_t window) {
    struct req r = {0};
    r_begin(&r, 42);
    r_flag(&r, 1, 2);
    r_u32(&r, window);
    r_u32(&r, X11_CURRENT_TIME);
    r_send(x, &r);
}

static void kill_client(struct xwm *x, uint32_t resource) {
    struct req r = {0};
    r_begin(&r, 113);
    r_u32(&r, resource);
    r_send(x, &r);
}


static void extension_request(struct xwm *x, int opcode, int minor, const uint32_t *values,
        size_t count) {
    struct req r = {0};
    r_begin(&r, (uint8_t)opcode);
    r_flag(&r, 1, (uint8_t)minor);
    for (size_t i = 0; i < count; i++) r_u32(&r, values[i]);
    r_send(x, &r);
}

static void xfixes_request(struct xwm *x, int minor, const uint32_t *values, size_t count) {
    struct req r = {0};
    r_begin(&r, (uint8_t)x->xfixes);
    r_flag(&r, 1, (uint8_t)minor);
    for (size_t i = 0; i < count; i++) r_u32(&r, values[i]);
    r_send(x, &r);
}

static uint32_t lookup_atom(struct xwm *x, int index) {
    return index >= 0 && index < A_COUNT ? x->atoms[index] : 0;
}

static bool atom_list_has(const struct property *property, uint32_t atom) {
    if (property->format != 32 || !property->data) return false;
    size_t count = property->length / 4;
    const uint32_t *values = (const uint32_t *)property->data;
    for (size_t i = 0; i < count; i++)
        if (values[i] == atom) return true;
    return false;
}

static bool utf8_valid(const unsigned char *text, size_t length) {
    size_t i = 0;
    while (i < length) {
        unsigned char byte = text[i];
        int extra = byte < 0x80 ? 0 : (byte & 0xe0) == 0xc0 ? 1 : (byte & 0xf0) == 0xe0 ? 2 :
            (byte & 0xf8) == 0xf0 ? 3 : -1;
        if (extra < 0 || i + (size_t)extra >= length) return false;
        i++;
        for (int j = 0; j < extra; j++, i++)
            if ((text[i] & 0xc0) != 0x80) return false;
    }
    return true;
}

static char *property_text(const struct property *property) {
    if (!property->data || !property->length) return strdup("");
    size_t length = property->length;
    while (length && property->data[length - 1] == 0) length--;
    char *text = malloc(length + 1);
    if (!text) return NULL;
    memcpy(text, property->data, length);
    text[length] = 0;
    return text;
}




static struct xwm_window *window_for(struct xwm *x, uint32_t xid) {
    struct xwm_window *w;
    xlist_for_each(w, &x->windows, link)
        if (w->xid == xid) return w;
    return NULL;
}

static char *replace_string(char **slot, const char *value) {
    if (!value) value = "";
    if (*slot && strcmp(*slot, value) == 0) return NULL;
    char *copy = strdup(value);
    if (!copy) return NULL;
    free(*slot);
    *slot = copy;
    return copy;
}

static void send_meta(struct xwm_window *w) {
    size_t tlen = w->title ? strlen(w->title) : 0;
    size_t alen = w->appid ? strlen(w->appid) : 0;
    if (tlen > 4096) tlen = 4096;
    if (alen > 4096) alen = 4096;
    size_t total = sizeof(struct xwm_msg_window_meta) + tlen + alen;
    uint8_t *buf = malloc(total);
    if (!buf) return;
    struct xwm_msg_window_meta msg = {
        .xid = w->xid,
        .transient = w->transient,
        .title_len = (uint32_t)tlen,
        .appid_len = (uint32_t)alen,
        .dialog = w->dialog ? 1 : 0,
        .fullscreen = w->fullscreen ? 1 : 0,
        .maximized = w->maximized ? 1 : 0,
    };
    memcpy(buf, &msg, sizeof(msg));
    if (tlen) memcpy(buf + sizeof(msg), w->title, tlen);
    if (alen) memcpy(buf + sizeof(msg) + tlen, w->appid, alen);
    sidecar_send(w->xwm->sc, XWM_EVT_WINDOW_META, buf, (uint32_t)total, -1);
    free(buf);
}

static uint16_t send_get_property(struct xwm *x, uint32_t window, uint32_t property) {
    struct req r = {0};
    r_begin(&r, 20);
    r_u32(&r, window);
    r_u32(&r, property);
    r_u32(&r, 0);
    r_u32(&r, 0);
    r_u32(&r, x->max_request / 4);
    return r_send(x, &r);
}

static void request_prop(struct xwm_window *w, int kind, uint32_t atom) {
    struct prop_request *request = calloc(1, sizeof(*request));
    if (!request) return;
    request->window = w;
    request->kind = kind;
    request->seq = send_get_property(w->xwm, w->xid, atom);
    xlist_insert(&w->xwm->props, &request->link);
}

static void request_window_props(struct xwm_window *w) {
    struct xwm *x = w->xwm;
    request_prop(w, PROP_CLASS, lookup_atom(x, A_WM_CLASS));
    request_prop(w, PROP_TITLE_NET, lookup_atom(x, A_NET_WM_NAME));
    request_prop(w, PROP_TITLE_WM, lookup_atom(x, A_WM_NAME));
    request_prop(w, PROP_TRANSIENT, lookup_atom(x, A_WM_TRANSIENT_FOR));
    request_prop(w, PROP_HINTS, lookup_atom(x, A_WM_HINTS));
    request_prop(w, PROP_NORMAL, lookup_atom(x, A_WM_NORMAL_HINTS));
    request_prop(w, PROP_PROTOCOLS, lookup_atom(x, A_WM_PROTOCOLS));
    request_prop(w, PROP_STATE, lookup_atom(x, A_NET_WM_STATE));
    request_prop(w, PROP_TYPE, lookup_atom(x, A_NET_WM_WINDOW_TYPE));
}

static void apply_class(struct xwm_window *w, const struct property *property) {
    char *appid = NULL;
    if (property->format == 8 && property->data && property->length) {
        const char *data = (const char *)property->data;
        size_t first = strnlen(data, property->length);
        if (first < property->length) {
            const char *second = data + first + 1;
            size_t second_len = strnlen(second, property->length - first - 1);
            if (second_len) appid = strndup(second, second_len);
        }
        if (!appid) appid = strndup(data, first);
    }
    if (appid && utf8_valid((const unsigned char *)appid, strlen(appid))) {
        replace_string(&w->appid, appid);
        send_meta(w);
    }
    free(appid);
}

static void apply_title(struct xwm_window *w, const struct property *property, bool net) {
    if (property->type == X11_NONE || property->format != 8 || !property->length) {
        if (net) {
            w->net_title = false;
            request_prop(w, PROP_TITLE_WM, lookup_atom(w->xwm, A_WM_NAME));
        }
        return;
    }
    char *title = property_text(property);
    if (!title) return;
    if (net) w->net_title = true;
    if (net || !w->net_title) {
        if (utf8_valid((const unsigned char *)title, strlen(title))) {
            replace_string(&w->title, title);
            send_meta(w);
        }
    }
    free(title);
}

static void apply_prop(struct prop_request *request, const struct property *property) {
    struct xwm_window *w = request->window;
    struct xwm *x = w->xwm;
    switch (request->kind) {
    case PROP_CLASS:
        apply_class(w, property);
        break;
    case PROP_TITLE_NET:
        apply_title(w, property, true);
        break;
    case PROP_TITLE_WM:
        apply_title(w, property, false);
        break;
    case PROP_TRANSIENT:
        w->transient = property->format == 32 && property->length >= 4 ?
            ((const uint32_t *)property->data)[0] : 0;
        w->dialog = w->transient != 0;
        send_meta(w);
        break;
    case PROP_HINTS:
        if (property->format == 32 && property->length >= 8) {
            const uint32_t *values = (const uint32_t *)property->data;
            if (values[0] & 1) {
                w->has_input_hint = true;
                w->input_hint = values[1] != 0;
            }
        }
        break;
    case PROP_NORMAL:
        if (property->format == 32 && property->length >= 18 * 4) {
            const uint32_t *values = (const uint32_t *)property->data;
            if (values[0] & (1u << 4)) {
                w->min_width = (int)values[5];
                w->min_height = (int)values[6];
            }
        }
        break;
    case PROP_PROTOCOLS:
        w->delete_window = atom_list_has(property, lookup_atom(x, A_WM_DELETE_WINDOW));
        w->take_focus = atom_list_has(property, lookup_atom(x, A_WM_TAKE_FOCUS));
        if (w->close_pending) {
            w->close_pending = false;
            if (w->delete_window) {
                uint32_t data[5] = { lookup_atom(x, A_WM_DELETE_WINDOW), X11_CURRENT_TIME, 0, 0, 0 };
                client_message(x, w->xid, lookup_atom(x, A_WM_PROTOCOLS), 0, data);
            } else {
                kill_client(x, w->xid);
            }
        }
        break;
    case PROP_STATE:
        w->fullscreen = atom_list_has(property, lookup_atom(x, A_NET_WM_STATE_FULLSCREEN));
        w->maximized = atom_list_has(property, lookup_atom(x, A_NET_WM_STATE_MAXIMIZED_VERT)) ||
            atom_list_has(property, lookup_atom(x, A_NET_WM_STATE_MAXIMIZED_HORZ));
        send_meta(w);
        break;
    case PROP_TYPE:
        if (atom_list_has(property, lookup_atom(x, A_NET_WM_WINDOW_TYPE_DIALOG))) {
            w->dialog = true;
            send_meta(w);
        }
        break;
    }
}

static void handle_property_reply(struct xwm *x, unsigned char *packet, size_t length) {
    uint16_t seq = (uint16_t)(packet[2] | packet[3] << 8);
    struct prop_request *request;
    xlist_for_each(request, &x->props, link) {
        if (request->seq != seq) continue;
        struct property property;
        property_from_reply(packet, length, &property);
        apply_prop(request, &property);
        property_finish(&property);
        xlist_remove(&request->link);
        free(request);
        return;
    }
}

static void set_wm_state(struct xwm_window *w, uint32_t state) {
    uint32_t values[2] = { state, X11_NONE };
    change_property(w->xwm, 0, w->xid, lookup_atom(w->xwm, A_WM_STATE),
        lookup_atom(w->xwm, A_WM_STATE), 32, values, 2);
}

static void refresh_client_list(struct xwm *x) {
    uint32_t list[256];
    uint32_t count = 0;
    struct xwm_window *w;
    xlist_for_each(w, &x->windows, link)
        if (!w->override && w->x_mapped && count < 256) list[count++] = w->xid;
    change_property(x, 0, x->root, lookup_atom(x, A_NET_CLIENT_LIST), XA_WINDOW, 32, list, count);
    change_property(x, 0, x->root, lookup_atom(x, A_NET_CLIENT_LIST_STACKING), XA_WINDOW, 32,
        list, count);
}

static void refresh_active(struct xwm *x) {
    change_property(x, 0, x->root, lookup_atom(x, A_NET_ACTIVE_WINDOW), XA_WINDOW, 32,
        &x->active, 1);
}

static void window_free(struct xwm_window *w) {
    struct xwm *x = w->xwm;
    struct prop_request *request, *next_request;
    xlist_for_each_safe(request, next_request, &x->props, link) {
        if (request->window != w) continue;
        xlist_remove(&request->link);
        free(request);
    }
    uint32_t xid = w->xid;
    free(w->title);
    free(w->appid);
    xlist_remove(&w->link);
    free(w);
    if (x->active == xid) {
        x->active = 0;
        if (!x->stopping) refresh_active(x);
    }
}

static struct xwm_window *window_new(struct xwm *x, uint32_t xid, bool override,
        int wx, int wy, int width, int height) {
    struct xwm_window *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->xwm = x;
    w->xid = xid;
    w->override = override;
    w->x = wx;
    w->y = wy;
    w->width = width;
    w->height = height;
    xlist_insert(&x->windows, &w->link);
    return w;
}

static void handle_create(struct xwm *x, const unsigned char *pkt) {
    uint32_t xid = (uint32_t)pkt[8] | (uint32_t)pkt[9] << 8 |
        (uint32_t)pkt[10] << 16 | (uint32_t)pkt[11] << 24;
    if (window_for(x, xid)) return;
    if (x->rid_mask && (xid & ~x->rid_mask) == x->rid_base) return;
    int wx = (int16_t)(pkt[12] | pkt[13] << 8);
    int wy = (int16_t)(pkt[14] | pkt[15] << 8);
    int width = (uint16_t)(pkt[16] | pkt[17] << 8);
    int height = (uint16_t)(pkt[18] | pkt[19] << 8);
    bool override = pkt[22] != 0;
    struct xwm_window *w = window_new(x, xid, override, wx, wy, width, height);
    if (!w) return;
    struct xwm_msg_window_create msg = {
        .xid = xid, .x = wx, .y = wy, .width = width, .height = height,
        .override = override ? 1 : 0,
    };
    sidecar_send(x->sc, XWM_EVT_WINDOW_CREATE, &msg, sizeof(msg), -1);
    set_event_mask(x, xid, MASK_PROPERTY_CHANGE | MASK_STRUCTURE_NOTIFY | MASK_FOCUS_CHANGE);
    request_window_props(w);
}

static void handle_destroy(struct xwm *x, const unsigned char *pkt) {
    uint32_t xid = (uint32_t)pkt[8] | (uint32_t)pkt[9] << 8 |
        (uint32_t)pkt[10] << 16 | (uint32_t)pkt[11] << 24;
    struct xwm_window *w = window_for(x, xid);
    if (!w) return;
    window_free(w);
    refresh_client_list(x);
    struct xwm_msg_window_id msg = { .xid = xid };
    sidecar_send(x->sc, XWM_EVT_WINDOW_DESTROY, &msg, sizeof(msg), -1);
}

static void handle_map_request(struct xwm *x, const unsigned char *pkt) {
    uint32_t xid = (uint32_t)pkt[8] | (uint32_t)pkt[9] << 8 |
        (uint32_t)pkt[10] << 16 | (uint32_t)pkt[11] << 24;
    struct xwm_window *w = window_for(x, xid);
    if (!w) return;
    if (!w->override && (w->width < 2 || w->height < 2)) {
        int width = w->min_width > 0 ? w->min_width : 800;
        int height = w->min_height > 0 ? w->min_height : 800;
        uint32_t values[7] = {0};
        values[2] = (uint32_t)width;
        values[3] = (uint32_t)height;
        configure_window(x, xid, CFG_WIDTH | CFG_HEIGHT, values);
        w->width = width;
        w->height = height;
    }
    simple_window(x, 8, xid);
}

static void handle_map_notify(struct xwm *x, const unsigned char *pkt) {
    uint32_t xid = (uint32_t)pkt[8] | (uint32_t)pkt[9] << 8 |
        (uint32_t)pkt[10] << 16 | (uint32_t)pkt[11] << 24;
    struct xwm_window *w = window_for(x, xid);
    if (!w) return;
    w->x_mapped = true;
    request_window_props(w);
    if (!w->override) {
        set_wm_state(w, 1);
        refresh_client_list(x);
    }
    struct xwm_msg_window_map msg = {
        .xid = xid, .x = w->x, .y = w->y, .width = w->width, .height = w->height,
        .override = w->override ? 1 : 0, .dialog = w->dialog ? 1 : 0,
        .fullscreen = w->fullscreen ? 1 : 0, .maximized = w->maximized ? 1 : 0,
    };
    sidecar_send(x->sc, XWM_EVT_WINDOW_MAP, &msg, sizeof(msg), -1);
}

static void handle_unmap_notify(struct xwm *x, const unsigned char *pkt) {
    uint32_t xid = (uint32_t)pkt[8] | (uint32_t)pkt[9] << 8 |
        (uint32_t)pkt[10] << 16 | (uint32_t)pkt[11] << 24;
    struct xwm_window *w = window_for(x, xid);
    if (!w) return;
    w->x_mapped = false;
    if (!w->override) {
        set_wm_state(w, 0);
        refresh_client_list(x);
    }
    struct xwm_msg_window_id msg = { .xid = xid };
    sidecar_send(x->sc, XWM_EVT_WINDOW_UNMAP, &msg, sizeof(msg), -1);
}

static void handle_configure_notify(struct xwm *x, const unsigned char *pkt) {
    uint32_t xid = (uint32_t)pkt[8] | (uint32_t)pkt[9] << 8 |
        (uint32_t)pkt[10] << 16 | (uint32_t)pkt[11] << 24;
    struct xwm_window *w = window_for(x, xid);
    if (!w) return;
    w->x = (int16_t)(pkt[16] | pkt[17] << 8);
    w->y = (int16_t)(pkt[18] | pkt[19] << 8);
    w->width = (uint16_t)(pkt[20] | pkt[21] << 8);
    w->height = (uint16_t)(pkt[22] | pkt[23] << 8);
    struct xwm_msg_window_geom msg = {
        .xid = xid, .x = w->x, .y = w->y, .width = w->width, .height = w->height,
    };
    sidecar_send(x->sc, XWM_EVT_WINDOW_GEOMETRY, &msg, sizeof(msg), -1);
}

static void handle_configure_request(struct xwm *x, const unsigned char *pkt) {
    uint32_t xid = (uint32_t)pkt[8] | (uint32_t)pkt[9] << 8 |
        (uint32_t)pkt[10] << 16 | (uint32_t)pkt[11] << 24;
    struct xwm_window *w = window_for(x, xid);
    if (!w) return;
    uint16_t mask = (uint16_t)(pkt[26] | pkt[27] << 8);
    uint32_t values[7] = {0};
    values[0] = (uint32_t)(int16_t)(pkt[16] | pkt[17] << 8);
    values[1] = (uint32_t)(int16_t)(pkt[18] | pkt[19] << 8);
    values[2] = (uint16_t)(pkt[20] | pkt[21] << 8);
    values[3] = (uint16_t)(pkt[22] | pkt[23] << 8);
    values[4] = (uint16_t)(pkt[24] | pkt[25] << 8);
    values[5] = (uint32_t)pkt[12] | (uint32_t)pkt[13] << 8 |
        (uint32_t)pkt[14] << 16 | (uint32_t)pkt[15] << 24;
    values[6] = pkt[1];
    if (!w->override && w->x_mapped) {
        unsigned char event[32] = {0};
        event[0] = 22;
        for (int i = 0; i < 4; i++) {
            event[4 + i] = (unsigned char)(xid >> (8 * i));
            event[8 + i] = (unsigned char)(xid >> (8 * i));
        }
        event[16] = (unsigned char)(uint16_t)w->x;
        event[17] = (unsigned char)((uint16_t)w->x >> 8);
        event[18] = (unsigned char)(uint16_t)w->y;
        event[19] = (unsigned char)((uint16_t)w->y >> 8);
        event[20] = (unsigned char)(uint16_t)w->width;
        event[21] = (unsigned char)((uint16_t)w->width >> 8);
        event[22] = (unsigned char)(uint16_t)w->height;
        event[23] = (unsigned char)((uint16_t)w->height >> 8);
        send_event(x, xid, MASK_STRUCTURE_NOTIFY, event);
        return;
    }
    uint32_t apply = mask & (CFG_X | CFG_Y | CFG_WIDTH | CFG_HEIGHT | CFG_BORDER | CFG_SIBLING);
    if (apply) configure_window(x, xid, apply, values);
}

static bool transfer_incr(struct xwm *x, uint32_t window, uint32_t property, uint8_t state);
static bool convert_property_notify(struct xwm *x, uint32_t window, uint32_t atom, uint8_t state);

static void handle_property_notify(struct xwm *x, const unsigned char *pkt) {
    uint32_t xid = (uint32_t)pkt[4] | (uint32_t)pkt[5] << 8 |
        (uint32_t)pkt[6] << 16 | (uint32_t)pkt[7] << 24;
    uint32_t atom = (uint32_t)pkt[8] | (uint32_t)pkt[9] << 8 |
        (uint32_t)pkt[10] << 16 | (uint32_t)pkt[11] << 24;
    if (convert_property_notify(x, xid, atom, pkt[16])) return;
    if (transfer_incr(x, xid, atom, pkt[16])) return;
    struct xwm_window *w = window_for(x, xid);
    if (!w) return;
    if (atom == lookup_atom(x, A_WM_CLASS)) request_prop(w, PROP_CLASS, atom);
    else if (atom == lookup_atom(x, A_NET_WM_NAME)) request_prop(w, PROP_TITLE_NET, atom);
    else if (atom == lookup_atom(x, A_WM_NAME)) request_prop(w, PROP_TITLE_WM, atom);
    else if (atom == lookup_atom(x, A_WM_TRANSIENT_FOR)) request_prop(w, PROP_TRANSIENT, atom);
    else if (atom == lookup_atom(x, A_WM_HINTS)) request_prop(w, PROP_HINTS, atom);
    else if (atom == lookup_atom(x, A_WM_NORMAL_HINTS)) request_prop(w, PROP_NORMAL, atom);
    else if (atom == lookup_atom(x, A_WM_PROTOCOLS)) request_prop(w, PROP_PROTOCOLS, atom);
    else if (atom == lookup_atom(x, A_NET_WM_STATE)) request_prop(w, PROP_STATE, atom);
    else if (atom == lookup_atom(x, A_NET_WM_WINDOW_TYPE)) request_prop(w, PROP_TYPE, atom);
}

static void request_state(struct xwm_window *w, uint32_t action, uint32_t atom) {
    uint8_t kind = 0;
    if (atom == lookup_atom(w->xwm, A_NET_WM_STATE_FULLSCREEN)) kind = XWM_STATE_REQ_FULLSCREEN;
    else if (atom == lookup_atom(w->xwm, A_NET_WM_STATE_MAXIMIZED_VERT) ||
            atom == lookup_atom(w->xwm, A_NET_WM_STATE_MAXIMIZED_HORZ)) kind = XWM_STATE_REQ_MAXIMIZED;
    else return;
    struct xwm_msg_state_req msg = { .xid = w->xid, .kind = kind, .action = (uint8_t)action };
    sidecar_send(w->xwm->sc, XWM_EVT_WINDOW_STATE_REQ, &msg, sizeof(msg), -1);
}

static void handle_client_message(struct xwm *x, const unsigned char *pkt) {
    uint32_t xid = (uint32_t)pkt[4] | (uint32_t)pkt[5] << 8 |
        (uint32_t)pkt[6] << 16 | (uint32_t)pkt[7] << 24;
    uint32_t type = (uint32_t)pkt[8] | (uint32_t)pkt[9] << 8 |
        (uint32_t)pkt[10] << 16 | (uint32_t)pkt[11] << 24;
    uint32_t data[5];
    for (int i = 0; i < 5; i++)
        data[i] = (uint32_t)pkt[12 + i * 4] | (uint32_t)pkt[13 + i * 4] << 8 |
            (uint32_t)pkt[14 + i * 4] << 16 | (uint32_t)pkt[15 + i * 4] << 24;
    struct xwm_window *w = window_for(x, xid);
    if (!w) return;
    if (type == lookup_atom(x, A_WL_SURFACE_SERIAL)) {
        struct xwm_msg_window_pair msg = {
            .xid = xid,
            .serial = ((uint64_t)data[1] << 32) | data[0],
        };
        sidecar_send(x->sc, XWM_EVT_WINDOW_PAIR, &msg, sizeof(msg), -1);
    } else if (type == lookup_atom(x, A_WL_SURFACE_ID)) {
        return;
    } else if (type == lookup_atom(x, A_NET_WM_STATE)) {
        request_state(w, data[0], data[1]);
        request_state(w, data[0], data[2]);
    } else if (type == lookup_atom(x, A_NET_ACTIVE_WINDOW)) {
        struct xwm_msg_state_req msg = { .xid = xid, .kind = XWM_STATE_REQ_ACTIVATE };
        sidecar_send(x->sc, XWM_EVT_WINDOW_STATE_REQ, &msg, sizeof(msg), -1);
    } else if (type == lookup_atom(x, A_NET_CLOSE_WINDOW)) {
        struct xwm_msg_state_req msg = { .xid = xid, .kind = XWM_STATE_REQ_CLOSE };
        sidecar_send(x->sc, XWM_EVT_WINDOW_STATE_REQ, &msg, sizeof(msg), -1);
    } else if (type == lookup_atom(x, A_WM_CHANGE_STATE)) {
        if (data[0] == 3) {
            struct xwm_msg_state_req msg = { .xid = xid, .kind = XWM_STATE_REQ_MINIMIZE };
            sidecar_send(x->sc, XWM_EVT_WINDOW_STATE_REQ, &msg, sizeof(msg), -1);
        }
    }
}

static void state_atom(struct xwm_window *w, uint32_t atom, bool enabled) {
    uint32_t data[5] = { enabled ? 1u : 0u, atom, 0, 2, 0 };
    client_message(w->xwm, w->xid, lookup_atom(w->xwm, A_NET_WM_STATE),
        MASK_SUBSTRUCTURE_REDIRECT | MASK_SUBSTRUCTURE_NOTIFY, data);
}

static void selection_notify(struct xwm *x, uint32_t requestor, uint32_t selection,
        uint32_t target, uint32_t property, uint32_t time) {
    unsigned char event[32] = {0};
    event[0] = 31;
    event[4] = (unsigned char)time;
    event[5] = (unsigned char)(time >> 8);
    event[6] = (unsigned char)(time >> 16);
    event[7] = (unsigned char)(time >> 24);
    uint32_t fields[4] = { requestor, selection, target, property };
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            event[8 + i * 4 + j] = (unsigned char)(fields[i] >> (8 * j));
    send_event(x, requestor, 0, event);
}

struct writer {
    struct xlist link;
    struct xwm *x;
    int fd;
    struct xev_source *source;
    unsigned char *data;
    size_t len, offset;
};

static void writer_free(struct writer *writer) {
    if (writer->source) xev_del(writer->x->sc, writer->source);
    if (writer->fd >= 0) close(writer->fd);
    xlist_remove(&writer->link);
    free(writer->data);
    free(writer);
}

static void writer_ready(int fd, uint32_t events, void *data) {
    struct writer *writer = data;
    (void)fd;
    (void)events;
    while (writer->offset < writer->len) {
        ssize_t n = write(writer->fd, writer->data + writer->offset, writer->len - writer->offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return;
        if (n <= 0) break;
        writer->offset += (size_t)n;
    }
    writer_free(writer);
}

static void writers_free_all(struct xwm *x) {
    struct writer *writer, *next;
    xlist_for_each_safe(writer, next, &x->writers, link) writer_free(writer);
}

static void write_async(struct xwm *x, int fd, unsigned char *data, size_t len) {
    fcntl(fd, F_SETFL, O_NONBLOCK);
    if (!len) {
        close(fd);
        free(data);
        return;
    }
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = write(fd, data + offset, len - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) break;
        if (n <= 0) {
            close(fd);
            free(data);
            return;
        }
        offset += (size_t)n;
    }
    if (offset == len) {
        close(fd);
        free(data);
        return;
    }
    struct writer *writer = calloc(1, sizeof(*writer));
    if (!writer) {
        close(fd);
        free(data);
        return;
    }
    writer->x = x;
    writer->fd = fd;
    writer->data = data;
    writer->len = len;
    writer->offset = offset;
    writer->source = xev_add(x->sc, fd, EPOLLOUT, writer_ready, writer);
    if (!writer->source) {
        close(fd);
        free(data);
        free(writer);
        return;
    }
    xlist_insert(&x->writers, &writer->link);
}



static uint32_t cache_atom(struct xwm *x, const char *mime) {
    if (strcmp(mime, "text/plain;charset=utf-8") == 0) return lookup_atom(x, A_UTF8_STRING);
    if (strcmp(mime, "text/plain") == 0) return lookup_atom(x, A_TEXT);
    for (size_t i = 0; i < x->cache_count; i++)
        if (strcmp(x->cache[i].mime, mime) == 0) return x->cache[i].atom;
    return 0;
}

static const char *cache_mime(struct xwm *x, uint32_t atom) {
    if (atom == lookup_atom(x, A_UTF8_STRING)) return "text/plain;charset=utf-8";
    if (atom == lookup_atom(x, A_TEXT) || atom == XA_STRING) return "text/plain";
    for (size_t i = 0; i < x->cache_count; i++)
        if (x->cache[i].atom == atom) return x->cache[i].mime;
    return NULL;
}

static void cache_add(struct xwm *x, uint32_t atom, const char *mime) {
    if (!atom || cache_atom(x, mime)) return;
    if (x->cache_count == x->cache_cap) {
        size_t cap = x->cache_cap ? x->cache_cap * 2 : 32;
        struct atom_entry *grown = realloc(x->cache, cap * sizeof(*grown));
        if (!grown) return;
        x->cache = grown;
        x->cache_cap = cap;
    }
    char *copy = strdup(mime);
    if (!copy) return;
    x->cache[x->cache_count++] = (struct atom_entry){ atom, copy };
}

static uint16_t send_intern(struct xwm *x, const char *name) {
    struct req r = {0};
    size_t len = strlen(name);
    r_begin(&r, 16);
    r_flag(&r, 1, 0);
    r_u16(&r, (uint16_t)len);
    r_u16(&r, 0);
    r_bytes(&r, name, len);
    return r_send(x, &r);
}

static uint32_t reply_u32(const unsigned char *reply, size_t at) {
    return (uint32_t)reply[at] | (uint32_t)reply[at + 1] << 8 | (uint32_t)reply[at + 2] << 16 |
        (uint32_t)reply[at + 3] << 24;
}

static void serve_targets(struct xwm *x, bool primary, const unsigned char *pkt) {
    uint32_t requestor = (uint32_t)pkt[12] | (uint32_t)pkt[13] << 8 |
        (uint32_t)pkt[14] << 16 | (uint32_t)pkt[15] << 24;
    uint32_t selection = (uint32_t)pkt[16] | (uint32_t)pkt[17] << 8 |
        (uint32_t)pkt[18] << 16 | (uint32_t)pkt[19] << 24;
    uint32_t property = (uint32_t)pkt[24] | (uint32_t)pkt[25] << 8 |
        (uint32_t)pkt[26] << 16 | (uint32_t)pkt[27] << 24;
    uint32_t time = (uint32_t)pkt[4] | (uint32_t)pkt[5] << 8 |
        (uint32_t)pkt[6] << 16 | (uint32_t)pkt[7] << 24;
    if (!property) property = lookup_atom(x, A_TARGETS);
    struct wl_sel_cache *cache = &x->wl_sel[primary ? 1 : 0];
    if (!cache->present) {
        selection_notify(x, requestor, selection, lookup_atom(x, A_TARGETS), X11_NONE, time);
        return;
    }
    uint32_t atoms[64];
    size_t used = 0;
    atoms[used++] = lookup_atom(x, A_TARGETS);
    atoms[used++] = lookup_atom(x, A_TIMESTAMP);
    for (size_t i = 0; i < cache->count && used < 64; i++) {
        uint32_t atom = cache_atom(x, cache->mimes[i]);
        if (!atom) continue;
        bool duplicate = false;
        for (size_t j = 0; j < used; j++) if (atoms[j] == atom) duplicate = true;
        if (!duplicate) atoms[used++] = atom;
        if (strcmp(cache->mimes[i], "text/plain") == 0) {
            bool has_string = false;
            for (size_t j = 0; j < used; j++) if (atoms[j] == XA_STRING) has_string = true;
            if (!has_string && used < 64) atoms[used++] = XA_STRING;
        }
    }
    change_property(x, 0, requestor, property, XA_ATOM, 32, atoms, (uint32_t)used);
    selection_notify(x, requestor, selection, lookup_atom(x, A_TARGETS), property, time);
}

struct transfer {
    struct xlist link;
    struct xwm *x;
    bool primary;
    int fd;
    struct xev_source *source;
    unsigned char *data;
    size_t len, cap;
    uint32_t requestor, property, target, time;
    bool incr;
    size_t offset;
};

static void transfer_free(struct transfer *transfer) {
    if (transfer->source) xev_del(transfer->x->sc, transfer->source);
    if (transfer->fd >= 0) close(transfer->fd);
    xlist_remove(&transfer->link);
    free(transfer->data);
    free(transfer);
}

static void transfers_free_all(struct xwm *x) {
    struct transfer *transfer, *next;
    xlist_for_each_safe(transfer, next, &x->transfers, link) transfer_free(transfer);
}

static void transfer_read(int fd, uint32_t events, void *data) {
    struct transfer *transfer = data;
    (void)fd;
    (void)events;
    for (;;) {
        if (transfer->cap - transfer->len < 4096) {
            size_t cap = transfer->cap ? transfer->cap * 2 : 65536;
            unsigned char *grown = realloc(transfer->data, cap);
            if (!grown) {
                transfer_free(transfer);
                return;
            }
            transfer->data = grown;
            transfer->cap = cap;
        }
        ssize_t n = read(transfer->fd, transfer->data + transfer->len,
            transfer->cap - transfer->len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return;
        if (n < 0) {
            selection_notify(transfer->x, transfer->requestor, transfer->primary ?
                lookup_atom(transfer->x, A_PRIMARY) : lookup_atom(transfer->x, A_CLIPBOARD),
                transfer->target, X11_NONE, transfer->time);
            transfer_free(transfer);
            return;
        }
        if (n == 0) break;
        transfer->len += (size_t)n;
    }
    xev_del(transfer->x->sc, transfer->source);
    transfer->source = NULL;
    close(transfer->fd);
    transfer->fd = -1;
    uint32_t selection = transfer->primary ?
        lookup_atom(transfer->x, A_PRIMARY) : lookup_atom(transfer->x, A_CLIPBOARD);
    if (transfer->len > INCR_CHUNK) {
        uint32_t size = (uint32_t)transfer->len;
        set_event_mask(transfer->x, transfer->requestor, MASK_PROPERTY_CHANGE);
        change_property(transfer->x, 0, transfer->requestor, transfer->property,
            lookup_atom(transfer->x, A_INCR), 32, &size, 1);
        selection_notify(transfer->x, transfer->requestor, selection, transfer->target,
            transfer->property, transfer->time);
        transfer->incr = true;
        return;
    }
    change_property(transfer->x, 0, transfer->requestor, transfer->property,
        transfer->target, 8, transfer->data, (uint32_t)transfer->len);
    selection_notify(transfer->x, transfer->requestor, selection, transfer->target,
        transfer->property, transfer->time);
    transfer_free(transfer);
}

static bool transfer_incr(struct xwm *x, uint32_t window, uint32_t property, uint8_t state) {
    if (state != 1) return false;
    struct transfer *transfer, *next;
    xlist_for_each_safe(transfer, next, &x->transfers, link) {
        if (!transfer->incr || transfer->requestor != window || transfer->property != property)
            continue;
        size_t remaining = transfer->len - transfer->offset;
        size_t chunk = remaining > INCR_CHUNK ? INCR_CHUNK : remaining;
        change_property(x, 0, transfer->requestor, transfer->property, transfer->target, 8,
            transfer->data + transfer->offset, (uint32_t)chunk);
        transfer->offset += chunk;
        if (!chunk) transfer_free(transfer);
        return true;
    }
    return false;
}

static void handle_selection_request(struct xwm *x, const unsigned char *pkt) {
    uint32_t time = (uint32_t)pkt[4] | (uint32_t)pkt[5] << 8 |
        (uint32_t)pkt[6] << 16 | (uint32_t)pkt[7] << 24;
    uint32_t requestor = (uint32_t)pkt[12] | (uint32_t)pkt[13] << 8 |
        (uint32_t)pkt[14] << 16 | (uint32_t)pkt[15] << 24;
    uint32_t selection = (uint32_t)pkt[16] | (uint32_t)pkt[17] << 8 |
        (uint32_t)pkt[18] << 16 | (uint32_t)pkt[19] << 24;
    uint32_t target = (uint32_t)pkt[20] | (uint32_t)pkt[21] << 8 |
        (uint32_t)pkt[22] << 16 | (uint32_t)pkt[23] << 24;
    uint32_t property = (uint32_t)pkt[24] | (uint32_t)pkt[25] << 8 |
        (uint32_t)pkt[26] << 16 | (uint32_t)pkt[27] << 24;
    bool primary;
    if (selection == lookup_atom(x, A_PRIMARY)) primary = true;
    else if (selection == lookup_atom(x, A_CLIPBOARD)) primary = false;
    else {
        selection_notify(x, requestor, selection, target, X11_NONE, time);
        return;
    }
    int slot = primary ? 1 : 0;
    if (x->interning[slot]) {
        struct deferred *wait = malloc(sizeof(*wait) + 32);
        if (!wait) {
            selection_notify(x, requestor, selection, target, X11_NONE, time);
            return;
        }
        wait->len = 32;
        memcpy(wait->data, pkt, 32);
        xlist_insert(x->sel_waiting[slot].prev, &wait->link);
        return;
    }
    if (target == lookup_atom(x, A_TARGETS)) {
        serve_targets(x, primary, pkt);
        return;
    }
    if (target == lookup_atom(x, A_TIMESTAMP)) {
        if (!property) property = target;
        uint32_t stamp = time;
        change_property(x, 0, requestor, property, XA_INTEGER, 32, &stamp, 1);
        selection_notify(x, requestor, selection, target, property, time);
        return;
    }
    struct wl_sel_cache *cache = &x->wl_sel[primary ? 1 : 0];
    if (!cache->present) {
        selection_notify(x, requestor, selection, target, X11_NONE, time);
        return;
    }
    const char *mime = cache_mime(x, target);
    const char *send_mime = NULL;
    if (mime) {
        for (size_t i = 0; i < cache->count; i++)
            if (strcmp(cache->mimes[i], mime) == 0) { send_mime = cache->mimes[i]; break; }
        if (!send_mime && strcmp(mime, "text/plain") == 0) {
            for (size_t i = 0; i < cache->count; i++)
                if (strcmp(cache->mimes[i], "text/plain;charset=utf-8") == 0) {
                    send_mime = cache->mimes[i];
                    break;
                }
        }
    }
    if (!send_mime) {
        selection_notify(x, requestor, selection, target, X11_NONE, time);
        return;
    }
    if (!property) property = target;
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK) < 0) {
        selection_notify(x, requestor, selection, target, X11_NONE, time);
        return;
    }
    struct transfer *transfer = calloc(1, sizeof(*transfer));
    if (!transfer) {
        close(pipes[0]);
        close(pipes[1]);
        selection_notify(x, requestor, selection, target, X11_NONE, time);
        return;
    }
    xlist_init(&transfer->link);
    transfer->x = x;
    transfer->primary = primary;
    transfer->fd = pipes[0];
    transfer->requestor = requestor;
    transfer->property = property;
    transfer->target = target;
    transfer->time = time;
    transfer->source = xev_add(x->sc, pipes[0], EPOLLIN, transfer_read, transfer);
    if (!transfer->source) {
        close(pipes[1]);
        transfer_free(transfer);
        selection_notify(x, requestor, selection, target, X11_NONE, time);
        return;
    }
    xlist_insert(&x->transfers, &transfer->link);
    size_t mlen = strlen(send_mime);
    if (mlen > 255) mlen = 255;
    uint8_t buf[sizeof(struct xwm_msg_sel_req) + 256];
    struct xwm_msg_sel_req req = { .primary = primary ? 1 : 0, .mime_len = (uint16_t)mlen };
    memcpy(buf, &req, sizeof(req));
    memcpy(buf + sizeof(req), send_mime, mlen);
    sidecar_send(x->sc, XWM_EVT_SELECTION_REQUEST, buf, (uint32_t)(sizeof(req) + mlen), pipes[1]);
}

static void convert_start_next(struct xwm *x);
static struct convert *convert_active(struct xwm *x);

static uint32_t convert_window(struct xwm *x, const struct convert *c) {
    (void)x;
    return c->window;
}

static void *convert_tag(const struct convert *c) {
    return (void *)(uintptr_t)c->id;
}

static struct convert *convert_tagged(struct xwm *x, void *ctx) {
    struct convert *c = convert_active(x);
    return c && c->id == (uint64_t)(uintptr_t)ctx ? c : NULL;
}

static void convert_arm(struct xwm *x, int ms) {
    struct itimerspec spec = {0};
    spec.it_value.tv_sec = ms / 1000;
    spec.it_value.tv_nsec = (long)(ms % 1000) * 1000000L;
    if (x->convert_timer_fd >= 0) timerfd_settime(x->convert_timer_fd, 0, &spec, NULL);
}

static struct convert *convert_active(struct xwm *x) {
    if (xlist_empty(&x->converts)) return NULL;
    struct convert *c = xcontainer_of(x->converts.next, c, link);
    return c->active ? c : NULL;
}

static void convert_free(struct convert *c) {
    if (c->fd >= 0) close(c->fd);
    free(c->data);
    free(c->mime);
    free(c);
}

static void adopt_free(void *ctx) {
    struct adopt *a = ctx;
    if (!a || --a->outstanding) return;
    for (size_t i = 0; i < a->count; i++) free(a->mimes[i]);
    free(a);
}

static void adopt_offer(struct xwm *x, struct adopt *a) {
    if (a->generation == x->sel_generation[a->primary ? 1 : 0]) {
        if (!a->count) {
            a->mimes[a->count++] = strdup("text/plain;charset=utf-8");
            a->mimes[a->count++] = strdup("text/plain");
        }
        uint8_t buf[4096];
        struct xwm_msg_sel_offer hdr = { .primary = a->primary ? 1 : 0, .count = 0 };
        size_t pos = sizeof(hdr);
        for (size_t i = 0; i < a->count; i++) {
            if (!a->mimes[i]) continue;
            size_t mlen = strlen(a->mimes[i]) + 1;
            if (pos + mlen > sizeof(buf)) break;
            memcpy(buf + pos, a->mimes[i], mlen);
            pos += mlen;
            hdr.count++;
        }
        memcpy(buf, &hdr, sizeof(hdr));
        sidecar_send(x->sc, XWM_EVT_SELECTION_OFFER, buf, (uint32_t)pos, -1);
    }
    for (size_t i = 0; i < a->count; i++) free(a->mimes[i]);
    free(a);
}

static void adopt_add(struct adopt *a, const char *mime) {
    for (size_t i = 0; i < a->count; i++)
        if (strcmp(a->mimes[i], mime) == 0) return;
    if (a->count < 64) a->mimes[a->count] = strdup(mime);
    if (a->count < 64 && a->mimes[a->count]) a->count++;
}

struct atom_name {
    struct adopt *adopt;
    uint32_t atom;
};

static void atom_name_drop(void *ctx) {
    struct atom_name *n = ctx;
    adopt_free(n->adopt);
    free(n);
}

static void atom_name_reply(struct xwm *x, const unsigned char *reply, size_t len, void *ctx) {
    struct atom_name *n = ctx;
    struct adopt *a = n->adopt;
    if (reply && len >= 32) {
        size_t name_len = (uint16_t)(reply[8] | reply[9] << 8);
        if (name_len > len - 32) name_len = len - 32;
        char *name = strndup((const char *)reply + 32, name_len);
        if (name && strchr(name, '/')) {
            cache_add(x, n->atom, name);
            adopt_add(a, name);
        }
        free(name);
    }
    free(n);
    if (--a->outstanding == 0) adopt_offer(x, a);
}

static void adopt_targets(struct xwm *x, struct convert *c, bool ok) {
    struct adopt *a = calloc(1, sizeof(*a));
    if (!a) return;
    a->primary = c->primary;
    a->generation = c->generation;
    a->outstanding = 1;
    if (ok && c->type == XA_ATOM) {
        size_t atoms = c->len / 4;
        for (size_t i = 0; i < atoms; i++) {
            uint32_t atom = reply_u32(c->data, i * 4);
            if (atom == lookup_atom(x, A_TARGETS) || atom == lookup_atom(x, A_TIMESTAMP) ||
                    atom == lookup_atom(x, A_INCR)) continue;
            const char *known = cache_mime(x, atom);
            if (known) {
                if (strchr(known, '/')) adopt_add(a, known);
                continue;
            }
            struct atom_name *n = calloc(1, sizeof(*n));
            if (!n) continue;
            n->adopt = a;
            n->atom = atom;
            a->outstanding++;
            struct req r = {0};
            r_begin(&r, 17);
            r_u32(&r, atom);
            expect(x, r_send(x, &r), atom_name_reply, n, atom_name_drop);
        }
    }
    if (--a->outstanding == 0) adopt_offer(x, a);
}

static void convert_finish(struct xwm *x, bool ok) {
    struct convert *c = convert_active(x);
    if (!c) return;
    xlist_remove(&c->link);
    convert_arm(x, 0);
    if (c->window && !x->stopping && !x->failed) simple_window(x, 4, c->window);
    if (c->kind == CONVERT_DATA) {
        if (ok) {
            write_async(x, c->fd, c->data, c->len);
            c->fd = -1;
            c->data = NULL;
        }
    } else {
        adopt_targets(x, c, ok);
    }
    convert_free(c);
    if (!x->stopping) convert_start_next(x);
}


static void convert_property_reply(struct xwm *x, const unsigned char *reply, size_t len, void *ctx);

static void convert_read(struct xwm *x, struct convert *c) {
    struct req r = {0};
    r_begin(&r, 20);
    r_u32(&r, convert_window(x, c));
    r_u32(&r, x->convert_property);
    r_u32(&r, 0);
    r_u32(&r, c->offset);
    r_u32(&r, x->max_request / 4);
    expect(x, r_send(x, &r), convert_property_reply, convert_tag(c), NULL);
    convert_arm(x, CONVERT_TIMEOUT_MS);
}

static bool convert_append(struct convert *c, const unsigned char *data, size_t length) {
    if (c->len + length > c->cap) {
        size_t cap = c->cap ? c->cap : 65536;
        while (cap < c->len + length) cap *= 2;
        unsigned char *grown = realloc(c->data, cap);
        if (!grown) return false;
        c->data = grown;
        c->cap = cap;
    }
    if (length) memcpy(c->data + c->len, data, length);
    c->len += length;
    return true;
}

static void convert_property_reply(struct xwm *x, const unsigned char *reply, size_t len, void *ctx) {
    struct convert *c = convert_tagged(x, ctx);
    if (!c) return;
    if (!reply) {
        convert_finish(x, false);
        return;
    }
    struct property property;
    property_from_reply(reply, len, &property);
    uint32_t window = convert_window(x, c);
    if (c->state == CS_PROP && c->offset == 0 && property.type == lookup_atom(x, A_INCR)) {
        property_finish(&property);
        delete_property(x, window, x->convert_property);
        c->state = CS_INCR_WAIT;
        c->type = XA_STRING;
        convert_arm(x, CONVERT_TIMEOUT_MS);
        return;
    }
    if (property.type == X11_NONE) {
        property_finish(&property);
        convert_finish(x, false);
        return;
    }
    if (c->state == CS_PROP && c->offset == 0) c->type = property.type;
    bool ok = convert_append(c, property.data, property.length);
    c->chunk += property.length;
    c->offset += (uint32_t)(property.length / 4);
    uint32_t after = property.after;
    property_finish(&property);
    if (!ok) {
        convert_finish(x, false);
        return;
    }
    if (after) {
        convert_read(x, c);
        return;
    }
    delete_property(x, window, x->convert_property);
    if (c->state == CS_PROP) {
        convert_finish(x, true);
        return;
    }
    if (!c->chunk) {
        convert_finish(x, true);
        return;
    }
    c->state = CS_INCR_WAIT;
    convert_arm(x, CONVERT_TIMEOUT_MS);
}

static void convert_send(struct xwm *x, struct convert *c) {
    uint32_t selection = lookup_atom(x, c->primary ? A_PRIMARY : A_CLIPBOARD);
    convert_selection(x, convert_window(x, c), selection, c->target, x->convert_property,
        X11_CURRENT_TIME);
    c->state = CS_NOTIFY;
    convert_arm(x, CONVERT_TIMEOUT_MS);
}

static void convert_atom_reply(struct xwm *x, const unsigned char *reply, size_t len, void *ctx) {
    struct convert *c = convert_tagged(x, ctx);
    if (!c || c->state != CS_ATOM) return;
    if (!reply || len < 12 || !reply_u32(reply, 8)) {
        convert_finish(x, false);
        return;
    }
    c->target = reply_u32(reply, 8);
    cache_add(x, c->target, c->mime);
    convert_send(x, c);
}

static void convert_start_next(struct xwm *x) {
    while (!xlist_empty(&x->converts)) {
        struct convert *c = xcontainer_of(x->converts.next, c, link);
        if (c->active) return;
        if (c->kind == CONVERT_TARGETS &&
                c->generation != x->sel_generation[c->primary ? 1 : 0]) {
            xlist_remove(&c->link);
            convert_free(c);
            continue;
        }
        c->active = true;
        c->id = ++x->convert_ids;
        c->window = r_rid(x);
        uint32_t values[32] = {0};
        values[11] = MASK_PROPERTY_CHANGE;
        create_window(x, c->window, x->root, 0, 1, 1, 1, CW_EVENT_MASK, values);
        if (!c->target) {
            c->state = CS_ATOM;
            expect(x, send_intern(x, c->mime), convert_atom_reply, convert_tag(c), NULL);
            convert_arm(x, CONVERT_TIMEOUT_MS);
            return;
        }
        convert_send(x, c);
        return;
    }
}

static void convert_enqueue(struct xwm *x, int kind, bool primary, uint32_t target,
        const char *mime, int fd) {
    struct convert *c = calloc(1, sizeof(*c));
    if (!c || (mime && !(c->mime = strdup(mime)))) {
        if (fd >= 0) close(fd);
        free(c);
        return;
    }
    c->kind = kind;
    c->primary = primary;
    c->target = target;
    c->fd = fd;
    c->generation = x->sel_generation[primary ? 1 : 0];
    xlist_insert(x->converts.prev, &c->link);
    convert_start_next(x);
}

static void handle_selection_notify(struct xwm *x, const unsigned char *pkt) {
    struct convert *c = convert_active(x);
    if (!c || c->state != CS_NOTIFY) return;
    uint32_t requestor = reply_u32(pkt, 8);
    uint32_t selection = reply_u32(pkt, 12);
    uint32_t target = reply_u32(pkt, 16);
    uint32_t property = reply_u32(pkt, 20);
    if (requestor != convert_window(x, c) || target != c->target ||
            selection != lookup_atom(x, c->primary ? A_PRIMARY : A_CLIPBOARD)) return;
    if (property != X11_NONE && property != x->convert_property) return;
    if (property == X11_NONE) {
        convert_finish(x, false);
        return;
    }
    c->state = CS_PROP;
    c->offset = 0;
    convert_read(x, c);
}

static bool convert_property_notify(struct xwm *x, uint32_t window, uint32_t atom, uint8_t state) {
    struct convert *c = convert_active(x);
    if (!c || window != convert_window(x, c) || atom != x->convert_property) return false;
    if (state == 0 && c->state == CS_INCR_WAIT) {
        c->state = CS_INCR_PROP;
        c->offset = 0;
        c->chunk = 0;
        convert_read(x, c);
    }
    return true;
}

static void convert_timeout(int fd, uint32_t events, void *data) {
    struct xwm *x = data;
    uint64_t expirations = 0;
    (void)events;
    ssize_t n;
    do n = read(fd, &expirations, sizeof(expirations));
    while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(expirations) || !expirations) return;
    if (convert_active(x)) {
        fprintf(stderr, "tomoe-xwm: selection conversion timed out\n");
        convert_finish(x, false);
    }
}

struct intern {
    int slot;
    char *mime;
};

static void intern_drop(void *ctx) {
    struct intern *in = ctx;
    free(in->mime);
    free(in);
}

static void intern_reply(struct xwm *x, const unsigned char *reply, size_t len, void *ctx) {
    struct intern *in = ctx;
    if (reply && len >= 12) cache_add(x, reply_u32(reply, 8), in->mime);
    int slot = in->slot;
    intern_drop(in);
    if (x->interning[slot] && --x->interning[slot]) return;
    struct xlist waiting;
    xlist_init(&waiting);
    while (!xlist_empty(&x->sel_waiting[slot])) {
        struct xlist *e = x->sel_waiting[slot].next;
        xlist_remove(e);
        xlist_insert(waiting.prev, e);
    }
    while (!xlist_empty(&waiting)) {
        struct deferred *d = xcontainer_of(waiting.next, d, link);
        xlist_remove(&d->link);
        handle_selection_request(x, d->data);
        free(d);
    }
}

static void intern_selection(struct xwm *x, int slot) {
    struct wl_sel_cache *cache = &x->wl_sel[slot];
    for (size_t i = 0; cache->present && i < cache->count; i++) {
        if (cache_atom(x, cache->mimes[i])) continue;
        struct intern *in = calloc(1, sizeof(*in));
        if (!in || !(in->mime = strdup(cache->mimes[i]))) {
            free(in);
            continue;
        }
        in->slot = slot;
        x->interning[slot]++;
        expect(x, send_intern(x, cache->mimes[i]), intern_reply, in, intern_drop);
    }
}

static void adopt_selection(struct xwm *x, bool primary) {
    int slot = primary ? 1 : 0;
    x->sel_generation[slot]++;
    convert_enqueue(x, CONVERT_TARGETS, primary, lookup_atom(x, A_TARGETS), NULL, -1);
}

static void handle_xfixes(struct xwm *x, const unsigned char *pkt) {
    uint32_t owner = (uint32_t)pkt[8] | (uint32_t)pkt[9] << 8 |
        (uint32_t)pkt[10] << 16 | (uint32_t)pkt[11] << 24;
    uint32_t selection = (uint32_t)pkt[12] | (uint32_t)pkt[13] << 8 |
        (uint32_t)pkt[14] << 16 | (uint32_t)pkt[15] << 24;
    bool primary;
    if (selection == lookup_atom(x, A_PRIMARY)) primary = true;
    else if (selection == lookup_atom(x, A_CLIPBOARD)) primary = false;
    else return;
    uint32_t ours = primary ? x->primary_window : x->clipboard_window;
    int slot = primary ? 1 : 0;
    if (owner == ours) {
        x->own[slot] = true;
        x->sel_generation[slot]++;
        return;
    }
    x->own[slot] = false;
    if (!owner) {
        x->sel_generation[slot]++;
        struct xwm_msg_sel_clear msg = { .primary = primary ? 1 : 0 };
        sidecar_send(x->sc, XWM_EVT_SELECTION_CLEAR, &msg, sizeof(msg), -1);
        return;
    }
    adopt_selection(x, primary);
}


static void setup_complete(struct xwm *x);

static void extension_reply(struct xwm *x, const unsigned char *reply, size_t len, void *ctx) {
    bool composite = ctx != NULL;
    if (reply && len >= 32 && reply[8]) {
        int opcode = reply[9];
        int event = reply[10];
        if (composite) {
            uint32_t version[2] = { 0, 4 };
            extension_request(x, opcode, 0, version, 2);
            struct req r = {0};
            r_begin(&r, (uint8_t)opcode);
            r_flag(&r, 1, 2);
            r_u32(&r, x->root);
            r_u8(&r, 1);
            r_u8(&r, 0); r_u8(&r, 0); r_u8(&r, 0);
            r_send(x, &r);
        } else {
            x->xfixes = opcode;
            x->xfixes_event = event;
            uint32_t version[2] = { 3, 0 };
            xfixes_request(x, 0, version, 2);
            uint32_t select[3];
            select[0] = x->clipboard_window;
            select[1] = lookup_atom(x, A_CLIPBOARD);
            select[2] = 7;
            xfixes_request(x, 2, select, 3);
            select[0] = x->primary_window;
            select[1] = lookup_atom(x, A_PRIMARY);
            xfixes_request(x, 2, select, 3);
        }
    }
    if (--x->setup_queries == 0) setup_complete(x);
}

static void send_query_extension(struct xwm *x, const char *name, bool composite) {
    struct req r = {0};
    size_t len = strlen(name);
    r_begin(&r, 98);
    r_u16(&r, (uint16_t)len);
    r_u16(&r, 0);
    r_bytes(&r, name, len);
    expect(x, r_send(x, &r), extension_reply, composite ? (void *)x : NULL, NULL);
}

static void setup_finish(struct xwm *x) {
    x->convert_property = lookup_atom(x, A_TOMOE_CONVERT);
    x->wm_window = r_rid(x);
    uint32_t values[32] = {0};
    values[11] = 0;
    create_window(x, x->wm_window, x->root, 0, 2, 1, 1, CW_EVENT_MASK, values);
    x->clipboard_window = r_rid(x);
    values[11] = MASK_PROPERTY_CHANGE;
    create_window(x, x->clipboard_window, x->root, 0, 1, 1, 1, CW_EVENT_MASK, values);
    x->primary_window = r_rid(x);
    create_window(x, x->primary_window, x->root, 0, 1, 1, 1, CW_EVENT_MASK, values);
    set_event_mask(x, x->root, MASK_SUBSTRUCTURE_REDIRECT | MASK_SUBSTRUCTURE_NOTIFY |
        MASK_PROPERTY_CHANGE | MASK_FOCUS_CHANGE);
    uint32_t check = x->wm_window;
    change_property(x, 0, x->root, lookup_atom(x, A_NET_SUPPORTING_WM_CHECK), XA_WINDOW, 32,
        &check, 1);
    change_property(x, 0, x->wm_window, lookup_atom(x, A_NET_SUPPORTING_WM_CHECK), XA_WINDOW, 32,
        &check, 1);
    const char name[] = "tomoe";
    change_property(x, 0, x->wm_window, lookup_atom(x, A_NET_WM_NAME),
        lookup_atom(x, A_UTF8_STRING), 8, name, sizeof(name) - 1);
    uint32_t supported[] = {
        lookup_atom(x, A_NET_SUPPORTED), lookup_atom(x, A_NET_SUPPORTING_WM_CHECK),
        lookup_atom(x, A_NET_ACTIVE_WINDOW), lookup_atom(x, A_NET_CLIENT_LIST),
        lookup_atom(x, A_NET_CLIENT_LIST_STACKING), lookup_atom(x, A_NET_CLOSE_WINDOW),
        lookup_atom(x, A_NET_WM_STATE), lookup_atom(x, A_NET_WM_STATE_FULLSCREEN),
        lookup_atom(x, A_NET_WM_STATE_MAXIMIZED_VERT),
        lookup_atom(x, A_NET_WM_STATE_MAXIMIZED_HORZ), lookup_atom(x, A_NET_WM_NAME),
    };
    change_property(x, 0, x->root, lookup_atom(x, A_NET_SUPPORTED), XA_ATOM, 32, supported,
        sizeof(supported) / sizeof(*supported));
    refresh_client_list(x);
    refresh_active(x);
    x->setup_queries = 2;
    send_query_extension(x, "Composite", true);
    send_query_extension(x, "XFIXES", false);
}

static void intern_selection(struct xwm *x, int slot);

static void setup_complete(struct xwm *x) {
    set_owner(x, x->wm_window, lookup_atom(x, A_WM_S0), X11_CURRENT_TIME);
    intern_selection(x, 0);
    intern_selection(x, 1);
    for (int primary = 0; primary < 2; primary++) {
        if (!x->wl_sel[primary].present) continue;
        set_owner(x, primary ? x->primary_window : x->clipboard_window,
            lookup_atom(x, primary ? A_PRIMARY : A_CLIPBOARD), X11_CURRENT_TIME);
        x->own[primary] = true;
    }
    x->ready = true;
    x->setup_state = SETUP_READY;
}

static void setup_atoms(struct xwm *x) {
    for (int i = 0; i < A_COUNT; i++) {
        if (!atom_names[i]) continue;
        struct req r = {0};
        size_t len = strlen(atom_names[i]);
        r_begin(&r, 16);
        r_flag(&r, 1, 0);
        r_u16(&r, (uint16_t)len);
        r_u16(&r, 0);
        r_bytes(&r, atom_names[i], len);
        x->atom_seq[i] = r_send(x, &r);
        x->atom_pending++;
    }
    x->setup_state = SETUP_ATOMS;
}

static void setup_reply(struct xwm *x, unsigned char *reply, size_t length) {
    if (reply[0] != 1 || length < 40) {
        x_fail(x, "setup refused", 0);
        return;
    }
    x->rid_base = (uint32_t)reply[12] | (uint32_t)reply[13] << 8 |
        (uint32_t)reply[14] << 16 | (uint32_t)reply[15] << 24;
    x->rid_mask = (uint32_t)reply[16] | (uint32_t)reply[17] << 8 |
        (uint32_t)reply[18] << 16 | (uint32_t)reply[19] << 24;
    x->max_request = ((uint32_t)reply[26] | (uint32_t)reply[27] << 8) * 4;
    uint16_t vendor_len = (uint16_t)(reply[24] | reply[25] << 8);
    uint8_t formats = reply[29];
    size_t offset = 40 + ((vendor_len + 3u) & ~3u) + (size_t)formats * 8;
    if (offset + 4 <= length) {
        const unsigned char *screen = reply + offset;
        x->root = (uint32_t)screen[0] | (uint32_t)screen[1] << 8 |
            (uint32_t)screen[2] << 16 | (uint32_t)screen[3] << 24;
    }
    if (!x->root) {
        x_fail(x, "setup has no root window", 0);
        return;
    }
    x->rid_next = 0;
    setup_atoms(x);
}

static void setup_atom_reply(struct xwm *x, unsigned char *reply, size_t length) {
    uint16_t seq = (uint16_t)(reply[2] | reply[3] << 8);
    if (length < 12) {
        x_fail(x, "short atom reply", 0);
        return;
    }
    uint32_t atom = (uint32_t)reply[8] | (uint32_t)reply[9] << 8 |
        (uint32_t)reply[10] << 16 | (uint32_t)reply[11] << 24;
    for (int i = 0; i < A_COUNT; i++) {
        if (!atom_names[i] || x->atom_seq[i] != seq) continue;
        x->atoms[i] = atom;
        if (x->atom_pending) x->atom_pending--;
        break;
    }
    if (!x->atom_pending) setup_finish(x);
}

static void x_handle(struct xwm *x, unsigned char *packet, size_t length) {
    uint8_t type = packet[0] & 0x7f;
    if (type == 0 && pending_dispatch(x, packet, length)) return;
    if (type == 0) {
        uint16_t seq = (uint16_t)(packet[2] | packet[3] << 8);
        struct prop_request *request, *next_request;
        xlist_for_each_safe(request, next_request, &x->props, link) {
            if (request->seq != seq) continue;
            xlist_remove(&request->link);
            free(request);
            break;
        }
        return;
    }
    if (type == 1) {
        if (x->setup_state == SETUP_WAIT) {
            setup_reply(x, packet, length);
            return;
        }
        if (pending_dispatch(x, packet, length)) return;
        if (x->setup_state == SETUP_WAIT) setup_reply(x, packet, length);
        else if (x->setup_state == SETUP_ATOMS) setup_atom_reply(x, packet, length);
        else if (x->ready) handle_property_reply(x, packet, length);
        return;
    }
    if (x->xfixes_event && type == x->xfixes_event) {
        handle_xfixes(x, packet);
        return;
    }
    switch (type) {
    case 16: handle_create(x, packet); break;
    case 17: handle_destroy(x, packet); break;
    case 18: handle_unmap_notify(x, packet); break;
    case 19: handle_map_notify(x, packet); break;
    case 20: handle_map_request(x, packet); break;
    case 22: handle_configure_notify(x, packet); break;
    case 23: handle_configure_request(x, packet); break;
    case 28: handle_property_notify(x, packet); break;
    case 30: handle_selection_request(x, packet); break;
    case 31: handle_selection_notify(x, packet); break;
    case 33: handle_client_message(x, packet); break;
    default: break;
    }
    (void)length;
}

static void x_drain_buffered(struct xwm *x) {
    while (!x->failed && x->in_len >= 4) {
        size_t need = packet_length(x, x->in, x->in_len);
        if (!need || x->in_len < need) break;
        unsigned char *packet = malloc(need);
        if (!packet) break;
        memcpy(packet, x->in, need);
        memmove(x->in, x->in + need, x->in_len - need);
        x->in_len -= need;
        x_handle(x, packet, need);
        free(packet);
    }
}

static void xwm_event(int fd, uint32_t events, void *data) {
    struct xwm *x = data;
    (void)fd;
    if (events & EPOLLOUT) x_flush(x);
    if (events & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
        for (;;) {
            ssize_t n = x_recv(x);
            if (n == -2) break;
            if (n <= 0) {
                x_fail(x, n ? "recv failed" : "connection closed", n ? errno : 0);
                return;
            }
            x->in_len += (size_t)n;
        }
        x_drain_buffered(x);
    }
}

struct xwm *xwm_create(struct xwm_sidecar *sc, int wmfd) {
    if (wmfd < 0) return NULL;
    struct xwm *x = calloc(1, sizeof(*x));
    if (!x) return NULL;
    x->sc = sc;
    x->fd = wmfd;
    xlist_init(&x->pending);
    xlist_init(&x->converts);
    xlist_init(&x->sel_waiting[0]);
    xlist_init(&x->sel_waiting[1]);
    xlist_init(&x->windows);
    xlist_init(&x->writers);
    xlist_init(&x->transfers);
    xlist_init(&x->props);
    x->convert_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (x->convert_timer_fd >= 0)
        x->convert_timer = xev_add(sc, x->convert_timer_fd, EPOLLIN, convert_timeout, x);
    x->source = xev_add(sc, wmfd, EPOLLIN, xwm_event, x);
    if (!x->source || !x->convert_timer) {
        if (x->source) xev_del(sc, x->source);
        if (x->convert_timer) xev_del(sc, x->convert_timer);
        if (x->convert_timer_fd >= 0) close(x->convert_timer_fd);
        free(x);
        return NULL;
    }
    unsigned char request[12] = { 'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    x->setup_state = SETUP_WAIT;
    x_write(x, request, sizeof(request));
    return x;
}

static void wl_sel_clear(struct wl_sel_cache *c) {
    for (size_t i = 0; i < c->count; i++) free(c->mimes[i]);
    c->count = 0;
    c->present = false;
}

void xwm_destroy(struct xwm *x) {
    if (!x) return;
    x->stopping = true;
    if (x->source) xev_del(x->sc, x->source);
    struct xwm_window *w, *next;
    xlist_for_each_safe(w, next, &x->windows, link) window_free(w);
    writers_free_all(x);
    transfers_free_all(x);
    struct prop_request *request, *next_request;
    xlist_for_each_safe(request, next_request, &x->props, link) {
        xlist_remove(&request->link);
        free(request);
    }
    struct pending *p, *next_pending;
    xlist_for_each_safe(p, next_pending, &x->pending, link) {
        xlist_remove(&p->link);
        if (p->drop) p->drop(p->ctx);
        free(p);
    }
    struct convert *c, *next_convert;
    xlist_for_each_safe(c, next_convert, &x->converts, link) {
        xlist_remove(&c->link);
        convert_free(c);
    }
    for (int slot = 0; slot < 2; slot++) {
        struct deferred *d, *next_d;
        xlist_for_each_safe(d, next_d, &x->sel_waiting[slot], link) {
            xlist_remove(&d->link);
            free(d);
        }
    }
    for (size_t i = 0; i < x->cache_count; i++) free(x->cache[i].mime);
    free(x->cache);
    if (x->convert_timer) xev_del(x->sc, x->convert_timer);
    if (x->convert_timer_fd >= 0) close(x->convert_timer_fd);
    wl_sel_clear(&x->wl_sel[0]);
    wl_sel_clear(&x->wl_sel[1]);
    free(x->in);
    free(x->out);
    free(x);
}

void xwm_handle_cmd(struct xwm *x, const struct xwm_hdr *hdr, const uint8_t *payload, int fd) {
    switch (hdr->type) {
    case XWM_CMD_CONFIGURE: {
        if (hdr->len < sizeof(struct xwm_cmd_configure)) break;
        struct xwm_cmd_configure cmd;
        memcpy(&cmd, payload, sizeof(cmd));
        struct xwm_window *w = window_for(x, cmd.xid);
        if (!w) break;
        uint32_t mask = 0;
        uint32_t values[7] = {0};
        if (cmd.has_pos) {
            mask |= CFG_X | CFG_Y;
            values[0] = (uint32_t)cmd.x;
            values[1] = (uint32_t)cmd.y;
        }
        if (cmd.has_size && cmd.width > 0 && cmd.height > 0) {
            mask |= CFG_WIDTH | CFG_HEIGHT;
            values[2] = (uint32_t)cmd.width;
            values[3] = (uint32_t)cmd.height;
        }
        if (mask) configure_window(x, w->xid, mask, values);
        break;
    }
    case XWM_CMD_ACTIVATE: {
        if (hdr->len < sizeof(struct xwm_cmd_activate)) break;
        struct xwm_cmd_activate cmd;
        memcpy(&cmd, payload, sizeof(cmd));
        struct xwm_window *w = window_for(x, cmd.xid);
        if (!w || !cmd.activated) break;
        if (!(w->has_input_hint && !w->input_hint)) set_focus(x, w->xid);
        if (w->take_focus) {
            uint32_t data[5] = { lookup_atom(x, A_WM_TAKE_FOCUS), X11_CURRENT_TIME, 0, 0, 0 };
            client_message(x, w->xid, lookup_atom(x, A_WM_PROTOCOLS), 0, data);
        }
        x->active = w->xid;
        refresh_active(x);
        break;
    }
    case XWM_CMD_STATE: {
        if (hdr->len < sizeof(struct xwm_cmd_state)) break;
        struct xwm_cmd_state cmd;
        memcpy(&cmd, payload, sizeof(cmd));
        struct xwm_window *w = window_for(x, cmd.xid);
        if (!w) break;
        if ((cmd.fullscreen != 0) != w->fullscreen)
            state_atom(w, lookup_atom(x, A_NET_WM_STATE_FULLSCREEN), cmd.fullscreen != 0);
        if ((cmd.maximized != 0) != w->maximized) {
            state_atom(w, lookup_atom(x, A_NET_WM_STATE_MAXIMIZED_VERT), cmd.maximized != 0);
            state_atom(w, lookup_atom(x, A_NET_WM_STATE_MAXIMIZED_HORZ), cmd.maximized != 0);
        }
        w->fullscreen = cmd.fullscreen != 0;
        w->maximized = cmd.maximized != 0;
        break;
    }
    case XWM_CMD_CLOSE: {
        if (hdr->len < sizeof(struct xwm_msg_window_id)) break;
        struct xwm_msg_window_id cmd;
        memcpy(&cmd, payload, sizeof(cmd));
        struct xwm_window *w = window_for(x, cmd.xid);
        if (!w) break;
        if (w->delete_window) {
            uint32_t data[5] = { lookup_atom(x, A_WM_DELETE_WINDOW), X11_CURRENT_TIME, 0, 0, 0 };
            client_message(x, w->xid, lookup_atom(x, A_WM_PROTOCOLS), 0, data);
        } else {
            w->close_pending = true;
            request_prop(w, PROP_PROTOCOLS, lookup_atom(x, A_WM_PROTOCOLS));
        }
        break;
    }
    case XWM_CMD_WAYLAND_SELECTION: {
        if (hdr->len < sizeof(struct xwm_cmd_wayland_sel)) break;
        struct xwm_cmd_wayland_sel cmd;
        memcpy(&cmd, payload, sizeof(cmd));
        int slot = cmd.primary ? 1 : 0;
        struct wl_sel_cache *cache = &x->wl_sel[slot];
        wl_sel_clear(cache);
        if (cmd.present) {
            cache->present = true;
            const char *p = (const char *)(payload + sizeof(cmd));
            const char *end = (const char *)(payload + hdr->len);
            while (p < end && cache->count < 64 && cache->count < cmd.count) {
                const char *nul = memchr(p, '\0', (size_t)(end - p));
                if (!nul) break;
                if (nul > p) {
                    char *dup = strndup(p, (size_t)(nul - p));
                    if (dup) cache->mimes[cache->count++] = dup;
                }
                p = nul + 1;
            }
        }
        x->sel_generation[slot]++;
        if (x->ready) intern_selection(x, slot);
        if (x->ready) {
            uint32_t selection = lookup_atom(x, cmd.primary ? A_PRIMARY : A_CLIPBOARD);
            uint32_t window = cmd.primary ? x->primary_window : x->clipboard_window;
            if (cache->present && !x->own[slot]) {
                set_owner(x, window, selection, X11_CURRENT_TIME);
                x->own[slot] = true;
            } else if (!cache->present && x->own[slot]) {
                set_owner(x, X11_NONE, selection, X11_CURRENT_TIME);
                x->own[slot] = false;
            }
        }
        break;
    }
    case XWM_CMD_SEND_SELECTION: {
        if (hdr->len < sizeof(struct xwm_msg_sel_req) || fd < 0) break;
        struct xwm_msg_sel_req cmd;
        memcpy(&cmd, payload, sizeof(cmd));
        if (sizeof(cmd) + cmd.mime_len > hdr->len || !x->ready) break;
        char mime[256];
        size_t copy = cmd.mime_len < sizeof(mime) - 1 ? cmd.mime_len : sizeof(mime) - 1;
        memcpy(mime, payload + sizeof(cmd), copy);
        mime[copy] = '\0';
        int out_fd = fd;
        fd = -1;
        fcntl(out_fd, F_SETFD, FD_CLOEXEC);
        convert_enqueue(x, CONVERT_DATA, cmd.primary != 0, cache_atom(x, mime), mime, out_fd);
        break;
    }
    default:
        break;
    }
    if (fd >= 0) close(fd);
}
