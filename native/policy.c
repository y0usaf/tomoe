#include "internal.h"
#include "budget.h"
#include "resources-internal.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static struct tomoe *policy_server;

static struct tomoe *server(void) {
    return policy_server;
}

static pv put(pv list, const char *key, pv value) {
    return pv_cons(value, pv_cons(pv_keyword(key), list));
}

static pv boolean(bool value) {
    return pv_bool(value);
}

static pv text_of(struct tomoe *s, uint32_t handle, const char *fallback) {
    size_t length = 0;
    const char *text = policy_event_string_at(s, handle, &length);
    return text ? pv_string(text, length) : pv_cstring(fallback ? fallback : "");
}

static pv optional_text(struct tomoe *s, uint32_t handle) {
    size_t length = 0;
    const char *text = policy_event_string_at(s, handle, &length);
    return text ? pv_string(text, length) : pv_nil();
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
    if (s->policy_capturing) return event_policies[type].flags & EVF_FACT;
    if (!s->policy_accepting) {
        bool queued = type == EV_WATCH || type == EV_PROCESS || type == EV_SOURCE ||
            type == EV_SERVICES || type == EV_SYSTEM;
        if (!queued || !s->policy_link || tomoe_test_flag("policy-downtime-drop")) return false;
    }
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

static pv convert_window(struct tomoe *s, const struct tomoe_event *e) {
    pv list = pv_nil();
    list = put(list, "TYPE", pv_keyword(e->type == EV_MAP ? "map" : "metadata"));
    list = put(list, "ID", pv_uint(e->id));
    list = put(list, "WIDTH", pv_int((int32_t)e->x));
    list = put(list, "HEIGHT", pv_int((int32_t)e->y));
    list = put(list, "CLIENT-WIDTH", pv_int((int32_t)e->dx));
    list = put(list, "CLIENT-HEIGHT", pv_int((int32_t)e->dy));
    list = put(list, "BUFFERED", boolean(e->state & POLICY_WINDOW_BUFFERED));
    list = put(list, "TITLE", text_of(s, e->str[0], ""));
    list = put(list, "APP-ID", text_of(s, e->str[1], ""));
    list = put(list, "PID", pv_int((int32_t)e->button));
    list = put(list, "CONTENT-TYPE", pv_keyword(window_content_type(e->keycode)));
    list = put(list, "TAG", text_of(s, e->str[2], ""));
    list = put(list, "DESCRIPTION", text_of(s, e->str[3], ""));
    list = put(list, "PARENT", e->modifiers ? pv_uint(e->modifiers) : pv_nil());
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
            list = put(list, "REQUEST", pv_keyword(name));
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
    return pv_nreverse(list);
}

static pv convert_buffer(struct tomoe *s, const struct tomoe_event *e) {
    pv list = pv_nil();
    list = put(list, "TYPE", pv_keyword("buffer"));
    list = put(list, "ID", pv_uint(e->id));
    list = put(list, "ATTACHED", boolean(e->state & POLICY_WINDOW_ATTACHED));
    list = put(list, "WIDTH", pv_int((int32_t)e->x));
    list = put(list, "HEIGHT", pv_int((int32_t)e->y));
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
    return pv_nreverse(list);
}

static pv convert_geometry(struct tomoe *s, const struct tomoe_event *e) {
    pv list = pv_nil();
    list = put(list, "TYPE", pv_keyword("geometry"));
    list = put(list, "ID", pv_uint(e->id));
    list = put(list, "WIDTH", pv_int((int32_t)e->x));
    list = put(list, "HEIGHT", pv_int((int32_t)e->y));
    list = put(list, "CLIENT-WIDTH", pv_int((int32_t)e->dx));
    list = put(list, "CLIENT-HEIGHT", pv_int((int32_t)e->dy));
    return pv_nreverse(list);
}

static pv layer_name(int32_t layer) {
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return pv_keyword("background");
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return pv_keyword("bottom");
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return pv_keyword("top");
    default: return pv_keyword("overlay");
    }
}

static pv keyboard_name(int32_t keyboard) {
    switch (keyboard) {
    case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE: return pv_keyword("none");
    case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE:
        return pv_keyword("exclusive");
    default: return pv_keyword("on-demand");
    }
}

