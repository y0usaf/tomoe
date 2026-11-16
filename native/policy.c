#include "internal.h"
#include "budget.h"
#include "json.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static struct tomoe *policy_server;

static struct tomoe *server(void) {
    return policy_server;
}

static lisp_value put(lisp_value list, const char *key, lisp_value value) {
    return lisp_cons(value, lisp_cons(lisp_keyword(key), list));
}

static lisp_value boolean(bool value) {
    return lisp_bool(value);
}

static lisp_value text_of(struct tomoe *s, uint32_t handle, const char *fallback) {
    size_t length = 0;
    const char *text = policy_event_string_at(s, handle, &length);
    return text ? lisp_string(text, length) : lisp_cstring(fallback ? fallback : "");
}

static lisp_value optional_text(struct tomoe *s, uint32_t handle) {
    size_t length = 0;
    const char *text = policy_event_string_at(s, handle, &length);
    return text ? lisp_string(text, length) : lisp_nil();
}

static bool text_copy(struct tomoe *s, uint32_t handle, char *buffer, size_t size) {
    size_t length = 0;
    const char *text = policy_event_string_at(s, handle, &length);
    if (!text || length >= size) return false;
    memcpy(buffer, text, length);
    buffer[length] = 0;
    return true;
}

static const char *const policy_keys[] = {
    "windows", "window-geometry", "rules", "data", "services", "outputs",
    "connectors", "output-config", "output-errors", "config-error", "workareas",
    "view", "layout", "stacking", "focus", "bindings", "commands", "keyboard",
    "settings", "layers", "surfaces", "command", "button", "pointer", "grab",
    "request", "screenshot", "screencast", "ipc", "ui", "activity", "system",
    "cursors"
};

enum policy_key_index {
    PK_WINDOWS, PK_WINDOW_GEOMETRY, PK_RULES, PK_DATA, PK_SERVICES, PK_OUTPUTS,
    PK_CONNECTORS, PK_OUTPUT_CONFIG, PK_OUTPUT_ERRORS, PK_CONFIG_ERROR, PK_WORKAREAS,
    PK_VIEW, PK_LAYOUT, PK_STACKING, PK_FOCUS, PK_BINDINGS, PK_COMMANDS, PK_KEYBOARD,
    PK_SETTINGS, PK_LAYERS, PK_SURFACES, PK_COMMAND, PK_BUTTON, PK_POINTER, PK_GRAB,
    PK_REQUEST, PK_SCREENSHOT, PK_SCREENCAST, PK_IPC, PK_UI, PK_ACTIVITY, PK_SYSTEM,
    PK_CURSORS, POLICY_KEY_COUNT
};

enum { POLICY_ALWAYS = -1, POLICY_TRANSPORT = -2 };

static const struct event_policy {
    int16_t queue;
    int16_t deliver;
    uint16_t flags;
} event_policies[EV_TYPE_COUNT] = {
    [EV_MAP] = { POLICY_ALWAYS, POLICY_ALWAYS, EVF_FACT },
    [EV_METADATA] = { POLICY_ALWAYS, POLICY_ALWAYS, EVF_FACT },
    [EV_BUFFER] = { POLICY_ALWAYS, POLICY_ALWAYS, EVF_FACT },
    [EV_GEOMETRY] = { POLICY_ALWAYS, POLICY_ALWAYS, EVF_FACT },
    [EV_LAYER] = { POLICY_ALWAYS, POLICY_ALWAYS, EVF_FACT },
    [EV_UNMAP] = { POLICY_ALWAYS, POLICY_ALWAYS, EVF_FACT },
    [EV_OUTPUTS] = { POLICY_ALWAYS, POLICY_ALWAYS, EVF_FACT },
    [EV_KEY] = { POLICY_ALWAYS, POLICY_ALWAYS, 0 },
    [EV_UI] = { POLICY_ALWAYS, POLICY_ALWAYS, 0 },
    [EV_BUTTON] = { PK_BUTTON, PK_BUTTON, EVF_OPTIONAL },
    [EV_POINTER] = { PK_POINTER, PK_POINTER, EVF_OPTIONAL },
    [EV_GRAB] = { PK_GRAB, PK_GRAB, EVF_COALESCE },
    [EV_ACTIVITY] = { POLICY_ALWAYS, PK_ACTIVITY, EVF_OPTIONAL },
    [EV_INPUT] = { POLICY_ALWAYS, POLICY_TRANSPORT, EVF_COALESCE | EVF_OPTIONAL },
    [EV_REQUEST] = { PK_REQUEST, PK_REQUEST, EVF_OPTIONAL },
    [EV_SCREENSHOT] = { PK_SCREENSHOT, PK_SCREENSHOT, EVF_OPTIONAL },
    [EV_SCREENCAST] = { PK_SCREENCAST, PK_SCREENCAST, EVF_OPTIONAL },
    [EV_TIMER] = { POLICY_ALWAYS, POLICY_ALWAYS, 0 },
    [EV_WATCH] = { POLICY_ALWAYS, POLICY_ALWAYS, 0 },
    [EV_EXEC] = { POLICY_ALWAYS, POLICY_ALWAYS, 0 },
    [EV_PROCESS] = { POLICY_ALWAYS, POLICY_ALWAYS, 0 },
    [EV_SERVICES] = { POLICY_ALWAYS, POLICY_ALWAYS, 0 },
    [EV_SYSTEM] = { PK_SYSTEM, PK_SYSTEM, 0 },
    [EV_SOURCE] = { POLICY_ALWAYS, POLICY_ALWAYS, 0 },
    [EV_TEXT] = { POLICY_ALWAYS, POLICY_TRANSPORT, EVF_OPTIONAL },
    [EV_SELECTOR] = { PK_SCREENSHOT, PK_SCREENSHOT, 0 }
};

static bool key_read(struct tomoe *s, int key) {
    if (key == POLICY_ALWAYS) return true;
    if (key < 0) return false;
    return ((s->event_read_mask >> (unsigned)key) & 1) != 0;
}

bool policy_reads(struct tomoe *s, enum tomoe_event_type type) {
    if (!s || type >= EV_TYPE_COUNT) return false;
    return key_read(s, event_policies[type].queue);
}

bool policy_event_delivers(struct tomoe *s, enum tomoe_event_type type) {
    if (!s || type >= EV_TYPE_COUNT) return false;
    return key_read(s, event_policies[type].deliver);
}

uint16_t policy_event_flags(enum tomoe_event_type type) {
    return type < EV_TYPE_COUNT ? event_policies[type].flags : 0;
}

static const char *window_content_type(uint32_t kind) {
    static const char *const types[] = { "none", "photo", "video", "game" };
    return kind < sizeof(types) / sizeof(types[0]) ? types[kind] : "none";
}

