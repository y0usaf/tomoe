#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define listen tomoe_listen
#include "internal.h"

enum {
    A_WM_PROTOCOLS, A_WM_DELETE_WINDOW, A_WM_TAKE_FOCUS, A_WM_STATE, A_WM_CLASS, A_WM_NAME,
    A_WM_TRANSIENT_FOR, A_WM_NORMAL_HINTS, A_WM_HINTS, A_WM_CHANGE_STATE, A_WM_S0,
    A_NET_WM_NAME, A_NET_WM_STATE, A_NET_WM_STATE_FULLSCREEN, A_NET_WM_STATE_MAXIMIZED_VERT,
    A_NET_WM_STATE_MAXIMIZED_HORZ, A_NET_WM_STATE_MODAL, A_NET_WM_STATE_HIDDEN,
    A_NET_ACTIVE_WINDOW, A_NET_CLIENT_LIST, A_NET_CLIENT_LIST_STACKING, A_NET_SUPPORTED,
    A_NET_SUPPORTING_WM_CHECK, A_NET_WM_WINDOW_TYPE, A_NET_WM_WINDOW_TYPE_DIALOG,
    A_NET_CLOSE_WINDOW, A_UTF8_STRING, A_TEXT, A_TARGETS, A_TIMESTAMP, A_INCR, A_CLIPBOARD,
    A_PRIMARY, A_CLIPBOARD_MANAGER, A_WL_SURFACE_SERIAL, A_WL_SURFACE_ID, A_COUNT,
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
#define MAX_CLIPBOARD (256 * 1024)
#define INCR_CHUNK (64 * 1024)

struct xwm_window;
struct xwm;

struct deferred {
    struct wl_list link;
    size_t len;
    unsigned char data[];
};

struct xwm {
    struct tomoe *server;
    int fd;
    struct wl_event_source *source;
    unsigned char *in;
    size_t in_len, in_cap;
    struct wl_list deferred;
    uint16_t seq;
    uint32_t rid_base, rid_mask, rid_next;
    uint32_t root, root_width, root_height, max_request;
    uint32_t wm_window, clipboard_window, primary_window, convert_property;
    uint32_t atoms[A_COUNT];
    int xfixes, xfixes_event;
    int setup_state;
    size_t atom_pending;
    uint16_t atom_seq[A_COUNT];
    bool ready;
    struct wl_list windows, writers, transfers, props;
    uint32_t active;
    bool own[2];
    bool stopping;
    bool failed;
};

struct xwm_window {
    struct wl_list link;
    struct xwm *xwm;
    uint32_t xid;
    int x, y, width, height;
    bool override, mapped, x_mapped, delete_window, take_focus, input_hint, has_input_hint;
    bool fullscreen, maximized, minimized, dialog;
    char *title, *appid;
    uint32_t transient;
    int min_width, min_height;
    bool net_title;
    struct surface *surface;
    struct window *window;
    struct xdg_toplevel *toplevel;
    struct xdg_surface *base;
    struct node *node;
    struct target target;
    struct wl_listener surface_destroy, surface_map, surface_unmap;
};

struct req {
    unsigned char *data;
    size_t len, cap;
};

static void x_write(struct xwm *x, const void *data, size_t len) {
    if (x->stopping) return;
    const unsigned char *p = data;
    while (len) {
        ssize_t n = send(x->fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            tomoe_log(LOG_DEBUG, "tomoe: xwm send failed on %d: %s", x->fd, strerror(errno));
            x->failed = true;
            return;
        }
        p += n;
        len -= (size_t)n;
    }
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
    if (!seq) seq = ++x->seq;
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

static ssize_t x_recv(struct xwm *x, int wait) {
    for (;;) {
        if (!x_ensure(x, 4096)) return -1;
        ssize_t n = recv(x->fd, x->in + x->in_len, x->in_cap - x->in_len,
            wait ? 0 : MSG_DONTWAIT);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN && !wait) return -2;
        if (errno == EAGAIN) {
            struct pollfd pollfd = { .fd = x->fd, .events = POLLIN };
            int ready = poll(&pollfd, 1, 5000);
            if (ready <= 0) {
                tomoe_log(LOG_DEBUG, "tomoe: xwm recv stalled on %d", x->fd);
                return -1;
            }
            continue;
        }
        tomoe_log(LOG_DEBUG, "tomoe: xwm recv failed on %d: %s", x->fd, strerror(errno));
        return -1;
    }
}

static size_t packet_length(struct xwm *x, const unsigned char *p, size_t available) {
    if (available < 8) return 0;
    uint8_t type = p[0] & 0x7f;
    if (type == 0) return 32;
    if (type == 1 && !x->seq)
        return 8 + ((size_t)p[6] | (size_t)p[7] << 8) * 4;
    size_t words = (size_t)p[4] | (size_t)p[5] << 8 | (size_t)p[6] << 16 |
        (size_t)p[7] << 24;
    if (type == 1 || type == 35) return 32 + words * 4;
    return 32;
}

static unsigned char *x_packet(struct xwm *x, size_t *length) {
    for (;;) {
        size_t need = x->in_len >= 4 ? packet_length(x, x->in, x->in_len) : 0;
        if (need && x->in_len >= need) {
            unsigned char *packet = malloc(need);
            if (!packet) return NULL;
            memcpy(packet, x->in, need);
            memmove(x->in, x->in + need, x->in_len - need);
            x->in_len -= need;
            *length = need;
            return packet;
        }
        ssize_t n = x_recv(x, 1);
        if (n <= 0) {
            tomoe_log(LOG_DEBUG, "tomoe: xwm connection lost on %d", x->fd);
            x->failed = true;
            return NULL;
        }
        x->in_len += (size_t)n;
    }
}

static void defer(struct xwm *x, unsigned char *packet, size_t len) {
    struct deferred *entry = malloc(sizeof(*entry) + len);
    if (!entry) {
        free(packet);
        return;
    }
    entry->len = len;
    memcpy(entry->data, packet, len);
    wl_list_insert(x->deferred.prev, &entry->link);
    free(packet);
}

static unsigned char *x_wait(struct xwm *x, uint16_t seq, size_t *length, int *error) {
    for (;;) {
        size_t len = 0;
        unsigned char *packet = x_packet(x, &len);
        if (!packet) return NULL;
        uint16_t packet_seq = (uint16_t)(packet[2] | packet[3] << 8);
        if (packet_seq == seq && (packet[0] == 0 || packet[0] == 1)) {
            if (packet[0] == 0) {
                if (error) *error = packet[1];
                free(packet);
                return NULL;
            }
            *length = len;
            return packet;
        }
        defer(x, packet, len);
    }
}

static uint32_t atom_intern(struct xwm *x, const char *name, bool only_if_exists) {
    struct req r = {0};
    size_t len = strlen(name);
    r_begin(&r, 16);
    r_flag(&r, 1, only_if_exists ? 1 : 0);
    r_u16(&r, (uint16_t)len);
    r_u16(&r, 0);
    r_bytes(&r, name, len);
    uint16_t seq = r_send(x, &r);
    size_t reply_len = 0;
    int error = 0;
    unsigned char *reply = x_wait(x, seq, &reply_len, &error);
    if (!reply || reply_len < 12) {
        free(reply);
        return 0;
    }
    uint32_t atom = (uint32_t)reply[8] | (uint32_t)reply[9] << 8 |
        (uint32_t)reply[10] << 16 | (uint32_t)reply[11] << 24;
    free(reply);
    return atom;
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
    struct wl_list link;
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

static bool get_property(struct xwm *x, uint32_t window, uint32_t property, uint32_t type,
        uint32_t offset, uint32_t length, struct property *out) {
    struct req r = {0};
    r_begin(&r, 20);
    r_u32(&r, window);
    r_u32(&r, property);
    r_u32(&r, type);
    r_u32(&r, offset);
    r_u32(&r, length);
    uint16_t seq = r_send(x, &r);
    size_t reply_len = 0;
    int error = 0;
    unsigned char *reply = x_wait(x, seq, &reply_len, &error);
    if (!reply) return false;
    property_from_reply(reply, reply_len, out);
    free(reply);
    return true;
}

static void property_finish(struct property *property) {
    free(property->data);
    property->data = NULL;
}

static bool read_property_all(struct xwm *x, uint32_t window, uint32_t property,
        struct property *out) {
    memset(out, 0, sizeof(*out));
    uint32_t offset = 0;
    for (;;) {
        struct property part;
        if (!get_property(x, window, property, 0, offset, x->max_request / 4, &part)) {
            property_finish(out);
            return false;
        }
        uint32_t items = (uint32_t)((part.length + 3) / 4);
        if (!out->data) {
            out->type = part.type;
            out->format = part.format;
            out->data = part.data;
            out->length = part.length;
        } else {
            unsigned char *data = realloc(out->data, out->length + part.length);
            if (!data) {
                property_finish(&part);
                property_finish(out);
                return false;
            }
            out->data = data;
            memcpy(out->data + out->length, part.data, part.length);
            out->length += part.length;
            free(part.data);
        }
        bool more = part.after > 0;
        offset += items;
        if (!more || !items) break;
    }
    return true;
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

static bool query_extension(struct xwm *x, const char *name, int *opcode, int *event) {
    struct req r = {0};
    size_t len = strlen(name);
    r_begin(&r, 98);
    r_u16(&r, (uint16_t)len);
    r_u16(&r, 0);
    r_bytes(&r, name, len);
    uint16_t seq = r_send(x, &r);
    size_t reply_len = 0;
    int error = 0;
    unsigned char *reply = x_wait(x, seq, &reply_len, &error);
    if (!reply || reply_len < 32) {
        free(reply);
        return false;
    }
    bool present = reply[8] != 0;
    *opcode = reply[9];
    *event = reply[10];
    free(reply);
    return present;
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
    wl_list_for_each(w, &x->windows, link)
        if (w->xid == xid) return w;
    return NULL;
}

static void sync_parent(struct xwm_window *w) {
    if (!w->toplevel) return;
    struct xwm_window *parent = w->transient ? window_for(w->xwm, w->transient) : NULL;
    while (parent == w) parent = NULL;
    w->toplevel->parent = parent && parent != w ? parent->toplevel : NULL;
    w->toplevel->external_dialog = w->dialog;
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

static void push_metadata(struct xwm_window *w) {
    if (!w->toplevel) return;
    char *title = replace_string(&w->toplevel->title, w->title);
    if (title) wl_signal_emit_mutable(&w->toplevel->events.set_title, NULL);
    char *app_id = replace_string(&w->toplevel->app_id, w->appid);
    if (app_id) wl_signal_emit_mutable(&w->toplevel->events.set_app_id, NULL);
    sync_parent(w);
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
    wl_list_insert(&w->xwm->props, &request->link);
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
        push_metadata(w);
        schedule_scene(w->xwm->server);
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
            push_metadata(w);
            schedule_scene(w->xwm->server);
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
        sync_parent(w);
        if (w->toplevel) wl_signal_emit_mutable(&w->toplevel->events.metadata, NULL);
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
        break;
    case PROP_STATE:
        w->fullscreen = atom_list_has(property, lookup_atom(x, A_NET_WM_STATE_FULLSCREEN));
        w->maximized = atom_list_has(property,
            lookup_atom(x, A_NET_WM_STATE_MAXIMIZED_VERT)) ||
            atom_list_has(property, lookup_atom(x, A_NET_WM_STATE_MAXIMIZED_HORZ));
        if (w->toplevel) {
            w->toplevel->current.fullscreen = w->fullscreen;
            w->toplevel->current.maximized = w->maximized;
        }
        break;
    case PROP_TYPE:
        if (atom_list_has(property, lookup_atom(x, A_NET_WM_WINDOW_TYPE_DIALOG))) {
            w->dialog = true;
            sync_parent(w);
            if (w->toplevel) wl_signal_emit_mutable(&w->toplevel->events.metadata, NULL);
        }
        break;
    }
}

static void handle_property_reply(struct xwm *x, unsigned char *packet, size_t length) {
    uint16_t seq = (uint16_t)(packet[2] | packet[3] << 8);
    struct prop_request *request;
    wl_list_for_each(request, &x->props, link) {
        if (request->seq != seq) continue;
        struct property property;
        property_from_reply(packet, length, &property);
        apply_prop(request, &property);
        property_finish(&property);
        wl_list_remove(&request->link);
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
    size_t count = 0;
    struct xwm_window *w;
    wl_list_for_each(w, &x->windows, link) {
        if (w->override || !w->x_mapped || count == 256) continue;
        list[count++] = w->xid;
    }
    change_property(x, 0, x->root, lookup_atom(x, A_NET_CLIENT_LIST), XA_WINDOW, 32, list,
        (uint32_t)count);
    change_property(x, 0, x->root, lookup_atom(x, A_NET_CLIENT_LIST_STACKING), XA_WINDOW, 32,
        list, (uint32_t)count);
}

static void refresh_active(struct xwm *x) {
    change_property(x, 0, x->root, lookup_atom(x, A_NET_ACTIVE_WINDOW), XA_WINDOW, 32,
        &x->active, 1);
}

static void logical_to_world(struct tomoe *s, int lx, int ly, int *ox, int *oy,
        double *scale) {
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

static void override_place(struct xwm_window *w) {
    if (!w->node) return;
    int x = 0, y = 0;
    double scale = 1;
    logical_to_world(w->xwm->server, w->x, w->y, &x, &y, &scale);
    w->target.x = x;
    w->target.y = y;
    w->target.scale = scale;
    w->target.client_width = w->width;
    w->target.client_height = w->height;
    if (w->surface) surface_set_scale(w->surface, scale);
    node_set_enabled(w->node, w->surface && w->surface->mapped && w->x_mapped);
    if (w->x_mapped) node_raise_to_top(w->node);
    schedule_scene(w->xwm->server);
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
    schedule_scene(w->xwm->server);
}

static void override_unmapped(struct wl_listener *listener, void *data) {
    struct xwm_window *w = wl_container_of(listener, w, surface_unmap);
    if (w->node) node_set_enabled(w->node, false);
    schedule_scene(w->xwm->server);
}

static bool attach_override(struct xwm_window *w, struct surface *surface) {
    detach(&w->surface_map);
    detach(&w->surface_unmap);
    if (w->node && w->surface != surface) {
        node_destroy(w->node);
        w->node = NULL;
    }
    if (!w->node) {
        w->node = node_create(w->xwm->server->popup_tree);
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
        base->server = w->xwm->server;
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
        w->window = xdg_toplevel_created(w->xwm->server, toplevel);
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
        return true;
    }
    w->surface = surface;
    listen(&w->surface_destroy, &surface->events.destroy, surface_gone);
    if (!window_attach_surface(w->window, surface)) return false;
    push_metadata(w);
    return true;
}

void xwm_associate(struct tomoe *s, uint32_t xid, struct surface *surface) {
    struct xwm *x = s->xwm;
    if (!x || !x->ready || !surface) return;
    struct xwm_window *w = window_for(x, xid);
    if (!w) return;
    if (w->surface == surface) return;
    detach(&w->surface_destroy);
    if (w->override) attach_override(w, surface);
    else attach_managed(w, surface);
}

static void window_free(struct xwm_window *w) {
    struct xwm *x = w->xwm;
    struct prop_request *request, *next_request;
    wl_list_for_each_safe(request, next_request, &x->props, link) {
        if (request->window != w) continue;
        wl_list_remove(&request->link);
        free(request);
    }
    detach(&w->surface_destroy);
    if (w->override) {
        if (w->node) node_destroy(w->node);
    } else {
        if (w->surface) w->surface->external_toplevel = NULL;
        if (w->toplevel) {
            wl_signal_emit_mutable(&w->toplevel->events.destroy, NULL);
            free(w->toplevel->title);
            free(w->toplevel->app_id);
            free(w->toplevel->tag);
            free(w->toplevel->description);
            free(w->toplevel);
        }
        if (w->base && w->base->scene) node_destroy(w->base->scene);
        free(w->base);
    }
    if (x->active == w->xid) {
        x->active = 0;
        refresh_active(x);
    }
    wl_list_remove(&w->link);
    free(w->title);
    free(w->appid);
    free(w);
}

static struct xwm_window *window_new(struct xwm *x, uint32_t xid, bool override,
        int x_, int y_, int width, int height) {
    struct xwm_window *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->xwm = x;
    w->xid = xid;
    w->override = override;
    w->x = x_;
    w->y = y_;
    w->width = width;
    w->height = height;
    wl_list_init(&w->surface_destroy.link);
    wl_list_insert(&x->windows, &w->link);
    return w;
}

static void handle_create(struct xwm *x, const unsigned char *pkt) {
    uint32_t xid = (uint32_t)pkt[8] | (uint32_t)pkt[9] << 8 |
        (uint32_t)pkt[10] << 16 | (uint32_t)pkt[11] << 24;
    if (window_for(x, xid)) return;
    int wx = (int16_t)(pkt[12] | pkt[13] << 8);
    int wy = (int16_t)(pkt[14] | pkt[15] << 8);
    int width = (uint16_t)(pkt[16] | pkt[17] << 8);
    int height = (uint16_t)(pkt[18] | pkt[19] << 8);
    bool override = pkt[22] != 0;
    struct xwm_window *w = window_new(x, xid, override, wx, wy, width, height);
    if (!w) return;
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
    schedule_scene(x->server);
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
    if (!w->override) {
        set_wm_state(w, 1);
        refresh_client_list(x);
    } else if (w->node) {
        node_set_enabled(w->node, w->surface && w->surface->mapped);
        node_raise_to_top(w->node);
    }
    schedule_scene(x->server);
}

static void handle_unmap_notify(struct xwm *x, const unsigned char *pkt) {
    uint32_t xid = (uint32_t)pkt[8] | (uint32_t)pkt[9] << 8 |
        (uint32_t)pkt[10] << 16 | (uint32_t)pkt[11] << 24;
    struct xwm_window *w = window_for(x, xid);
    if (!w) return;
    w->x_mapped = false;
    if (!w->override) {
        set_wm_state(w, 3);
        refresh_client_list(x);
    } else if (w->node) {
        node_set_enabled(w->node, false);
    }
    schedule_scene(x->server);
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
    if (w->base) {
        w->base->geometry.width = w->width;
        w->base->geometry.height = w->height;
    }
    if (w->override) override_place(w);
    schedule_scene(x->server);
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
    if (!w->override) {
        unsigned char event[32] = {0};
        event[0] = 22;
        for (int j = 0; j < 4; j++) {
            event[4 + j] = (unsigned char)(xid >> (8 * j));
            event[8 + j] = (unsigned char)(xid >> (8 * j));
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
    if (mask & CFG_SIBLING) node_raise_to_top(w->node);
    if (w->node && w->x_mapped) node_raise_to_top(w->node);
}

static bool transfer_incr(struct xwm *x, uint32_t window, uint32_t property, uint8_t state);

static void handle_property_notify(struct xwm *x, const unsigned char *pkt) {
    uint32_t xid = (uint32_t)pkt[4] | (uint32_t)pkt[5] << 8 |
        (uint32_t)pkt[6] << 16 | (uint32_t)pkt[7] << 24;
    uint32_t atom = (uint32_t)pkt[8] | (uint32_t)pkt[9] << 8 |
        (uint32_t)pkt[10] << 16 | (uint32_t)pkt[11] << 24;
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
    if (!w->toplevel) return;
    bool *value = NULL;
    if (atom == lookup_atom(w->xwm, A_NET_WM_STATE_FULLSCREEN)) value = &w->fullscreen;
    else if (atom == lookup_atom(w->xwm, A_NET_WM_STATE_MAXIMIZED_VERT) ||
            atom == lookup_atom(w->xwm, A_NET_WM_STATE_MAXIMIZED_HORZ)) value = &w->maximized;
    else return;
    bool state = action == 1 ? true : action == 2 ? !*value : false;
    *value = state;
    if (atom == lookup_atom(w->xwm, A_NET_WM_STATE_FULLSCREEN)) {
        w->toplevel->requested.fullscreen = state;
        wl_signal_emit_mutable(&w->toplevel->events.request_fullscreen, NULL);
    } else {
        w->toplevel->requested.maximized = state;
        wl_signal_emit_mutable(&w->toplevel->events.request_maximize, NULL);
    }
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
        xwayland_pair_window(x->server, xid, ((uint64_t)data[1] << 32) | data[0]);
    } else if (type == lookup_atom(x, A_WL_SURFACE_ID)) {
        return;
    } else if (type == lookup_atom(x, A_NET_WM_STATE)) {
        uint32_t action = data[0];
        request_state(w, action, data[1]);
        request_state(w, action, data[2]);
    } else if (type == lookup_atom(x, A_NET_ACTIVE_WINDOW)) {
        if (w->window) window_external_activate(w->window);
    } else if (type == lookup_atom(x, A_NET_CLOSE_WINDOW)) {
        if (w->toplevel) xwm_close(w->toplevel);
    } else if (type == lookup_atom(x, A_WM_CHANGE_STATE)) {
        if (data[0] == 3 && w->toplevel) {
            w->minimized = true;
            wl_signal_emit_mutable(&w->toplevel->events.request_minimize, NULL);
        }
    }
}

void xwm_configure(struct xdg_toplevel *toplevel, int width, int height) {
    struct xwm_window *w = toplevel->xwm;
    if (!w || !w->xid) return;
    toplevel->scheduled.width = width;
    toplevel->scheduled.height = height;
    if (width <= 0 || height <= 0) return;
    uint32_t values[7] = {0};
    values[2] = (uint32_t)width;
    values[3] = (uint32_t)height;
    configure_window(w->xwm, w->xid, CFG_WIDTH | CFG_HEIGHT, values);
}

void xwm_schedule(struct xdg_toplevel *toplevel) {
    (void)toplevel;
}

void xwm_activate(struct xdg_toplevel *toplevel, bool activated) {
    struct xwm_window *w = toplevel->xwm;
    if (!w) return;
    toplevel->scheduled.activated = activated;
    if (!activated) return;
    if (!(w->has_input_hint && !w->input_hint)) set_focus(w->xwm, w->xid);
    if (w->take_focus) {
        uint32_t data[5] = { lookup_atom(w->xwm, A_WM_TAKE_FOCUS), X11_CURRENT_TIME, 0, 0, 0 };
        client_message(w->xwm, w->xid, lookup_atom(w->xwm, A_WM_PROTOCOLS), 0, data);
    }
    w->xwm->active = w->xid;
    refresh_active(w->xwm);
}

static void state_atom(struct xwm_window *w, uint32_t atom, bool enabled) {
    uint32_t data[5] = { enabled ? 1u : 0u, atom, 0, 2, 0 };
    client_message(w->xwm, w->xid, lookup_atom(w->xwm, A_NET_WM_STATE),
        MASK_SUBSTRUCTURE_REDIRECT | MASK_SUBSTRUCTURE_NOTIFY, data);
}

void xwm_state(struct xdg_toplevel *toplevel, int fullscreen, int maximized) {
    struct xwm_window *w = toplevel->xwm;
    if (!w) return;
    if (!!fullscreen != toplevel->scheduled.fullscreen)
        state_atom(w, lookup_atom(w->xwm, A_NET_WM_STATE_FULLSCREEN), fullscreen != 0);
    if (!!maximized != toplevel->scheduled.maximized) {
        state_atom(w, lookup_atom(w->xwm, A_NET_WM_STATE_MAXIMIZED_VERT), maximized != 0);
        state_atom(w, lookup_atom(w->xwm, A_NET_WM_STATE_MAXIMIZED_HORZ), maximized != 0);
    }
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
    if (w->delete_window) {
        uint32_t data[5] = { lookup_atom(w->xwm, A_WM_DELETE_WINDOW), X11_CURRENT_TIME, 0, 0, 0 };
        client_message(w->xwm, w->xid, lookup_atom(w->xwm, A_WM_PROTOCOLS), 0, data);
    } else
        kill_client(w->xwm, w->xid);
}

void xwm_wayland_selection(struct tomoe *s, bool primary, bool present) {
    struct xwm *x = s->xwm;
    if (!x || !x->ready) return;
    bool x11 = false;
    const char *const *mimes = NULL;
    size_t count = 0;
    bool has = selection_source_info(s, primary, &x11, &mimes, &count);
    (void)mimes;
    (void)count;
    if (has && x11) return;
    uint32_t selection = lookup_atom(x, primary ? A_PRIMARY : A_CLIPBOARD);
    uint32_t window = primary ? x->primary_window : x->clipboard_window;
    int slot = primary ? 1 : 0;
    if (has && !x->own[slot]) {
        set_owner(x, window, selection, X11_CURRENT_TIME);
        x->own[slot] = true;
    } else if (!has && x->own[slot]) {
        set_owner(x, X11_NONE, selection, X11_CURRENT_TIME);
        x->own[slot] = false;
    }
}

enum { SETUP_NONE, SETUP_WAIT, SETUP_ATOMS, SETUP_READY };

static void setup_finish(struct xwm *x) {
    x->convert_property = atom_intern(x, "_TOMOE_CONVERT", false);
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
    int opcode = 0, event = 0;
    if (query_extension(x, "Composite", &opcode, &event)) {
        uint32_t version[2] = { 0, 4 };
        extension_request(x, opcode, 0, version, 2);
        uint32_t redirect[1] = { x->root };
        struct req r = {0};
        r_begin(&r, (uint8_t)opcode);
        r_flag(&r, 1, 2);
        r_u32(&r, redirect[0]);
        r_u8(&r, 1);
        r_u8(&r, 0); r_u8(&r, 0); r_u8(&r, 0);
        r_send(x, &r);
    }
    if (query_extension(x, "XFIXES", &opcode, &event)) {
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
    set_owner(x, x->wm_window, lookup_atom(x, A_WM_S0), X11_CURRENT_TIME);
    for (int primary = 0; primary < 2; primary++) {
        bool x11 = false;
        const char *const *mimes = NULL;
        size_t count = 0;
        if (!selection_source_info(x->server, primary != 0, &x11, &mimes, &count) || x11)
            continue;
        set_owner(x, primary ? x->primary_window : x->clipboard_window,
            lookup_atom(x, primary ? A_PRIMARY : A_CLIPBOARD), X11_CURRENT_TIME);
        x->own[primary] = true;
    }
    x->ready = true;
    x->setup_state = SETUP_READY;
    tomoe_log(LOG_DEBUG, "tomoe: Xwayland window manager ready on root %u", x->root);
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
        x->failed = true;
        return;
    }
    x->rid_base = (uint32_t)reply[12] | (uint32_t)reply[13] << 8 |
        (uint32_t)reply[14] << 16 | (uint32_t)reply[15] << 24;
    x->rid_mask = (uint32_t)reply[16] | (uint32_t)reply[17] << 8 |
        (uint32_t)reply[18] << 16 | (uint32_t)reply[19] << 24;
    x->max_request = ((uint32_t)reply[26] | (uint32_t)reply[27] << 8) * 4;
    uint16_t vendor = (uint16_t)(reply[24] | reply[25] << 8);
    int formats = reply[29];
    size_t offset = 40 + (((size_t)vendor + 3) & ~(size_t)3) + (size_t)formats * 8;
    if (offset + 40 <= length) {
        const unsigned char *screen = reply + offset;
        x->root = (uint32_t)screen[0] | (uint32_t)screen[1] << 8 |
            (uint32_t)screen[2] << 16 | (uint32_t)screen[3] << 24;
        x->root_width = (uint16_t)(screen[20] | screen[21] << 8);
        x->root_height = (uint16_t)(screen[22] | screen[23] << 8);
    }
    if (!x->root) {
        x->failed = true;
        return;
    }
    x->rid_next = 0;
    setup_atoms(x);
}

static void setup_atom_reply(struct xwm *x, unsigned char *reply, size_t length) {
    uint16_t seq = (uint16_t)(reply[2] | reply[3] << 8);
    if (length < 12) {
        x->failed = true;
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

static void x_handle(struct xwm *x, unsigned char *packet, size_t length);

static int xwm_event(int fd, uint32_t mask, void *data) {
    struct xwm *x = data;
    for (;;) {
        ssize_t n = x_recv(x, 0);
        if (n == -2) break;
        if (n <= 0) {
            xwm_stop(x->server);
            return 0;
        }
        x->in_len += (size_t)n;
    }
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
    while (!x->failed && !wl_list_empty(&x->deferred)) {
        struct deferred *entry = wl_container_of(x->deferred.next, entry, link);
        wl_list_remove(&entry->link);
        x_handle(x, entry->data, entry->len);
        free(entry);
    }
    if (x->failed) xwm_stop(x->server);
    return 0;
}

static void xwm_setup_request(void *data) {
    struct xwm *x = data;
    unsigned char request[12] = { 'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    x_write(x, request, sizeof(request));
    x->setup_state = SETUP_WAIT;
}

bool xwm_start(struct tomoe *s, int fd) {
    if (s->xwm) xwm_stop(s);
    if (fd < 0) return false;
    struct xwm *x = calloc(1, sizeof(*x));
    if (!x) return false;
    x->server = s;
    x->fd = fd;
    wl_list_init(&x->deferred);
    wl_list_init(&x->windows);
    wl_list_init(&x->writers);
    wl_list_init(&x->transfers);
    wl_list_init(&x->props);
    s->xwm = x;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    x->source = wl_event_loop_add_fd(wl_display_get_event_loop(s->display), fd,
        WL_EVENT_READABLE, xwm_event, x);
    if (!x->source) {
        s->xwm = NULL;
        free(x->in);
        free(x);
        return false;
    }
    if (!wl_event_loop_add_idle(wl_display_get_event_loop(s->display), xwm_setup_request, x)) {
        xwm_stop(s);
        return false;
    }
    return true;
}

void xwm_stop(struct tomoe *s) {
    struct xwm *x = s ? s->xwm : NULL;
    if (!x) return;
    x->stopping = true;
    if (x->source) wl_event_source_remove(x->source);
    struct xwm_window *w, *next;
    wl_list_for_each_safe(w, next, &x->windows, link) window_free(w);
    struct deferred *entry, *next_entry;
    wl_list_for_each_safe(entry, next_entry, &x->deferred, link) {
        wl_list_remove(&entry->link);
        free(entry);
    }
    free(x->in);
    free(x);
    s->xwm = NULL;
}

void xwm_client_gone(struct tomoe *s) {
    xwm_stop(s);
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
    struct wl_list link;
    int fd;
    struct wl_event_source *source;
    unsigned char *data;
    size_t len, offset;
};

static int writer_ready(int fd, uint32_t mask, void *data) {
    struct writer *writer = data;
    while (writer->offset < writer->len) {
        ssize_t n = write(writer->fd, writer->data + writer->offset,
            writer->len - writer->offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return 0;
        if (n <= 0) break;
        writer->offset += (size_t)n;
    }
    wl_event_source_remove(writer->source);
    close(writer->fd);
    wl_list_remove(&writer->link);
    free(writer->data);
    free(writer);
    return 0;
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
    writer->fd = fd;
    writer->data = data;
    writer->len = len;
    writer->offset = offset;
    writer->source = wl_event_loop_add_fd(
        wl_display_get_event_loop(x->server->display), fd, WL_EVENT_WRITABLE,
        writer_ready, writer);
    if (!writer->source) {
        close(fd);
        free(data);
        free(writer);
        return;
    }
    wl_list_insert(&x->writers, &writer->link);
}

static uint32_t mime_atom(struct xwm *x, const char *mime) {
    if (strcmp(mime, "text/plain;charset=utf-8") == 0) return lookup_atom(x, A_UTF8_STRING);
    if (strcmp(mime, "text/plain") == 0) return lookup_atom(x, A_TEXT);
    return atom_intern(x, mime, false);
}

static char *atom_mime(struct xwm *x, uint32_t atom) {
    if (atom == lookup_atom(x, A_UTF8_STRING)) return strdup("text/plain;charset=utf-8");
    if (atom == lookup_atom(x, A_TEXT) || atom == XA_STRING) return strdup("text/plain");
    struct req r = {0};
    r_begin(&r, 17);
    r_u32(&r, atom);
    uint16_t seq = r_send(x, &r);
    size_t len = 0;
    int error = 0;
    unsigned char *reply = x_wait(x, seq, &len, &error);
    if (!reply || len < 32) {
        free(reply);
        return NULL;
    }
    size_t name_len = (uint16_t)(reply[8] | reply[9] << 8);
    if (name_len > len - 32) name_len = len - 32;
    char *name = malloc(name_len + 1);
    if (!name) {
        free(reply);
        return NULL;
    }
    memcpy(name, reply + 32, name_len);
    name[name_len] = 0;
    free(reply);
    if (!strchr(name, '/')) {
        free(name);
        return NULL;
    }
    return name;
}

static unsigned char *convert_from_x(struct xwm *x, uint32_t window, uint32_t selection,
        uint32_t target, size_t *length, uint32_t *type) {
    convert_selection(x, window, selection, target, x->convert_property, X11_CURRENT_TIME);
    for (;;) {
        size_t len = 0;
        unsigned char *packet = x_packet(x, &len);
        if ((packet[0] & 0x7f) == 31) {
            uint32_t requestor = (uint32_t)packet[8] | (uint32_t)packet[9] << 8 |
                (uint32_t)packet[10] << 16 | (uint32_t)packet[11] << 24;
            uint32_t property = (uint32_t)packet[20] | (uint32_t)packet[21] << 8 |
                (uint32_t)packet[22] << 16 | (uint32_t)packet[23] << 24;
            if (requestor == window && property == x->convert_property) {
                free(packet);
                break;
            }
            if (requestor == window && property == X11_NONE) {
                free(packet);
                return NULL;
            }
        }
        defer(x, packet, len);
    }
    struct property property;
    if (!read_property_all(x, window, x->convert_property, &property) ||
            property.type == X11_NONE) {
        property_finish(&property);
        return NULL;
    }
    if (property.type == lookup_atom(x, A_INCR)) {
        uint32_t hint = property.length >= 4 ? ((uint32_t *)property.data)[0] : 0;
        property_finish(&property);
        (void)hint;
        delete_property(x, window, x->convert_property);
        unsigned char *data = NULL;
        size_t total = 0, capacity = 0;
        for (;;) {
            unsigned char *notify = NULL;
            for (;;) {
                size_t len = 0;
                unsigned char *packet = x_packet(x, &len);
                if (!packet) {
                    free(data);
                    return NULL;
                }
                if ((packet[0] & 0x7f) == 28) {
                    uint32_t w = (uint32_t)packet[4] | (uint32_t)packet[5] << 8 |
                        (uint32_t)packet[6] << 16 | (uint32_t)packet[7] << 24;
                    uint32_t atom = (uint32_t)packet[8] | (uint32_t)packet[9] << 8 |
                        (uint32_t)packet[10] << 16 | (uint32_t)packet[11] << 24;
                    if (w == window && atom == x->convert_property && packet[16] == 0) {
                        free(packet);
                        notify = (unsigned char *)1;
                        break;
                    }
                    if (w == window && atom == x->convert_property) {
                        free(packet);
                        continue;
                    }
                }
                defer(x, packet, len);
            }
            if (!notify) {
                free(data);
                return NULL;
            }
            struct property part;
            if (!read_property_all(x, window, x->convert_property, &part) ||
                    part.type == X11_NONE) {
                property_finish(&part);
                free(data);
                return NULL;
            }
            if (!part.length) {
                property_finish(&part);
                delete_property(x, window, x->convert_property);
                break;
            }
            if (total + part.length > capacity) {
                size_t cap = capacity ? capacity : 65536;
                while (cap < total + part.length) cap *= 2;
                unsigned char *grown = realloc(data, cap);
                if (!grown) {
                    property_finish(&part);
                    free(data);
                    return NULL;
                }
                data = grown;
                capacity = cap;
            }
            memcpy(data + total, part.data, part.length);
            total += part.length;
            property_finish(&part);
            delete_property(x, window, x->convert_property);
        }
        *length = total;
        *type = XA_STRING;
        return data;
    }
    *length = property.length;
    *type = property.type;
    delete_property(x, window, x->convert_property);
    return property.data;
}

void xwm_source_send(struct tomoe *s, bool primary, const char *mime, int fd) {
    struct xwm *x = s->xwm;
    if (!x || !x->ready) {
        close(fd);
        return;
    }
    uint32_t selection = lookup_atom(x, primary ? A_PRIMARY : A_CLIPBOARD);
    uint32_t window = primary ? x->primary_window : x->clipboard_window;
    uint32_t target = mime_atom(x, mime);
    size_t length = 0;
    uint32_t type = 0;
    unsigned char *data = convert_from_x(x, window, selection, target, &length, &type);
    if (!data && length) return;
    (void)type;
    write_async(x, fd, data ? data : (unsigned char *)strdup(""), data ? length : 0);
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
    const char *const *mimes = NULL;
    size_t count = 0;
    bool x11 = false;
    if (!selection_source_info(x->server, primary, &x11, &mimes, &count) || x11) {
        selection_notify(x, requestor, selection, lookup_atom(x, A_TARGETS), X11_NONE, time);
        return;
    }
    uint32_t atoms[64];
    size_t used = 0;
    atoms[used++] = lookup_atom(x, A_TARGETS);
    atoms[used++] = lookup_atom(x, A_TIMESTAMP);
    for (size_t i = 0; i < count && used < 64; i++) {
        uint32_t atom = mime_atom(x, mimes[i]);
        bool duplicate = false;
        for (size_t j = 0; j < used; j++) if (atoms[j] == atom) duplicate = true;
        if (!duplicate) atoms[used++] = atom;
        if (strcmp(mimes[i], "text/plain") == 0) {
            bool has_string = false;
            for (size_t j = 0; j < used; j++) if (atoms[j] == XA_STRING) has_string = true;
            if (!has_string && used < 64) atoms[used++] = XA_STRING;
        }
    }
    change_property(x, 0, requestor, property, XA_ATOM, 32, atoms, (uint32_t)used);
    selection_notify(x, requestor, selection, lookup_atom(x, A_TARGETS), property, time);
}

struct transfer {
    struct wl_list link;
    struct xwm *x;
    bool primary;
    int fd;
    struct wl_event_source *source;
    unsigned char *data;
    size_t len, cap;
    uint32_t requestor, property, target, time;
    bool incr;
    size_t offset;
};

static void transfer_free(struct transfer *transfer) {
    if (transfer->source) wl_event_source_remove(transfer->source);
    if (transfer->fd >= 0) close(transfer->fd);
    wl_list_remove(&transfer->link);
    free(transfer->data);
    free(transfer);
}

static int transfer_read(int fd, uint32_t mask, void *data) {
    struct transfer *transfer = data;
    for (;;) {
        if (transfer->cap - transfer->len < 4096) {
            size_t cap = transfer->cap ? transfer->cap * 2 : 65536;
            unsigned char *grown = realloc(transfer->data, cap);
            if (!grown) {
                transfer_free(transfer);
                return 0;
            }
            transfer->data = grown;
            transfer->cap = cap;
        }
        ssize_t n = read(transfer->fd, transfer->data + transfer->len,
            transfer->cap - transfer->len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return 0;
        if (n < 0) {
            selection_notify(transfer->x, transfer->requestor, transfer->primary ?
                lookup_atom(transfer->x, A_PRIMARY) : lookup_atom(transfer->x, A_CLIPBOARD),
                transfer->target, X11_NONE, transfer->time);
            transfer_free(transfer);
            return 0;
        }
        if (n == 0) break;
        transfer->len += (size_t)n;
    }
    wl_event_source_remove(transfer->source);
    transfer->source = NULL;
    close(transfer->fd);
    transfer->fd = -1;
    uint32_t selection = transfer->primary ? lookup_atom(transfer->x, A_PRIMARY) :
        lookup_atom(transfer->x, A_CLIPBOARD);
    if (transfer->len > transfer->x->max_request - 128) {
        set_event_mask(transfer->x, transfer->requestor, MASK_PROPERTY_CHANGE);
        uint32_t size = (uint32_t)transfer->len;
        change_property(transfer->x, 0, transfer->requestor, transfer->property,
            lookup_atom(transfer->x, A_INCR), 32, &size, 1);
        transfer->incr = true;
        transfer->offset = 0;
        selection_notify(transfer->x, transfer->requestor, selection, transfer->target,
            transfer->property, transfer->time);
        return 0;
    }
    change_property(transfer->x, 0, transfer->requestor, transfer->property,
        transfer->target, 8, transfer->data, (uint32_t)transfer->len);
    selection_notify(transfer->x, transfer->requestor, selection, transfer->target,
        transfer->property, transfer->time);
    transfer_free(transfer);
    return 0;
}

static bool transfer_incr(struct xwm *x, uint32_t window, uint32_t property, uint8_t state) {
    struct transfer *transfer;
    wl_list_for_each(transfer, &x->transfers, link) {
        if (!transfer->incr || transfer->requestor != window ||
                transfer->property != property || state != 1) continue;
        size_t chunk = transfer->len - transfer->offset;
        if (chunk > INCR_CHUNK) chunk = INCR_CHUNK;
        change_property(x, 2, window, property, transfer->target, 8,
            transfer->data + transfer->offset, (uint32_t)chunk);
        transfer->offset += chunk;
        if (transfer->offset == transfer->len && chunk == 0) transfer_free(transfer);
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
    if (!property) property = target;
    bool primary = selection == lookup_atom(x, A_PRIMARY);
    if (!primary && selection != lookup_atom(x, A_CLIPBOARD)) {
        selection_notify(x, requestor, selection, target, X11_NONE, time);
        return;
    }
    if (target == lookup_atom(x, A_TARGETS)) {
        serve_targets(x, primary, pkt);
        return;
    }
    if (target == lookup_atom(x, A_TIMESTAMP)) {
        uint32_t stamp = time;
        change_property(x, 0, requestor, property, XA_INTEGER, 32, &stamp, 1);
        selection_notify(x, requestor, selection, target, property, time);
        return;
    }
    char *mime = atom_mime(x, target);
    bool x11 = false;
    const char *const *mimes = NULL;
    size_t count = 0;
    if (!mime || !selection_source_info(x->server, primary, &x11, &mimes, &count) || x11) {
        free(mime);
        selection_notify(x, requestor, selection, target, X11_NONE, time);
        return;
    }
    bool supported = false;
    for (size_t i = 0; i < count; i++)
        if (strcmp(mimes[i], mime) == 0) {
            supported = true;
            break;
        }
    if (!supported) {
        free(mime);
        selection_notify(x, requestor, selection, target, X11_NONE, time);
        return;
    }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) {
        free(mime);
        selection_notify(x, requestor, selection, target, X11_NONE, time);
        return;
    }
    if (!selection_source_send(x->server, primary, mime, pipefd[1])) {
        close(pipefd[0]);
        free(mime);
        selection_notify(x, requestor, selection, target, X11_NONE, time);
        return;
    }
    free(mime);
    struct transfer *transfer = calloc(1, sizeof(*transfer));
    if (!transfer) {
        close(pipefd[0]);
        return;
    }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    transfer->x = x;
    transfer->primary = primary;
    transfer->fd = pipefd[0];
    transfer->requestor = requestor;
    transfer->property = property;
    transfer->target = target;
    transfer->time = time;
    transfer->source = wl_event_loop_add_fd(wl_display_get_event_loop(x->server->display),
        pipefd[0], WL_EVENT_READABLE, transfer_read, transfer);
    if (!transfer->source) {
        transfer_free(transfer);
        return;
    }
    wl_list_insert(&x->transfers, &transfer->link);
}

static void adopt_selection(struct xwm *x, bool primary) {
    uint32_t selection = lookup_atom(x, primary ? A_PRIMARY : A_CLIPBOARD);
    uint32_t window = primary ? x->primary_window : x->clipboard_window;
    size_t length = 0;
    uint32_t type = 0;
    unsigned char *data = convert_from_x(x, window, selection, lookup_atom(x, A_TARGETS),
        &length, &type);
    char *mimes[64];
    size_t count = 0;
    if (data && type == XA_ATOM) {
        size_t atoms = length / 4;
        const uint32_t *values = (const uint32_t *)data;
        for (size_t i = 0; i < atoms && count < 64; i++) {
            if (values[i] == lookup_atom(x, A_TARGETS) || values[i] == lookup_atom(x, A_TIMESTAMP) ||
                    values[i] == lookup_atom(x, A_INCR)) continue;
            char *mime = atom_mime(x, values[i]);
            if (!mime) continue;
            bool duplicate = false;
            for (size_t j = 0; j < count; j++)
                if (strcmp(mimes[j], mime) == 0) duplicate = true;
            if (duplicate) free(mime);
            else mimes[count++] = mime;
        }
    }
    free(data);
    if (!count) {
        mimes[count++] = strdup("text/plain;charset=utf-8");
        mimes[count++] = strdup("text/plain");
    }
    selection_x11_offer(x->server, primary, (const char *const *)mimes, count);
    for (size_t i = 0; i < count; i++) free(mimes[i]);
}

static void handle_xfixes(struct xwm *x, const unsigned char *pkt) {
    uint32_t window = (uint32_t)pkt[4] | (uint32_t)pkt[5] << 8 |
        (uint32_t)pkt[6] << 16 | (uint32_t)pkt[7] << 24;
    uint32_t owner = (uint32_t)pkt[8] | (uint32_t)pkt[9] << 8 |
        (uint32_t)pkt[10] << 16 | (uint32_t)pkt[11] << 24;
    uint32_t selection = (uint32_t)pkt[12] | (uint32_t)pkt[13] << 8 |
        (uint32_t)pkt[14] << 16 | (uint32_t)pkt[15] << 24;
    (void)window;
    bool primary;
    if (selection == lookup_atom(x, A_PRIMARY)) primary = true;
    else if (selection == lookup_atom(x, A_CLIPBOARD)) primary = false;
    else return;
    uint32_t ours = primary ? x->primary_window : x->clipboard_window;
    int slot = primary ? 1 : 0;
    if (owner == ours) {
        x->own[slot] = true;
        return;
    }
    x->own[slot] = false;
    if (!owner) {
        selection_x11_clear(x->server, primary);
        return;
    }
    adopt_selection(x, primary);
}

static void x_handle(struct xwm *x, unsigned char *packet, size_t length) {
    uint8_t type = packet[0] & 0x7f;
    if (type == 0) {
        if (log_verbosity >= LOG_DEBUG)
            tomoe_log(LOG_DEBUG, "tomoe: X error %u on request %u.%u",
                packet[1], packet[10], (unsigned)packet[8] | (unsigned)packet[9] << 8);
        uint16_t seq = (uint16_t)(packet[2] | packet[3] << 8);
        struct prop_request *request, *next_request;
        wl_list_for_each_safe(request, next_request, &x->props, link) {
            if (request->seq != seq) continue;
            wl_list_remove(&request->link);
            free(request);
            break;
        }
        return;
    }
    if (type == 1) {
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
    case 33: handle_client_message(x, packet); break;
    default: break;
    }
    (void)length;
}