static pv anchor_list(uint32_t anchor) {
    static const struct { uint32_t bit; const char *name; } anchors[] = {
        { ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP, "top" },
        { ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM, "bottom" },
        { ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT, "left" },
        { ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT, "right" },
    };
    pv list = pv_nil();
    for (size_t i = 0; i < sizeof(anchors) / sizeof(anchors[0]); i++)
        if (anchor & anchors[i].bit)
            list = pv_cons(pv_keyword(anchors[i].name), list);
    return pv_nreverse(list);
}

static pv edge_name(uint32_t edge) {
    switch (edge) {
    case EDGE_TOP: return pv_keyword("top");
    case EDGE_RIGHT: return pv_keyword("right");
    case EDGE_BOTTOM: return pv_keyword("bottom");
    case EDGE_LEFT: return pv_keyword("left");
    default: return pv_nil();
    }
}

static pv convert_layer(struct tomoe *s, const struct tomoe_event *e) {
    size_t size = 0;
    const struct policy_layer_payload *payload = (const void *)
        policy_event_string_at(s, e->str[2], &size);
    if (!payload || size < sizeof(*payload)) return pv_nil();
    pv list = pv_nil();
    list = put(list, "TYPE", pv_keyword("layer"));
    list = put(list, "ID", pv_uint(payload->id));
    list = put(list, "NAMESPACE", text_of(s, e->str[0], ""));
    list = put(list, "LAYER", layer_name((int32_t)payload->layer));
    list = put(list, "ANCHORS", anchor_list(payload->anchor));
    list = put(list, "EXCLUSIVE-ZONE", pv_int((int32_t)payload->exclusive_zone));
    list = put(list, "MARGIN", pv_list(4,
        pv_int(payload->margin[0]), pv_int(payload->margin[1]),
        pv_int(payload->margin[2]), pv_int(payload->margin[3])));
    list = put(list, "WIDTH", pv_uint(payload->width));
    list = put(list, "HEIGHT", pv_uint(payload->height));
    list = put(list, "KEYBOARD", keyboard_name((int32_t)payload->keyboard));
    list = put(list, "SCALE-120", pv_int((int64_t)(payload->scale_120 + 0.5)));
    list = put(list, "OUTPUT", text_of(s, e->str[1], ""));
    pv request = pv_nil();
    request = put(request, "WIDTH", pv_uint(payload->request_width));
    request = put(request, "HEIGHT", pv_uint(payload->request_height));
    request = put(request, "MARGIN", pv_list(4,
        pv_int(payload->request_margin[0]), pv_int(payload->request_margin[1]),
        pv_int(payload->request_margin[2]), pv_int(payload->request_margin[3])));
    request = put(request, "EXCLUSIVE-ZONE",
        pv_int((int32_t)payload->request_exclusive_zone));
    request = put(request, "ANCHORS", anchor_list(payload->request_anchor));
    request = put(request, "EXCLUSIVE-EDGE", edge_name(payload->request_exclusive_edge));
    request = put(request, "LAYER", layer_name(payload->request_layer));
    request = put(request, "KEYBOARD", keyboard_name(payload->request_keyboard));
    list = put(list, "REQUEST", pv_nreverse(request));
    return pv_nreverse(list);
}

static pv convert_unmap(struct tomoe *s, const struct tomoe_event *e) {
    pv list = pv_nil();
    list = put(list, "TYPE", pv_keyword("unmap"));
    list = put(list, "ID", pv_uint(e->id));
    return pv_nreverse(list);
}

static pv key_state(uint32_t state) {
    return pv_keyword(state == POLICY_KEY_RELEASED ? "released" : "pressed");
}

static pv pointer_position(struct tomoe *s, const struct tomoe_event *e,
        pv list) {
    list = put(list, "X", pv_double(e->fx));
    list = put(list, "Y", pv_double(e->fy));
    list = put(list, "SX", pv_double(e->sx));
    list = put(list, "SY", pv_double(e->sy));
    return list;
}