static lisp_value convert_window(struct tomoe *s, const struct tomoe_event *e) {
    lisp_value list = lisp_nil();
    list = put(list, "TYPE", lisp_keyword(e->type == EV_MAP ? "map" : "metadata"));
    list = put(list, "ID", lisp_uint(e->id));
    list = put(list, "WIDTH", lisp_int((int32_t)e->x));
    list = put(list, "HEIGHT", lisp_int((int32_t)e->y));
    list = put(list, "CLIENT-WIDTH", lisp_int((int32_t)e->dx));
    list = put(list, "CLIENT-HEIGHT", lisp_int((int32_t)e->dy));
    list = put(list, "BUFFERED", boolean(e->state & POLICY_WINDOW_BUFFERED));
    list = put(list, "TITLE", text_of(s, e->str[0], ""));
    list = put(list, "APP-ID", text_of(s, e->str[1], ""));
    list = put(list, "PID", lisp_int((int32_t)e->button));
    list = put(list, "CONTENT-TYPE", lisp_keyword(window_content_type(e->keycode)));
    list = put(list, "TAG", text_of(s, e->str[2], ""));
    list = put(list, "DESCRIPTION", text_of(s, e->str[3], ""));
    list = put(list, "PARENT", e->modifiers ? lisp_uint(e->modifiers) : lisp_nil());
    list = put(list, "DIALOG", boolean(e->state & POLICY_WINDOW_DIALOG));
    list = put(list, "MODAL", boolean(e->state & POLICY_WINDOW_MODAL));
    list = put(list, "FULLSCREEN", boolean(e->state & POLICY_WINDOW_FULLSCREEN));
    list = put(list, "MAXIMIZE", boolean(e->state & POLICY_WINDOW_MAXIMIZE));
    list = put(list, "FULLSCREEN-REQUESTED",
        boolean(e->state & POLICY_WINDOW_FULLSCREEN_REQUESTED));
    list = put(list, "MAXIMIZE-REQUESTED",
        boolean(e->state & POLICY_WINDOW_MAXIMIZE_REQUESTED));
    uint32_t request = (uint32_t)e->source_id;
    if (request != EVENT_NO_STRING) {
        char name[64];
        if (text_copy(s, request, name, sizeof(name))) {
            list = put(list, "REQUEST", lisp_keyword(name));
            if (strcmp(name, "fullscreen") == 0)
                list = put(list, "REQUESTED",
                    boolean(e->state & POLICY_WINDOW_FULLSCREEN_REQUESTED));
            else if (strcmp(name, "maximize") == 0)
                list = put(list, "REQUESTED",
                    boolean(e->state & POLICY_WINDOW_MAXIMIZE_REQUESTED));
        }
    }
    if ((uint32_t)e->binding_or_callback_id != EVENT_NO_STRING)
        list = put(list, "OUTPUT", optional_text(s, (uint32_t)e->binding_or_callback_id));
    return lisp_nreverse(list);
}

static lisp_value convert_buffer(struct tomoe *s, const struct tomoe_event *e) {
    lisp_value list = lisp_nil();
    list = put(list, "TYPE", lisp_keyword("buffer"));
    list = put(list, "ID", lisp_uint(e->id));
    list = put(list, "ATTACHED", boolean(e->state & POLICY_WINDOW_ATTACHED));
    list = put(list, "WIDTH", lisp_int((int32_t)e->x));
    list = put(list, "HEIGHT", lisp_int((int32_t)e->y));
    list = put(list, "TITLE", text_of(s, e->str[0], ""));
    list = put(list, "APP-ID", text_of(s, e->str[1], ""));
    list = put(list, "FULLSCREEN", boolean(e->state & POLICY_WINDOW_FULLSCREEN));
    list = put(list, "MAXIMIZE", boolean(e->state & POLICY_WINDOW_MAXIMIZE));
    list = put(list, "FULLSCREEN-REQUESTED",
        boolean(e->state & POLICY_WINDOW_FULLSCREEN_REQUESTED));
    list = put(list, "MAXIMIZE-REQUESTED",
        boolean(e->state & POLICY_WINDOW_MAXIMIZE_REQUESTED));
    if ((uint32_t)e->binding_or_callback_id != EVENT_NO_STRING)
        list = put(list, "OUTPUT", optional_text(s, (uint32_t)e->binding_or_callback_id));
    return lisp_nreverse(list);
}

static lisp_value convert_geometry(struct tomoe *s, const struct tomoe_event *e) {
    lisp_value list = lisp_nil();
    list = put(list, "TYPE", lisp_keyword("geometry"));
    list = put(list, "ID", lisp_uint(e->id));
    list = put(list, "WIDTH", lisp_int((int32_t)e->x));
    list = put(list, "HEIGHT", lisp_int((int32_t)e->y));
    list = put(list, "CLIENT-WIDTH", lisp_int((int32_t)e->dx));
    list = put(list, "CLIENT-HEIGHT", lisp_int((int32_t)e->dy));
    return lisp_nreverse(list);
}

static lisp_value layer_name(int32_t layer) {
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return lisp_keyword("background");
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return lisp_keyword("bottom");
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return lisp_keyword("top");
    default: return lisp_keyword("overlay");
    }
}

static lisp_value keyboard_name(int32_t keyboard) {
    switch (keyboard) {
    case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE: return lisp_keyword("none");
    case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE:
        return lisp_keyword("exclusive");
    default: return lisp_keyword("on-demand");
    }
}

static lisp_value anchor_list(uint32_t anchor) {
    static const struct { uint32_t bit; const char *name; } anchors[] = {
        { ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP, "top" },
        { ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM, "bottom" },
        { ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT, "left" },
        { ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT, "right" },
    };
    lisp_value list = lisp_nil();
    for (size_t i = 0; i < sizeof(anchors) / sizeof(anchors[0]); i++)
        if (anchor & anchors[i].bit)
            list = lisp_cons(lisp_keyword(anchors[i].name), list);
    return lisp_nreverse(list);
}

static lisp_value edge_name(uint32_t edge) {
    switch (edge) {
    case EDGE_TOP: return lisp_keyword("top");
    case EDGE_RIGHT: return lisp_keyword("right");
    case EDGE_BOTTOM: return lisp_keyword("bottom");
    case EDGE_LEFT: return lisp_keyword("left");
    default: return lisp_nil();
    }
}