static pv convert_key(struct tomoe *s, const struct tomoe_event *e) {
    pv list = pv_nil();
    list = put(list, "TYPE", pv_keyword("key"));
    list = put(list, "OWNER", text_of(s, e->str[0], ""));
    list = put(list, "COMMAND", text_of(s, e->str[1], ""));
    list = put(list, "STATE", key_state(e->state));
    list = put(list, "SOURCE-ID", pv_uint(e->source_id));
    if (e->binding_or_callback_id)
        list = put(list, "BINDING-ID", pv_uint(e->binding_or_callback_id));
    list = put(list, "DEVICE", pv_uint(e->time_us));
    if (e->flags & POLICY_KEY_KEYSYM) {
        list = put(list, "KEYCODE", pv_uint(e->keycode));
        list = put(list, "KEYSYM", optional_text(s, e->str[2]));
    } else if (e->flags & POLICY_KEY_POINTER) {
        list = put(list, "BUTTON", pv_uint(e->button));
        list = put(list, "WINDOW", pv_uint(e->id));
        list = put(list, "DELTA", pv_double(e->delta));
        list = pointer_position(s, e, list);
    } else {
        list = put(list, "KEYCODE", pv_uint(e->keycode));
        list = pointer_position(s, e, list);
    }
    return pv_nreverse(list);
}

static pv convert_button(struct tomoe *s, const struct tomoe_event *e) {
    pv list = pv_nil();
    list = put(list, "TYPE", pv_keyword("button"));
    list = put(list, "ID", pv_uint(e->id));
    list = put(list, "BUTTON", pv_uint(e->button));
    list = put(list, "STATE", key_state(e->state));
    list = put(list, "X", pv_int((int32_t)e->x));
    list = put(list, "Y", pv_int((int32_t)e->y));
    list = put(list, "MODIFIERS", pv_uint(e->modifiers));
    return pv_nreverse(list);
}

static pv convert_pointer(struct tomoe *s, const struct tomoe_event *e) {
    pv list = pv_nil();
    list = put(list, "TYPE", pv_keyword("pointer"));
    if (e->state == POLICY_POINTER_MOTION) {
        static const char *const sides[] = { "top", "bottom", "left", "right" };
        list = put(list, "STATE", pv_keyword("motion"));
        list = put(list, "ID", pv_uint(e->id));
        list = put(list, "MOVED", boolean(e->button != 0));
        list = put(list, "X", pv_int((int32_t)e->x));
        list = put(list, "Y", pv_int((int32_t)e->y));
        list = put(list, "DISTANCE", pv_int((int32_t)e->dx));
        pv entered = pv_nil(), left = pv_nil();
        pv edges_in = pv_nil(), edges_out = pv_nil();
        struct motion_state *m = &s->motion;
        for (size_t i = 0; i < m->plan.region_count && i < 64; i++) {
            if (e->source_id >> i & 1)
                entered = pv_cons(pv_cstring(m->plan.region[i].name), entered);
            if (e->binding_or_callback_id >> i & 1)
                left = pv_cons(pv_cstring(m->plan.region[i].name), left);
        }
        for (size_t i = 0; i < m->plan.edge_count && i < 4; i++) {
            if (e->keycode >> i & 1)
                edges_in = pv_cons(pv_keyword(sides[m->plan.edge[i].side]), edges_in);
            if (e->modifiers >> i & 1)
                edges_out = pv_cons(pv_keyword(sides[m->plan.edge[i].side]), edges_out);
        }
        list = put(list, "ENTERED", pv_nreverse(entered));
        list = put(list, "LEFT", pv_nreverse(left));
        list = put(list, "EDGES-IN", pv_nreverse(edges_in));
        list = put(list, "EDGES-OUT", pv_nreverse(edges_out));
    } else {
        list = put(list, "STATE", pv_keyword(e->state == POLICY_POINTER_LEAVE ?
            "leave" : "enter"));
        list = put(list, "ID", pv_uint(e->id));
        list = put(list, "MOVED", boolean(e->button != 0));
    }
    return pv_nreverse(list);
}