static lisp_value convert_layer(struct tomoe *s, const struct tomoe_event *e) {
    size_t size = 0;
    const struct policy_layer_payload *payload = (const void *)
        policy_event_string_at(s, e->str[2], &size);
    if (!payload || size < sizeof(*payload)) return lisp_nil();
    lisp_value list = lisp_nil();
    list = put(list, "TYPE", lisp_keyword("layer"));
    list = put(list, "ID", lisp_uint(payload->id));
    list = put(list, "NAMESPACE", text_of(s, e->str[0], ""));
    list = put(list, "LAYER", layer_name((int32_t)payload->layer));
    list = put(list, "ANCHORS", anchor_list(payload->anchor));
    list = put(list, "EXCLUSIVE-ZONE", lisp_int((int32_t)payload->exclusive_zone));
    list = put(list, "MARGIN", lisp_list(4,
        lisp_int(payload->margin[0]), lisp_int(payload->margin[1]),
        lisp_int(payload->margin[2]), lisp_int(payload->margin[3])));
    list = put(list, "WIDTH", lisp_uint(payload->width));
    list = put(list, "HEIGHT", lisp_uint(payload->height));
    list = put(list, "KEYBOARD", keyboard_name((int32_t)payload->keyboard));
    list = put(list, "SCALE-120", lisp_int((int64_t)(payload->scale_120 + 0.5)));
    list = put(list, "OUTPUT", text_of(s, e->str[1], ""));
    lisp_value request = lisp_nil();
    request = put(request, "WIDTH", lisp_uint(payload->request_width));
    request = put(request, "HEIGHT", lisp_uint(payload->request_height));
    request = put(request, "MARGIN", lisp_list(4,
        lisp_int(payload->request_margin[0]), lisp_int(payload->request_margin[1]),
        lisp_int(payload->request_margin[2]), lisp_int(payload->request_margin[3])));
    request = put(request, "EXCLUSIVE-ZONE",
        lisp_int((int32_t)payload->request_exclusive_zone));
    request = put(request, "ANCHORS", anchor_list(payload->request_anchor));
    request = put(request, "EXCLUSIVE-EDGE", edge_name(payload->request_exclusive_edge));
    request = put(request, "LAYER", layer_name(payload->request_layer));
    request = put(request, "KEYBOARD", keyboard_name(payload->request_keyboard));
    list = put(list, "REQUEST", lisp_nreverse(request));
    return lisp_nreverse(list);
}

static lisp_value convert_unmap(struct tomoe *s, const struct tomoe_event *e) {
    lisp_value list = lisp_nil();
    list = put(list, "TYPE", lisp_keyword("unmap"));
    list = put(list, "ID", lisp_uint(e->id));
    return lisp_nreverse(list);
}

static lisp_value key_state(uint32_t state) {
    return lisp_keyword(state == POLICY_KEY_RELEASED ? "released" : "pressed");
}

static lisp_value pointer_position(struct tomoe *s, const struct tomoe_event *e,
        lisp_value list) {
    list = put(list, "X", lisp_double(e->fx));
    list = put(list, "Y", lisp_double(e->fy));
    list = put(list, "SX", lisp_double(e->sx));
    list = put(list, "SY", lisp_double(e->sy));
    return list;
}

static lisp_value convert_key(struct tomoe *s, const struct tomoe_event *e) {
    lisp_value list = lisp_nil();
    list = put(list, "TYPE", lisp_keyword("key"));
    list = put(list, "OWNER", text_of(s, e->str[0], ""));
    list = put(list, "COMMAND", text_of(s, e->str[1], ""));
    list = put(list, "STATE", key_state(e->state));
    list = put(list, "SOURCE-ID", lisp_uint(e->source_id));
    if (e->binding_or_callback_id)
        list = put(list, "BINDING-ID", lisp_uint(e->binding_or_callback_id));
    list = put(list, "DEVICE", lisp_uint(e->time_us));
    if (e->flags & POLICY_KEY_KEYSYM) {
        list = put(list, "KEYCODE", lisp_uint(e->keycode));
        list = put(list, "KEYSYM", optional_text(s, e->str[2]));
    } else if (e->flags & POLICY_KEY_POINTER) {
        list = put(list, "BUTTON", lisp_uint(e->button));
        list = put(list, "WINDOW", lisp_uint(e->id));
        list = put(list, "DELTA", lisp_double(e->delta));
        list = pointer_position(s, e, list);
    } else {
        list = put(list, "KEYCODE", lisp_uint(e->keycode));
        list = pointer_position(s, e, list);
    }
    return lisp_nreverse(list);
}

static lisp_value convert_button(struct tomoe *s, const struct tomoe_event *e) {
    lisp_value list = lisp_nil();
    list = put(list, "TYPE", lisp_keyword("button"));
    list = put(list, "ID", lisp_uint(e->id));
    list = put(list, "BUTTON", lisp_uint(e->button));
    list = put(list, "STATE", key_state(e->state));
    list = put(list, "X", lisp_int((int32_t)e->x));
    list = put(list, "Y", lisp_int((int32_t)e->y));
    list = put(list, "MODIFIERS", lisp_uint(e->modifiers));
    return lisp_nreverse(list);
}

static lisp_value convert_pointer(struct tomoe *s, const struct tomoe_event *e) {
    lisp_value list = lisp_nil();
    list = put(list, "TYPE", lisp_keyword("pointer"));
    if (e->state == POLICY_POINTER_MOTION) {
        static const char *const sides[] = { "top", "bottom", "left", "right" };
        list = put(list, "STATE", lisp_keyword("motion"));
        list = put(list, "ID", lisp_uint(e->id));
        list = put(list, "MOVED", boolean(e->button != 0));
        list = put(list, "X", lisp_int((int32_t)e->x));
        list = put(list, "Y", lisp_int((int32_t)e->y));
        list = put(list, "DISTANCE", lisp_int((int32_t)e->dx));
        lisp_value entered = lisp_nil(), left = lisp_nil();
        lisp_value edges_in = lisp_nil(), edges_out = lisp_nil();
        struct motion_state *m = &s->motion;
        for (size_t i = 0; i < m->plan.region_count && i < 64; i++) {
            if (e->source_id >> i & 1)
                entered = lisp_cons(lisp_cstring(m->plan.region[i].name), entered);
            if (e->binding_or_callback_id >> i & 1)
                left = lisp_cons(lisp_cstring(m->plan.region[i].name), left);
        }
        for (size_t i = 0; i < m->plan.edge_count && i < 4; i++) {
            if (e->keycode >> i & 1)
                edges_in = lisp_cons(lisp_keyword(sides[m->plan.edge[i].side]), edges_in);
            if (e->modifiers >> i & 1)
                edges_out = lisp_cons(lisp_keyword(sides[m->plan.edge[i].side]), edges_out);
        }
        list = put(list, "ENTERED", lisp_nreverse(entered));
        list = put(list, "LEFT", lisp_nreverse(left));
        list = put(list, "EDGES-IN", lisp_nreverse(edges_in));
        list = put(list, "EDGES-OUT", lisp_nreverse(edges_out));
    } else {
        list = put(list, "STATE", lisp_keyword(e->state == POLICY_POINTER_LEAVE ?
            "leave" : "enter"));
        list = put(list, "ID", lisp_uint(e->id));
        list = put(list, "MOVED", boolean(e->button != 0));
    }
    return lisp_nreverse(list);
}

static lisp_value convert_grab(struct tomoe *s, const struct tomoe_event *e) {
    static const char *const modes[] = { "", "move", "resize", "pointer" };
    lisp_value list = lisp_nil();
    list = put(list, "TYPE", lisp_keyword("grab"));
    list = put(list, "ID", lisp_uint(e->id));
    list = put(list, "MODE", lisp_keyword(e->x < 4 ? modes[e->x] : modes[0]));
    list = put(list, "X", lisp_int((int32_t)e->y));
    list = put(list, "Y", lisp_int((int32_t)e->dx));
    list = put(list, "DX", lisp_int((int32_t)e->fx));
    list = put(list, "DY", lisp_int((int32_t)e->fy));
    return lisp_nreverse(list);
}

static lisp_value convert_activity(struct tomoe *s, const struct tomoe_event *e) {
    lisp_value list = lisp_nil();
    list = put(list, "TYPE", lisp_keyword("activity"));
    list = put(list, "HAND", lisp_cstring(e->id ? "right" : "left"));
    return lisp_nreverse(list);
}

static lisp_value convert_request(struct tomoe *s, const struct tomoe_event *e) {
    lisp_value list = lisp_nil();
    list = put(list, "TYPE", lisp_keyword("request"));
    list = put(list, "ID", lisp_uint(e->id));
    char request[64];
    list = put(list, "REQUEST", text_copy(s, e->str[0], request, sizeof(request)) ?
        lisp_keyword(request) : lisp_nil());
    if (e->x != POLICY_REQUEST_ABSENT)
        list = put(list, "REQUESTED", boolean(e->x == POLICY_REQUEST_TRUE));
    if ((uint32_t)e->str[1] != EVENT_NO_STRING)
        list = put(list, "OUTPUT", optional_text(s, e->str[1]));
    if (e->button) {
        static const char *const names[] = { "top", "bottom", "left", "right" };
        static const uint32_t bits[] = { EDGE_TOP, EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT };
        char edge[64];
        size_t used = 0;
        for (size_t i = 0; i < 4; i++) {
            if (!(e->button & bits[i])) continue;
            if (used) edge[used++] = '-';
            size_t length = strlen(names[i]);
            memcpy(edge + used, names[i], length);
            used += length;
        }
        edge[used] = 0;
        list = put(list, "EDGES", lisp_keyword(edge));
    }
    return lisp_nreverse(list);
}

static lisp_value convert_screenshot(struct tomoe *s, const struct tomoe_event *e) {
    lisp_value list = lisp_nil();
    list = put(list, "TYPE", lisp_keyword("screenshot"));
    list = put(list, "PATH", text_of(s, e->str[0], ""));
    return lisp_nreverse(list);
}

static const char *const selector_tools[] = { "select", "arrow", "text", "free" };

static lisp_value convert_selector(struct tomoe *s, const struct tomoe_event *e) {
    lisp_value list = lisp_nil();
    list = put(list, "TYPE", lisp_keyword("selector"));
    list = put(list, "OPEN", boolean(e->id != 0));
    if (e->id) {
        list = put(list, "OUTPUT", text_of(s, e->str[0], ""));
        list = put(list, "ANCHOR", lisp_cons(lisp_int(e->x), lisp_cons(lisp_int(e->y), lisp_nil())));
        list = put(list, "CORNER", lisp_cons(lisp_int(e->dx), lisp_cons(lisp_int(e->dy), lisp_nil())));
        list = put(list, "SELECTED", boolean(e->state & 1));
        list = put(list, "DRAGGING", boolean(e->state & 2));
        list = put(list, "FROZEN", boolean(e->state & 4));
        list = put(list, "TYPING", boolean(e->state & 8));
        list = put(list, "TOOL", lisp_keyword(selector_tools[(e->state >> 4) & 3]));
    }
    return lisp_nreverse(list);
}

static lisp_value convert_ui(struct tomoe *s, const struct tomoe_event *e) {
    lisp_value list = lisp_nil();
    list = put(list, "TYPE", lisp_keyword("ui"));
    list = put(list, "OWNER", text_of(s, e->str[0], ""));
    list = put(list, "SOURCE-ID", lisp_uint(e->source_id));
    list = put(list, "CALLBACK-ID", lisp_uint(e->binding_or_callback_id));
    list = put(list, "SURFACE", text_of(s, e->str[1], ""));
    list = put(list, "OUTPUT", text_of(s, e->str[2], ""));
    list = put(list, "ELEMENT", text_of(s, e->str[3], ""));
    list = put(list, "COMMAND", text_of(s, (uint32_t)e->time_us, ""));
    list = put(list, "X", lisp_double(e->fx));
    list = put(list, "Y", lisp_double(e->fy));
    list = put(list, "BUTTON", lisp_uint(e->button));
    list = put(list, "MODIFIERS", lisp_uint(e->modifiers));
    return lisp_nreverse(list);
}

static policy_converter converters[EV_TYPE_COUNT] = {
    [EV_MAP] = convert_window,
    [EV_METADATA] = convert_window,
    [EV_BUFFER] = convert_buffer,
    [EV_GEOMETRY] = convert_geometry,
    [EV_LAYER] = convert_layer,
    [EV_UNMAP] = convert_unmap,
    [EV_KEY] = convert_key,
    [EV_BUTTON] = convert_button,
    [EV_POINTER] = convert_pointer,
    [EV_GRAB] = convert_grab,
    [EV_ACTIVITY] = convert_activity,
    [EV_REQUEST] = convert_request,
    [EV_SCREENSHOT] = convert_screenshot,
    [EV_SELECTOR] = convert_selector,
    [EV_UI] = convert_ui
};

void policy_set_converter(enum tomoe_event_type type, policy_converter fn) {
    if (type < EV_TYPE_COUNT) converters[type] = fn;
}

static const struct tomoe_event *ring_at(struct tomoe *s, size_t index) {
    return &s->event_ring[(s->event_head + index) % s->event_capacity];
}

static void ring_consume(struct tomoe *s, size_t consumed) {
    if (consumed >= s->event_count) {
        s->event_head = 0;
        s->event_count = 0;
        s->observation_barrier = 0;
        return;
    }
    s->event_head = (s->event_head + consumed) % s->event_capacity;
    s->event_count -= consumed;
    s->observation_barrier = s->observation_barrier > consumed ?
        s->observation_barrier - consumed : 0;
}

static bool native_waiting(struct tomoe *s) {
    if (!s->display) return false;
    struct pollfd loop = { .fd = wl_event_loop_get_fd(wl_display_get_event_loop(s->display)),
        .events = POLLIN };
    return poll(&loop, 1, 0) > 0;
}

void policy_drain(struct tomoe *s) {
    if (!s || !s->event_ring || !s->event_count || s->grab_held) return;
    size_t consumed = 0;
    uint16_t last = EV_NONE;
    for (; consumed < s->event_count; consumed++) {
        const struct tomoe_event *event = ring_at(s, consumed);
        bool called = false;
        timing_event_taken(s);
        if (event->type == EV_INPUT || event->type == EV_ACTIVITY)
            policy_event_broadcast(s, event);
        if (policy_event_delivers(s, event->type) && converters[event->type]) {
            lisp_value value = converters[event->type](s, event);
            if (!lisp_nilp(value)) {
                lisp_value events = lisp_cons(value, lisp_nil());
                char error[256];
                lisp_value result;
                if (policy_call(POLICY_HOOK_DISPATCH, 1, &events, &result, error, sizeof(error)))
                    tomoe_log(LOG_ERROR, "tomoe: dispatch failed: %s", error);
                lisp_end_turn();
                called = true;
            }
        }
        timing_event_done(s);
        last = event->type;
        if (event->type == EV_GRAB || (called && consumed + 1 < s->event_count &&
                native_waiting(s))) {
            consumed++;
            break;
        }
    }
    ring_consume(s, consumed);
    if (last == EV_GRAB) policy_event_grab_hold(s);
    if (!s->event_count) s->event_arena_used = 0;
}