static pv convert_grab(struct tomoe *s, const struct tomoe_event *e) {
    static const char *const modes[] = { "", "move", "resize", "pointer" };
    pv list = pv_nil();
    list = put(list, "TYPE", pv_keyword("grab"));
    list = put(list, "ID", pv_uint(e->id));
    list = put(list, "MODE", pv_keyword(e->x < 4 ? modes[e->x] : modes[0]));
    list = put(list, "X", pv_int((int32_t)e->y));
    list = put(list, "Y", pv_int((int32_t)e->dx));
    list = put(list, "DX", pv_int((int32_t)e->fx));
    list = put(list, "DY", pv_int((int32_t)e->fy));
    return pv_nreverse(list);
}

static pv convert_activity(struct tomoe *s, const struct tomoe_event *e) {
    pv list = pv_nil();
    list = put(list, "TYPE", pv_keyword("activity"));
    list = put(list, "HAND", pv_cstring(e->id ? "right" : "left"));
    return pv_nreverse(list);
}

static pv convert_request(struct tomoe *s, const struct tomoe_event *e) {
    pv list = pv_nil();
    list = put(list, "TYPE", pv_keyword("request"));
    list = put(list, "ID", pv_uint(e->id));
    char request[64];
    list = put(list, "REQUEST", text_copy(s, e->str[0], request, sizeof(request)) ?
        pv_keyword(request) : pv_nil());
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
        list = put(list, "EDGES", pv_keyword(edge));
    }
    return pv_nreverse(list);
}

static pv convert_screenshot(struct tomoe *s, const struct tomoe_event *e) {
    pv list = pv_nil();
    list = put(list, "TYPE", pv_keyword("screenshot"));
    list = put(list, "PATH", text_of(s, e->str[0], ""));
    return pv_nreverse(list);
}

static const char *const selector_tools[] = { "select", "arrow", "text", "free" };
static const char *const selector_marks[] = { "arrow", "free", "text" };
static const char *const selector_phases[] = { "aim", "selecting", "reselect", "ready", "editing",
    "drawing", "typing", "typing" };

static pv convert_selector(struct tomoe *s, const struct tomoe_event *e) {
    pv list = pv_nil();
    list = put(list, "TYPE", pv_keyword("selector"));
    list = put(list, "OPEN", boolean(e->id != 0));
    if (e->id) {
        list = put(list, "OUTPUT", text_of(s, e->str[0], ""));
        list = put(list, "ANCHOR", pv_cons(pv_int(e->x), pv_cons(pv_int(e->y), pv_nil())));
        list = put(list, "CORNER", pv_cons(pv_int(e->dx), pv_cons(pv_int(e->dy), pv_nil())));
        list = put(list, "SELECTED", boolean(e->state & 1));
        list = put(list, "DRAGGING", boolean(e->state & 2));
        list = put(list, "FROZEN", boolean(e->state & 4));
        list = put(list, "TYPING", boolean(e->state & 8));
        list = put(list, "TOOL", pv_keyword(selector_tools[(e->state >> 4) & 3]));
        list = put(list, "COLOUR", pv_int((e->state >> 8) & 63));
        unsigned editing = (e->state >> 16) & 3;
        list = put(list, "EDITING", editing ? pv_keyword(selector_marks[editing - 1]) : pv_nil());
        list = put(list, "PHASE", pv_keyword(selector_phases[(e->state >> 20) & 7]));
        list = put(list, "SIZE", pv_double(e->fx));
    }
    return pv_nreverse(list);
}