int policy_call(const char *hook, int argc, const lisp_value *argv, lisp_value *result,
        char *error, size_t error_size) {
    struct tomoe *s = policy_server;
    if (s) lisp_set_limits(s->policy_steps, s->policy_deadline_ns);
    return lisp_call(hook, argc, argv, result, error, error_size);
}

static int number(lisp_value value) {
    if (lisp_intp(value)) return (int)lisp_get_int(value);
    return lisp_numberp(value) ? (int)lisp_get_double(value) : 0;
}

static int64_t integer(lisp_value value) {
    if (lisp_intp(value)) return lisp_get_int(value);
    return lisp_numberp(value) ? (int64_t)lisp_get_double(value) : 0;
}

static uint64_t unsigned_integer(lisp_value value) {
    return (uint64_t)integer(value);
}

static double real(lisp_value value) {
    if (lisp_intp(value)) return (double)lisp_get_int(value);
    return lisp_numberp(value) ? lisp_get_double(value) : 0.0;
}

static bool truth(lisp_value value) {
    return lisp_intp(value) ? lisp_get_int(value) != 0 : !lisp_nilp(value);
}

static const char *string(lisp_value value) {
    if (!lisp_stringp(value)) return "";
    const char *text = lisp_get_string(value, NULL);
    return text ? text : "";
}

static const char *nullable_string(lisp_value value) {
    return lisp_nilp(value) ? NULL : string(value);
}

static lisp_value status(int value) {
    return lisp_int(value);
}

static lisp_value message(const char *text) {
    return text ? lisp_cstring(text) : lisp_nil();
}

static lisp_value data(const char *snapshot) {
    if (!snapshot) return lisp_nil();
    lisp_value value = lisp_nil();
    char error[128];
    if (lisp_read_data(snapshot, strlen(snapshot), &value, error, sizeof(error)))
        return lisp_nil();
    return value;
}

static lisp_value prim_abi(int argc, lisp_value *argv) {
    return status(tomoe_abi_version());
}

static lisp_value prim_activation_token(int argc, lisp_value *argv) {
    return message(tomoe_activation_token(server()));
}

static lisp_value prim_activation_revoke(int argc, lisp_value *argv) {
    tomoe_activation_revoke(server(), string(argv[0]));
    return lisp_nil();
}

static lisp_value prim_hit_test(int argc, lisp_value *argv) {
    return data(tomoe_hit_test(server(), real(argv[0]), real(argv[1])));
}

static lisp_value prim_frames(int argc, lisp_value *argv) {
    return data(tomoe_frames(server()));
}

static lisp_value prim_animation(int argc, lisp_value *argv) {
    return data(tomoe_animation(server()));
}

static lisp_value prim_trim_heap(int argc, lisp_value *argv) {
    tomoe_trim_heap();
    return lisp_nil();
}

static lisp_value prim_timings(int argc, lisp_value *argv) {
    return data(tomoe_timings());
}

static lisp_value prim_timing_enabled(int argc, lisp_value *argv) {
    return status(tomoe_timing_enabled());
}

static lisp_value prim_timing_now(int argc, lisp_value *argv) {
    return lisp_uint(tomoe_timing_now());
}

static lisp_value prim_timing_channel(int argc, lisp_value *argv) {
    return status(tomoe_timing_channel(string(argv[0])));
}

static lisp_value prim_timing_reset(int argc, lisp_value *argv) {
    tomoe_timing_reset();
    return lisp_nil();
}

static lisp_value prim_timing_sample(int argc, lisp_value *argv) {
    tomoe_timing_sample(number(argv[0]), unsigned_integer(argv[1]));
    return lisp_nil();
}

static lisp_value prim_activity_serial(int argc, lisp_value *argv) {
    return lisp_uint(tomoe_activity_serial(server()));
}

static lisp_value prim_cursor_state(int argc, lisp_value *argv) {
    return data(tomoe_cursor_state(server(), unsigned_integer(argv[0])));
}

static lisp_value prim_cursor_motion(int argc, lisp_value *argv) {
    return data(tomoe_cursor_motion(server()));
}

static lisp_value prim_cursor_apply(int argc, lisp_value *argv) {
    return status(tomoe_cursor_apply(server(), real(argv[0]), real(argv[1]), real(argv[2]),
        real(argv[3]), real(argv[4]), real(argv[5]), real(argv[6])));
}

static lisp_value prim_keyboard_focus(int argc, lisp_value *argv) {
    return lisp_uint(tomoe_keyboard_focus(server()));
}

static lisp_value prim_input_stream(int argc, lisp_value *argv) {
    return status(tomoe_input_stream(server(), truth(argv[0])));
}

static lisp_value prim_grab_id(int argc, lisp_value *argv) {
    return lisp_uint(tomoe_grab_id(server()));
}

static lisp_value prim_grab_mode(int argc, lisp_value *argv) {
    return status(tomoe_grab_mode(server()));
}

static lisp_value prim_close(int argc, lisp_value *argv) {
    tomoe_close(server(), (uint32_t)unsigned_integer(argv[0]));
    return lisp_nil();
}

static lisp_value prim_window_identifier(int argc, lisp_value *argv) {
    return message(tomoe_window_identifier(server(), (uint32_t)unsigned_integer(argv[0])));
}

static lisp_value prim_clipboard_copy(int argc, lisp_value *argv) {
    return status(tomoe_clipboard_copy(server(), string(argv[0]), string(argv[1]),
        truth(argv[2])));
}

static lisp_value prim_screenshot(int argc, lisp_value *argv) {
    tomoe_screenshot(server(), truth(argv[0]));
    return lisp_nil();
}

static lisp_value prim_keysym(int argc, lisp_value *argv) {
    return lisp_uint(tomoe_keysym(string(argv[0])));
}

static lisp_value prim_warm_shaders(int argc, lisp_value *argv) {
    return status(tomoe_warm_shaders(string(argv[0]), string(argv[1]), NULL));
}

static lisp_value prim_outputs_begin(int argc, lisp_value *argv) {
    return status(tomoe_outputs_begin(server()));
}

static lisp_value prim_outputs_pending(int argc, lisp_value *argv) {
    return status(tomoe_outputs_pending(server()));
}

static lisp_value prim_output_power(int argc, lisp_value *argv) {
    return status(tomoe_output_power(server(), nullable_string(argv[0]), number(argv[1])));
}

static lisp_value prim_virtual_output(int argc, lisp_value *argv) {
    tomoe_virtual_output(server(), string(argv[0]), number(argv[1]), number(argv[2]),
        number(argv[3]));
    return lisp_nil();
}

static lisp_value prim_output_hold(int argc, lisp_value *argv) {
    return status(tomoe_output_hold(server(), string(argv[0])));
}

static lisp_value prim_output(int argc, lisp_value *argv) {
    return status(tomoe_output(server(), string(argv[0]), number(argv[1]), number(argv[2]),
        number(argv[3]), number(argv[4]), number(argv[5]), number(argv[6]), number(argv[7]),
        truth(argv[8]), number(argv[9])));
}

static lisp_value prim_output_options(int argc, lisp_value *argv) {
    return status(tomoe_output_options(server(), string(argv[0]), truth(argv[1]),
        string(argv[2]), truth(argv[3]), truth(argv[4])));
}

static lisp_value prim_output_icc(int argc, lisp_value *argv) {
    return status(tomoe_output_icc(server(), string(argv[0]), string(argv[1])));
}

static lisp_value prim_outputs_preview(int argc, lisp_value *argv) {
    return data(tomoe_outputs_preview(server()));
}

static lisp_value prim_outputs_revision(int argc, lisp_value *argv) {
    return lisp_uint(tomoe_outputs_revision(server()));
}

static lisp_value prim_outputs_current(int argc, lisp_value *argv) {
    return data(tomoe_outputs_current(server()));
}

static lisp_value prim_layers_begin(int argc, lisp_value *argv) {
    return status(tomoe_layers_begin(server(), truth(argv[0])));
}

static lisp_value prim_layers_output(int argc, lisp_value *argv) {
    return status(tomoe_layers_output(server(), string(argv[0]), number(argv[1]),
        number(argv[2]), number(argv[3]), number(argv[4]), number(argv[5])));
}

static lisp_value prim_layers_surface(int argc, lisp_value *argv) {
    return status(tomoe_layers_surface(server(), (uint32_t)unsigned_integer(argv[0]),
        number(argv[1]), number(argv[2]), number(argv[3]), number(argv[4])));
}

static lisp_value prim_layers_preview(int argc, lisp_value *argv) {
    return data(tomoe_layers_preview(server()));
}

static lisp_value prim_present_begin(int argc, lisp_value *argv) {
    return status(tomoe_present_begin(server(), number(argv[0]), number(argv[1]),
        real(argv[2]), (uint32_t)unsigned_integer(argv[3]), truth(argv[4]),
        truth(argv[5]), truth(argv[6]), (uint32_t)unsigned_integer(argv[7]),
        number(argv[8])));
}

static lisp_value prim_present_bind(int argc, lisp_value *argv) {
    return status(tomoe_present_bind(server(), (uint32_t)unsigned_integer(argv[0]),
        (uint32_t)unsigned_integer(argv[1]), string(argv[2]), string(argv[3]),
        string(argv[4]), unsigned_integer(argv[5])));
}

static lisp_value prim_present_keyboard(int argc, lisp_value *argv) {
    return status(tomoe_present_keyboard(server(), string(argv[0]), string(argv[1]),
        string(argv[2]), string(argv[3]), string(argv[4]), number(argv[5]),
        number(argv[6])));
}

static lisp_value prim_present_window(int argc, lisp_value *argv) {
    return status(tomoe_present_window(server(), (uint32_t)unsigned_integer(argv[0]),
        number(argv[1]), number(argv[2]), number(argv[3]), number(argv[4]),
        truth(argv[5]), truth(argv[6]), truth(argv[7])));
}

static lisp_value prim_present_settings(int argc, lisp_value *argv) {
    return status(tomoe_present_settings(server()));
}

static lisp_value prim_present_setting(int argc, lisp_value *argv) {
    return status(tomoe_present_setting(server(), string(argv[0]), real(argv[1])));
}

static lisp_value prim_present_setting_text(int argc, lisp_value *argv) {
    return status(tomoe_present_setting_text(server(), string(argv[0]), string(argv[1])));
}

static lisp_value prim_present_window_style(int argc, lisp_value *argv) {
    return status(tomoe_present_window_style(server(), (uint32_t)unsigned_integer(argv[0]),
        number(argv[1]), number(argv[2]), number(argv[3]), integer(argv[4]),
        integer(argv[5])));
}

static lisp_value prim_present_window_visual(int argc, lisp_value *argv) {
    return status(tomoe_present_window_visual(server(),
        (uint32_t)unsigned_integer(argv[0]), real(argv[1]),
        (uint32_t)unsigned_integer(argv[2]), real(argv[3]), real(argv[4]),
        number(argv[5]), number(argv[6])));
}

static lisp_value prim_present_window_shader(int argc, lisp_value *argv) {
    return status(tomoe_present_window_shader(server(),
        (uint32_t)unsigned_integer(argv[0]), string(argv[1]), number(argv[2]),
        number(argv[3]), number(argv[4]), real(argv[5]), string(argv[6])));
}

static lisp_value prim_present_window_mesh(int argc, lisp_value *argv) {
    lisp_value tail = argv[3];
    size_t count = 0;
    for (lisp_value item = tail; lisp_consp(item); item = lisp_cdr(item)) count++;
    double inline_values[256];
    double *values = count <= sizeof(inline_values) / sizeof(inline_values[0]) ? inline_values :
        malloc(count * sizeof(double));
    if (!values) return lisp_nil();
    size_t index = 0;
    for (lisp_value item = tail; lisp_consp(item) && index < count; item = lisp_cdr(item))
        values[index++] = real(lisp_car(item));
    int result = tomoe_present_window_mesh(server(),
        (uint32_t)unsigned_integer(argv[0]), number(argv[1]), number(argv[2]),
        values, (unsigned long)count);
    if (values != inline_values) free(values);
    return status(result);
}

static lisp_value prim_present_keyboard_grab(int argc, lisp_value *argv) {
    return status(tomoe_present_keyboard_grab(server(), string(argv[0]),
        unsigned_integer(argv[1]), string(argv[2])));
}

static lisp_value prim_motion(int argc, lisp_value *argv) {
    return status(tomoe_motion(server(), truth(argv[0]), truth(argv[1]), truth(argv[2]),
        truth(argv[3]), number(argv[4])));
}

static lisp_value prim_motion_region(int argc, lisp_value *argv) {
    return status(tomoe_motion_region(server(), string(argv[0]), number(argv[1]),
        number(argv[2]), number(argv[3]), number(argv[4])));
}

static lisp_value prim_motion_edge(int argc, lisp_value *argv) {
    return status(tomoe_motion_edge(server(), number(argv[0]), number(argv[1])));
}

static lisp_value prim_motion_drag(int argc, lisp_value *argv) {
    return status(tomoe_motion_drag(server(), (uint32_t)unsigned_integer(argv[0]),
        number(argv[1]), number(argv[2]), number(argv[3]), number(argv[4])));
}

static lisp_value prim_present_sounds(int argc, lisp_value *argv) {
    return status(tomoe_present_sounds(server()));
}

static lisp_value prim_present_sound(int argc, lisp_value *argv) {
    return message(tomoe_present_sound(server(), number(argv[0]), string(argv[1]),
        real(argv[2])));
}

static lisp_value prim_sound_stats(int argc, lisp_value *argv) {
    return data(tomoe_sound_stats(server()));
}

static lisp_value prim_native_errors(int argc, lisp_value *argv) {
    return data(tomoe_native_errors());
}

static lisp_value prim_present_apply(int argc, lisp_value *argv) {
    return message(tomoe_present_apply(server()));
}

static lisp_value prim_present_stack(int argc, lisp_value *argv) {
    return status(tomoe_present_stack(server(), (uint32_t)unsigned_integer(argv[0])));
}