static pv convert_ui(struct tomoe *s, const struct tomoe_event *e) {
    pv list = pv_nil();
    list = put(list, "TYPE", pv_keyword("ui"));
    list = put(list, "OWNER", text_of(s, e->str[0], ""));
    list = put(list, "SOURCE-ID", pv_uint(e->source_id));
    list = put(list, "CALLBACK-ID", pv_uint(e->binding_or_callback_id));
    list = put(list, "SURFACE", text_of(s, e->str[1], ""));
    list = put(list, "OUTPUT", text_of(s, e->str[2], ""));
    list = put(list, "ELEMENT", text_of(s, e->str[3], ""));
    list = put(list, "COMMAND", text_of(s, (uint32_t)e->time_us, ""));
    list = put(list, "X", pv_double(e->fx));
    list = put(list, "Y", pv_double(e->fy));
    list = put(list, "BUTTON", pv_uint(e->button));
    list = put(list, "MODIFIERS", pv_uint(e->modifiers));
    return pv_nreverse(list);
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

pv policy_event_value(struct tomoe *s, const struct tomoe_event *event, pv *aux) {
    *aux = pv_nil();
    if (!policy_event_delivers(s, event->type) || !converters[event->type]) return pv_nil();
    pv value = converters[event->type](s, event);
    if (!pv_nilp(value) && (event->type == EV_MAP || event->type == EV_METADATA)) {
        const char *identifier = tomoe_window_identifier(s, event->id);
        *aux = pv_list(3, pv_uint(event->id), identifier ? pv_cstring(identifier) : pv_nil(),
            pv_bool(res_background_window_p((int32_t)event->button)));
    }
    return value;
}

static int number(pv value) {
    if (pv_intp(value)) return (int)pv_get_int(value);
    return pv_numberp(value) ? (int)pv_get_double(value) : 0;
}

static int64_t integer(pv value) {
    if (pv_intp(value)) return pv_get_int(value);
    return pv_numberp(value) ? (int64_t)pv_get_double(value) : 0;
}

static uint64_t unsigned_integer(pv value) {
    return (uint64_t)integer(value);
}

static double real(pv value) {
    if (pv_intp(value)) return (double)pv_get_int(value);
    return pv_numberp(value) ? pv_get_double(value) : 0.0;
}

static bool truth(pv value) {
    return pv_intp(value) ? pv_get_int(value) != 0 : !pv_nilp(value);
}

static const char *string(pv value) {
    if (!pv_stringp(value)) return "";
    const char *text = pv_get_string(value, NULL);
    return text ? text : "";
}

static const char *nullable_string(pv value) {
    return pv_nilp(value) ? NULL : string(value);
}

static pv status(int value) {
    return pv_int(value);
}

static pv message(const char *text) {
    return text ? pv_cstring(text) : pv_nil();
}

static pv data(const char *snapshot) {
    if (!snapshot) return pv_nil();
    return pv_read(snapshot, strlen(snapshot));
}

static pv prim_abi(int argc, pv *argv) {
    return status(tomoe_abi_version());
}

static pv prim_activation_token(int argc, pv *argv) {
    return message(tomoe_activation_token(server()));
}

static pv prim_activation_revoke(int argc, pv *argv) {
    tomoe_activation_revoke(server(), string(argv[0]));
    return pv_nil();
}

static pv prim_hit_test(int argc, pv *argv) {
    return data(tomoe_hit_test(server(), real(argv[0]), real(argv[1])));
}

static pv prim_frames(int argc, pv *argv) {
    return data(tomoe_frames(server()));
}

static pv prim_animation(int argc, pv *argv) {
    return data(tomoe_animation(server()));
}

static pv prim_trim_heap(int argc, pv *argv) {
    tomoe_trim_heap();
    return pv_nil();
}

static pv prim_timings(int argc, pv *argv) {
    return data(tomoe_timings());
}

static pv prim_timing_enabled(int argc, pv *argv) {
    return status(tomoe_timing_enabled());
}

static pv prim_timing_now(int argc, pv *argv) {
    return pv_uint(tomoe_timing_now());
}

static pv prim_timing_channel(int argc, pv *argv) {
    return status(tomoe_timing_channel(string(argv[0])));
}

static pv prim_timing_reset(int argc, pv *argv) {
    tomoe_timing_reset();
    return pv_nil();
}

static pv prim_timing_sample(int argc, pv *argv) {
    tomoe_timing_sample(number(argv[0]), unsigned_integer(argv[1]));
    return pv_nil();
}

static pv prim_cursor_state(int argc, pv *argv) {
    return data(tomoe_cursor_state(server(), unsigned_integer(argv[0])));
}

static pv prim_cursor_motion(int argc, pv *argv) {
    return data(tomoe_cursor_motion(server()));
}

static pv prim_cursor_apply(int argc, pv *argv) {
    return status(tomoe_cursor_apply(server(), real(argv[0]), real(argv[1]), real(argv[2]),
        real(argv[3]), real(argv[4]), real(argv[5]), real(argv[6])));
}

static pv prim_input_stream(int argc, pv *argv) {
    return status(tomoe_input_stream(server(), truth(argv[0])));
}

static pv prim_close(int argc, pv *argv) {
    tomoe_close(server(), (uint32_t)unsigned_integer(argv[0]));
    return pv_nil();
}

static pv prim_window_identifier(int argc, pv *argv) {
    return message(tomoe_window_identifier(server(), (uint32_t)unsigned_integer(argv[0])));
}

static pv prim_clipboard_copy(int argc, pv *argv) {
    return status(tomoe_clipboard_copy(server(), string(argv[0]), string(argv[1]),
        truth(argv[2])));
}

static pv prim_screenshot(int argc, pv *argv) {
    tomoe_screenshot(server(), truth(argv[0]));
    return pv_nil();
}

static pv prim_keysym(int argc, pv *argv) {
    return pv_uint(tomoe_keysym(string(argv[0])));
}

static pv prim_warm_shaders(int argc, pv *argv) {
    return status(tomoe_warm_shaders(string(argv[0]), string(argv[1]), "", NULL));
}

static pv prim_outputs_begin(int argc, pv *argv) {
    return status(tomoe_outputs_begin(server()));
}

static pv prim_outputs_pending(int argc, pv *argv) {
    return status(tomoe_outputs_pending(server()));
}

static pv prim_output_power(int argc, pv *argv) {
    return status(tomoe_output_power(server(), nullable_string(argv[0]), number(argv[1])));
}

static pv prim_virtual_output(int argc, pv *argv) {
    tomoe_virtual_output(server(), string(argv[0]), number(argv[1]), number(argv[2]),
        number(argv[3]));
    return pv_nil();
}

static pv prim_output_hold(int argc, pv *argv) {
    return status(tomoe_output_hold(server(), string(argv[0])));
}

static pv prim_output(int argc, pv *argv) {
    return status(tomoe_output(server(), string(argv[0]), number(argv[1]), number(argv[2]),
        number(argv[3]), number(argv[4]), number(argv[5]), number(argv[6]), number(argv[7]),
        truth(argv[8]), number(argv[9])));
}

static pv prim_output_options(int argc, pv *argv) {
    return status(tomoe_output_options(server(), string(argv[0]), truth(argv[1]),
        string(argv[2]), truth(argv[3]), truth(argv[4])));
}

static pv prim_output_icc(int argc, pv *argv) {
    return status(tomoe_output_icc(server(), string(argv[0]), string(argv[1])));
}

static pv prim_outputs_preview(int argc, pv *argv) {
    return data(tomoe_outputs_preview(server()));
}

static pv prim_outputs_current(int argc, pv *argv) {
    return data(tomoe_outputs_current(server()));
}

static pv prim_layers_begin(int argc, pv *argv) {
    return status(tomoe_layers_begin(server(), truth(argv[0])));
}

static pv prim_layers_output(int argc, pv *argv) {
    return status(tomoe_layers_output(server(), string(argv[0]), number(argv[1]),
        number(argv[2]), number(argv[3]), number(argv[4]), number(argv[5])));
}

static pv prim_layers_surface(int argc, pv *argv) {
    return status(tomoe_layers_surface(server(), (uint32_t)unsigned_integer(argv[0]),
        number(argv[1]), number(argv[2]), number(argv[3]), number(argv[4])));
}

static pv prim_layers_preview(int argc, pv *argv) {
    return data(tomoe_layers_preview(server()));
}

static pv prim_present_begin(int argc, pv *argv) {
    return status(tomoe_present_begin(server(), number(argv[0]), number(argv[1]),
        real(argv[2]), (uint32_t)unsigned_integer(argv[3]), truth(argv[4]),
        truth(argv[5]), truth(argv[6]), (uint32_t)unsigned_integer(argv[7]),
        number(argv[8])));
}

static pv prim_present_bind(int argc, pv *argv) {
    return status(tomoe_present_bind(server(), (uint32_t)unsigned_integer(argv[0]),
        (uint32_t)unsigned_integer(argv[1]), string(argv[2]), string(argv[3]),
        string(argv[4]), unsigned_integer(argv[5])));
}

static pv prim_present_keyboard(int argc, pv *argv) {
    return status(tomoe_present_keyboard(server(), string(argv[0]), string(argv[1]),
        string(argv[2]), string(argv[3]), string(argv[4]), number(argv[5]),
        number(argv[6])));
}

static pv prim_present_window(int argc, pv *argv) {
    return status(tomoe_present_window(server(), (uint32_t)unsigned_integer(argv[0]),
        number(argv[1]), number(argv[2]), number(argv[3]), number(argv[4]),
        truth(argv[5]), truth(argv[6]), truth(argv[7])));
}

static pv prim_present_settings(int argc, pv *argv) {
    return status(tomoe_present_settings(server()));
}

static pv prim_present_setting(int argc, pv *argv) {
    return status(tomoe_present_setting(server(), string(argv[0]), real(argv[1])));
}

static pv prim_present_setting_text(int argc, pv *argv) {
    return status(tomoe_present_setting_text(server(), string(argv[0]), string(argv[1])));
}

static pv prim_present_window_style(int argc, pv *argv) {
    return status(tomoe_present_window_style(server(), (uint32_t)unsigned_integer(argv[0]),
        number(argv[1]), number(argv[2]), number(argv[3]), integer(argv[4]),
        integer(argv[5])));
}

static pv prim_present_window_visual(int argc, pv *argv) {
    return status(tomoe_present_window_visual(server(),
        (uint32_t)unsigned_integer(argv[0]), real(argv[1]),
        (uint32_t)unsigned_integer(argv[2]), real(argv[3]), real(argv[4]),
        number(argv[5]), number(argv[6])));
}



static pv prim_present_keyboard_grab(int argc, pv *argv) {
    return status(tomoe_present_keyboard_grab(server(), string(argv[0]),
        unsigned_integer(argv[1]), string(argv[2])));
}

static pv prim_motion(int argc, pv *argv) {
    return status(tomoe_motion(server(), truth(argv[0]), truth(argv[1]), truth(argv[2]),
        truth(argv[3]), number(argv[4])));
}

static pv prim_motion_region(int argc, pv *argv) {
    return status(tomoe_motion_region(server(), string(argv[0]), number(argv[1]),
        number(argv[2]), number(argv[3]), number(argv[4])));
}

static pv prim_motion_edge(int argc, pv *argv) {
    return status(tomoe_motion_edge(server(), number(argv[0]), number(argv[1])));
}

static pv prim_motion_drag(int argc, pv *argv) {
    return status(tomoe_motion_drag(server(), (uint32_t)unsigned_integer(argv[0]),
        number(argv[1]), number(argv[2]), number(argv[3]), number(argv[4])));
}

static pv prim_present_sounds(int argc, pv *argv) {
    return status(tomoe_present_sounds(server()));
}

static pv prim_present_sound(int argc, pv *argv) {
    return message(tomoe_present_sound(server(), number(argv[0]), string(argv[1]),
        real(argv[2])));
}

static pv prim_sound_stats(int argc, pv *argv) {
    return data(tomoe_sound_stats(server()));
}

static pv prim_native_errors(int argc, pv *argv) {
    return data(tomoe_native_errors());
}

static pv prim_present_apply(int argc, pv *argv) {
    return message(tomoe_present_apply(server()));
}

static pv prim_present_stack(int argc, pv *argv) {
    return status(tomoe_present_stack(server(), (uint32_t)unsigned_integer(argv[0])));
}

static pv prim_present_abort(int argc, pv *argv) {
    tomoe_present_abort(server());
    return pv_nil();
}

static pv prim_event_barrier(int argc, pv *argv) {
    return status(policy_event_barrier(server()));
}

static pv prim_set_reads(int argc, pv *argv) {
    struct tomoe *s = server();
    uint64_t mask = 0;
    for (pv tail = argv[0]; pv_consp(tail); tail = pv_cdr(tail)) {
        pv item = pv_car(tail);
        if (!pv_keywordp(item) && !pv_symbolp(item)) continue;
        const char *name = pv_symbol_name(item);
        if (!name) continue;
        for (int i = 0; i < POLICY_KEY_COUNT; i++)
            if (strcasecmp(name, policy_keys[i]) == 0) mask |= UINT64_C(1) << i;
    }
    s->event_read_mask = mask;
    return pv_nil();
}

static int log_level(const char *name) {
    if (strcasecmp(name, "DEBUG") == 0) return LOG_DEBUG;
    if (strcasecmp(name, "INFO") == 0) return LOG_INFO;
    if (strcasecmp(name, "WARNING") == 0 || strcasecmp(name, "WARN") == 0)
        return LOG_WARNING;
    return LOG_ERROR;
}

static pv prim_log(int argc, pv *argv) {
    const char *level = pv_keywordp(argv[0]) || pv_symbolp(argv[0]) ?
        pv_symbol_name(argv[0]) : string(argv[0]);
    tomoe_log(log_level(level ? level : "INFO"), "%s", string(argv[1]));
    return pv_nil();
}

static pv prim_fence(int argc, pv *argv) {
    return pv_nil();
}

static pv prim_memory(int argc, pv *argv) {
    return tomoe_memory_report(server());
}

static const struct primitive {
    const char *name;
    prim_fn fn;
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
    { "%cursor-state", prim_cursor_state, 1, 1 },
    { "%cursor-motion", prim_cursor_motion, 0, 0 },
    { "%cursor-apply", prim_cursor_apply, 7, 7 },
    { "%input-stream", prim_input_stream, 1, 1 },
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
    { "%present-keyboard-grab", prim_present_keyboard_grab, 3, 3 },
    { "%motion", prim_motion, 5, 5 },
    { "%motion-region", prim_motion_region, 5, 5 },
    { "%motion-edge", prim_motion_edge, 2, 2 },
    { "%motion-drag", prim_motion_drag, 5, 5 },
    { "%present-sounds", prim_present_sounds, 0, 0 },
    { "%present-sound", prim_present_sound, 3, 3 },
    { "%sound-stats", prim_sound_stats, 0, 0 },
    { "%native-errors", prim_native_errors, 0, 0 },
    { "%fence", prim_fence, 0, 0 },
    { "%memory", prim_memory, 0, 0 },
    { "%present-apply", prim_present_apply, 0, 0 },
    { "%present-stack", prim_present_stack, 1, 1 },
    { "%present-abort", prim_present_abort, 0, 0 },
    { "%event-barrier", prim_event_barrier, 0, 0 },
    { "%set-reads", prim_set_reads, 1, 1 },
    { "%log", prim_log, 2, 2 },
};

void policy_register_primitives(struct tomoe *server) {
    policy_server = server;
    for (size_t i = 0; i < sizeof(primitives) / sizeof(primitives[0]); i++)
        prim_define(primitives[i].name, primitives[i].fn,
            primitives[i].min, primitives[i].max);
    policy_set_converter(EV_OUTPUTS, outputs_event_value);
}

static const struct prim_spec {
    const char *name;
    int min, max, cls;
} prim_specs[TPL_PRIM_COUNT] = {
#define TPL_SPEC(id, name, min, max, cls) [TPL_PRIM_##id] = { name, min, max, cls },
    TOMOE_PRIMITIVES(TPL_SPEC)
#undef TPL_SPEC
};

static prim_fn prim_table[TPL_PRIM_COUNT];

int policy_primitive_index(const char *name) {
    for (int i = 0; i < TPL_PRIM_COUNT; i++)
        if (!strcmp(prim_specs[i].name, name)) return i;
    return -1;
}

void prim_define(const char *name, prim_fn fn, int min_args, int max_args) {
    int index = policy_primitive_index(name);
    if (index < 0 || prim_specs[index].min != min_args || prim_specs[index].max != max_args) {
        tomoe_log(LOG_ERROR, "tomoe: primitive %s does not match the policy protocol", name);
        abort();
    }
    prim_table[index] = fn;
}

pv policy_call_primitive(int prim, int argc, pv *argv) {
    if (prim < 0 || prim >= TPL_PRIM_COUNT || !prim_table[prim])
        pv_error("Unknown native primitive %d.", prim);
    const struct prim_spec *spec = &prim_specs[prim];
    if (argc < spec->min || (spec->max >= 0 && argc > spec->max))
        pv_error("%s takes %d to %d arguments, not %d.", spec->name, spec->min, spec->max, argc);
    pv result = prim_table[prim](argc, argv);
    if (spec->cls == TPL_CHECK && !(pv_intp(result) && pv_get_int(result) == 1))
        pv_error("%s was refused by the compositor.", spec->name);
    return result;
}