static lisp_value prim_present_abort(int argc, lisp_value *argv) {
    tomoe_present_abort(server());
    return lisp_nil();
}

static lisp_value prim_event_barrier(int argc, lisp_value *argv) {
    return status(policy_event_barrier(server()));
}

static lisp_value prim_set_reads(int argc, lisp_value *argv) {
    struct tomoe *s = server();
    uint64_t mask = 0;
    for (lisp_value tail = argv[0]; lisp_consp(tail); tail = lisp_cdr(tail)) {
        lisp_value item = lisp_car(tail);
        if (!lisp_keywordp(item) && !lisp_symbolp(item)) continue;
        const char *name = lisp_symbol_name(item);
        if (!name) continue;
        for (int i = 0; i < POLICY_KEY_COUNT; i++)
            if (strcasecmp(name, policy_keys[i]) == 0) mask |= UINT64_C(1) << i;
    }
    s->event_read_mask = mask;
    return lisp_nil();
}

static lisp_value prim_now_ms(int argc, lisp_value *argv) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return lisp_uint((uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u);
}

static lisp_value prim_strftime(int argc, lisp_value *argv) {
    const char *format = string(argv[0]);
    time_t now = time(NULL);
    struct tm broken;
    localtime_r(&now, &broken);
    char buffer[1024];
    size_t written = strftime(buffer, sizeof(buffer), format, &broken);
    return lisp_string(buffer, written);
}

static int log_level(const char *name) {
    if (strcasecmp(name, "DEBUG") == 0) return LOG_DEBUG;
    if (strcasecmp(name, "INFO") == 0) return LOG_INFO;
    if (strcasecmp(name, "WARNING") == 0 || strcasecmp(name, "WARN") == 0)
        return LOG_WARNING;
    return LOG_ERROR;
}

static lisp_value prim_log(int argc, lisp_value *argv) {
    const char *level = lisp_keywordp(argv[0]) || lisp_symbolp(argv[0]) ?
        lisp_symbol_name(argv[0]) : string(argv[0]);
    tomoe_log(log_level(level ? level : "INFO"), "%s", string(argv[1]));
    return lisp_nil();
}

static lisp_value prim_getenv(int argc, lisp_value *argv) {
    const char *value = getenv(string(argv[0]));
    return value ? lisp_cstring(value) : lisp_nil();
}

static lisp_value prim_read_file(int argc, lisp_value *argv) {
    const char *path = string(argv[0]);
    uint64_t limit = unsigned_integer(argv[1]);
    if (!*path) return lisp_nil();
    struct stat status;
    if (stat(path, &status) < 0 || !S_ISREG(status.st_mode)) return lisp_nil();
    if (status.st_size < 0 || (uint64_t)status.st_size > limit) return lisp_nil();
    FILE *file = fopen(path, "rb");
    if (!file) return lisp_nil();
    size_t size = (size_t)status.st_size;
    char *buffer = malloc(size + 1);
    if (!buffer) {
        fclose(file);
        return lisp_nil();
    }
    size_t got = fread(buffer, 1, size, file);
    fclose(file);
    if (got != size) {
        free(buffer);
        return lisp_nil();
    }
    buffer[size] = '\0';
    lisp_value text = lisp_string(buffer, size);
    free(buffer);
    return text;
}

struct path_list {
    char **items;
    size_t count, capacity;
    bool failed;
};

static void path_list_add(struct path_list *list, const char *path) {
    if (list->failed) return;
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 64;
        char **grown = realloc(list->items, capacity * sizeof(char *));
        if (!grown) { list->failed = true; return; }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count] = strdup(path);
    if (!list->items[list->count]) { list->failed = true; return; }
    list->count++;
}

static void path_list_walk(struct path_list *list, const char *path) {
    if (list->failed) return;
    struct stat info;
    if (lstat(path, &info) != 0) return;
    if (S_ISREG(info.st_mode)) {
        path_list_add(list, path);
        return;
    }
    if (!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) return;
    DIR *directory = opendir(path);
    if (!directory) return;
    struct dirent *entry;
    while (!list->failed && (entry = readdir(directory))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        int written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(child)) continue;
        path_list_walk(list, child);
    }
    closedir(directory);
}

static int compare_paths(const void *left, const void *right) {
    return strcmp(*(char *const *)left, *(char *const *)right);
}

static lisp_value prim_list_directory(int argc, lisp_value *argv) {
    char resolved[PATH_MAX];
    if (!realpath(string(argv[0]), resolved)) return lisp_nil();
    struct path_list list = { 0 };
    path_list_walk(&list, resolved);
    if (list.failed) {
        for (size_t i = 0; i < list.count; i++) free(list.items[i]);
        free(list.items);
        lisp_error("Cannot list directory %s.", resolved);
    }
    qsort(list.items, list.count, sizeof(char *), compare_paths);
    lisp_value result = lisp_nil();
    for (size_t i = list.count; i > 0; i--) {
        result = lisp_cons(lisp_cstring(list.items[i - 1]), result);
        free(list.items[i - 1]);
    }
    free(list.items);
    return result;
}

static bool patch_name(const char *name) {
    size_t length = strlen(name);
    if (length < 1 || length > 40) return false;
    for (size_t i = 0; i < length; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '-' && name[i] != '_')
            return false;
    return true;
}

static lisp_value prim_write_append(int argc, lisp_value *argv) {
    const char *path = string(argv[0]);
    size_t length = 0;
    const char *text = lisp_get_string(argv[1], &length);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) lisp_error("Cannot open %s.", path);
    while (length > 0) {
        ssize_t written = write(fd, text, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { close(fd); lisp_error("Cannot write %s.", path); }
        text += written;
        length -= (size_t)written;
    }
    close(fd);
    return lisp_t();
}

static lisp_value prim_delete_file(int argc, lisp_value *argv) {
    const char *path = string(argv[0]);
    if (unlink(path) != 0 && errno != ENOENT) lisp_error("Cannot delete %s.", path);
    return lisp_t();
}

static lisp_value prim_write_patch_file(int argc, lisp_value *argv) {
    const char *name = string(argv[0]);
    if (!patch_name(name)) lisp_error("Invalid patch name %s.", name);
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime) lisp_error("XDG_RUNTIME_DIR is not set.");
    char directory[PATH_MAX], path[PATH_MAX];
    int written = snprintf(directory, sizeof(directory), "%s/tomoe-patches", runtime);
    if (written < 0 || (size_t)written >= sizeof(directory))
        lisp_error("Patch directory path is too long.");
    if (mkdir(directory, 0700) != 0 && errno != EEXIST)
        lisp_error("Cannot create %s.", directory);
    written = snprintf(path, sizeof(path), "%s/%s.lisp", directory, name);
    if (written < 0 || (size_t)written >= sizeof(path))
        lisp_error("Patch path is too long.");
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) lisp_error("Cannot write %s.", path);
    if (fchmod(fd, 0600) != 0) { close(fd); lisp_error("Cannot write %s.", path); }
    size_t length = 0;
    const char *text = lisp_stringp(argv[1]) ? lisp_get_string(argv[1], &length) : "";
    if (!text) { close(fd); lisp_error("Cannot write %s.", path); }
    size_t offset = 0;
    while (offset < length) {
        ssize_t step = write(fd, text + offset, length - offset);
        if (step <= 0) { close(fd); lisp_error("Cannot write %s.", path); }
        offset += (size_t)step;
    }
    if (close(fd) != 0) lisp_error("Cannot write %s.", path);
    return lisp_cstring(path);
}

static lisp_value prim_json_broadcast(int argc, lisp_value *argv) {
    ipc_broadcast(string(argv[0]), argv[1]);
    return lisp_nil();
}

static lisp_value prim_json_subscribed(int argc, lisp_value *argv) {
    return boolean(ipc_subscribed());
}

static lisp_value prim_json_validate_value(int argc, lisp_value *argv) {
    return json_validate_value(argv[0]);
}

static lisp_value prim_json_output_size(int argc, lisp_value *argv) {
    return lisp_uint(json_output_size(argv[0]));
}

static lisp_value prim_json_object(int argc, lisp_value *argv) {
    return json_object_of(argc, argv);
}

static lisp_value prim_json_array(int argc, lisp_value *argv) {
    return json_array_of(argc, argv);
}

static const struct primitive {
    const char *name;
    lisp_primitive fn;
    int min, max;
} primitives[] = {
    { "%abi", prim_abi, 0, 0 },
    { "%activation-token", prim_activation_token, 0, 0 },
    { "%activation-revoke", prim_activation_revoke, 1, 1 },
    { "%hit-test", prim_hit_test, 2, 2 },
    { "%frames", prim_frames, 0, 0 },
    { "%animation", prim_animation, 0, 0 },
    { "%trim-heap", prim_trim_heap, 0, 0 },
    { "%timings", prim_timings, 0, 0 },
    { "%timing-enabled", prim_timing_enabled, 0, 0 },
    { "%timing-now", prim_timing_now, 0, 0 },
    { "%timing-channel", prim_timing_channel, 1, 1 },
    { "%timing-reset", prim_timing_reset, 0, 0 },
    { "%timing-sample", prim_timing_sample, 2, 2 },
    { "%activity-serial", prim_activity_serial, 0, 0 },
    { "%cursor-state", prim_cursor_state, 1, 1 },
    { "%cursor-motion", prim_cursor_motion, 0, 0 },
    { "%cursor-apply", prim_cursor_apply, 7, 7 },
    { "%keyboard-focus", prim_keyboard_focus, 0, 0 },
    { "%input-stream", prim_input_stream, 1, 1 },
    { "%grab-id", prim_grab_id, 0, 0 },
    { "%grab-mode", prim_grab_mode, 0, 0 },
    { "%close", prim_close, 1, 1 },
    { "%window-identifier", prim_window_identifier, 1, 1 },
    { "%clipboard-copy", prim_clipboard_copy, 3, 3 },
    { "%screenshot", prim_screenshot, 1, 1 },
    { "%keysym", prim_keysym, 1, 1 },
    { "%warm-shaders", prim_warm_shaders, 2, 2 },
    { "%outputs-begin", prim_outputs_begin, 0, 0 },
    { "%outputs-pending", prim_outputs_pending, 0, 0 },
    { "%output-power", prim_output_power, 2, 2 },
    { "%virtual-output", prim_virtual_output, 4, 4 },
    { "%output-hold", prim_output_hold, 1, 1 },
    { "%output", prim_output, 10, 10 },
    { "%output-options", prim_output_options, 5, 5 },
    { "%output-icc", prim_output_icc, 2, 2 },
    { "%outputs-preview", prim_outputs_preview, 0, 0 },
    { "%outputs-revision", prim_outputs_revision, 0, 0 },
    { "%outputs-current", prim_outputs_current, 0, 0 },
    { "%layers-begin", prim_layers_begin, 1, 1 },
    { "%layers-output", prim_layers_output, 6, 6 },
    { "%layers-surface", prim_layers_surface, 5, 5 },
    { "%layers-preview", prim_layers_preview, 0, 0 },
    { "%present-begin", prim_present_begin, 9, 9 },
    { "%present-bind", prim_present_bind, 6, 6 },
    { "%present-keyboard", prim_present_keyboard, 7, 7 },
    { "%present-window", prim_present_window, 8, 8 },
    { "%present-settings", prim_present_settings, 0, 0 },
    { "%present-setting", prim_present_setting, 2, 2 },
    { "%present-setting-text", prim_present_setting_text, 2, 2 },
    { "%present-window-style", prim_present_window_style, 6, 6 },
    { "%present-window-visual", prim_present_window_visual, 7, 7 },
    { "%present-window-shader", prim_present_window_shader, 7, 7 },
    { "%present-window-mesh", prim_present_window_mesh, 4, 4 },
    { "%present-keyboard-grab", prim_present_keyboard_grab, 3, 3 },
    { "%motion", prim_motion, 5, 5 },
    { "%motion-region", prim_motion_region, 5, 5 },
    { "%motion-edge", prim_motion_edge, 2, 2 },
    { "%motion-drag", prim_motion_drag, 5, 5 },
    { "%present-sounds", prim_present_sounds, 0, 0 },
    { "%present-sound", prim_present_sound, 3, 3 },
    { "%sound-stats", prim_sound_stats, 0, 0 },
    { "%native-errors", prim_native_errors, 0, 0 },
    { "%present-apply", prim_present_apply, 0, 0 },
    { "%present-stack", prim_present_stack, 1, 1 },
    { "%present-abort", prim_present_abort, 0, 0 },
    { "%event-barrier", prim_event_barrier, 0, 0 },
    { "%set-reads", prim_set_reads, 1, 1 },
    { "%now-ms", prim_now_ms, 0, 0 },
    { "%strftime", prim_strftime, 1, 1 },
    { "%log", prim_log, 2, 2 },
    { "%getenv", prim_getenv, 1, 1 },
    { "%read-file", prim_read_file, 2, 2 },
    { "%list-directory", prim_list_directory, 1, 1 },
    { "%write-patch-file", prim_write_patch_file, 2, 2 },
    { "%write-append", prim_write_append, 2, 2 },
    { "%delete-file", prim_delete_file, 1, 1 },
    { "%json-broadcast", prim_json_broadcast, 2, 2 },
    { "%json-subscribed-p", prim_json_subscribed, 0, 0 },
    { "%json-validate-value", prim_json_validate_value, 1, 1 },
    { "%json-output-size", prim_json_output_size, 2, 2 },
    { "json-object", prim_json_object, 0, -1 },
    { "json-array", prim_json_array, 0, -1 }
};

void policy_register_primitives(struct tomoe *server) {
    policy_server = server;
    server->policy_steps = 4000000;
    server->policy_deadline_ns = 1000000000;
    for (size_t i = 0; i < sizeof(primitives) / sizeof(primitives[0]); i++)
        lisp_define_primitive(primitives[i].name, primitives[i].fn,
            primitives[i].min, primitives[i].max);
    policy_set_converter(EV_OUTPUTS, outputs_event_value);
}
