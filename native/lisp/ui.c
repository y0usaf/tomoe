#include <math.h>
#include <float.h>
#include <string.h>

#include "lisp-internal.h"
#include "../ui-shape.h"

enum ui_key {
    UK_KIND, UK_WIDTH, UK_HEIGHT, UK_GROW, UK_BACKGROUND, UK_BORDER_COLOR, UK_RADIUS, UK_BORDER,
    UK_KEY, UK_ON_CLICK, UK_ON_HOVER, UK_CHILDREN, UK_GAP, UK_PADDING, UK_ALIGN, UK_JUSTIFY,
    UK_TEXT, UK_SIZE, UK_LINE_HEIGHT, UK_COLOR, UK_FONT, UK_ROTATE, UK_SRC, UK_NAME, UK_PATH,
    UK_ORIENTATION, UK_THICKNESS, UK_VALUE, UK_TRACK, UK_POINTS, UK_SHADOW, UK_COUNT
};

enum ui_kind {
    UI_ROW, UI_COLUMN, UI_BUTTON, UI_STACK, UI_TEXT, UI_IMAGE, UI_ICON, UI_RECT, UI_SPACER,
    UI_SEPARATOR, UI_PROGRESS, UI_CIRCULAR_PROGRESS, UI_SHAPE, UI_KIND_COUNT
};

enum ui_word {
    UW_START, UW_CENTER, UW_END, UW_STRETCH, UW_HORIZONTAL, UW_VERTICAL, UW_COUNT
};

static const char *const key_names[UK_COUNT] = {
    "KIND", "WIDTH", "HEIGHT", "GROW", "BACKGROUND", "BORDER-COLOR", "RADIUS", "BORDER",
    "KEY", "ON-CLICK", "ON-HOVER", "CHILDREN", "GAP", "PADDING", "ALIGN", "JUSTIFY",
    "TEXT", "SIZE", "LINE-HEIGHT", "COLOR", "FONT", "ROTATE", "SRC", "NAME", "PATH",
    "ORIENTATION", "THICKNESS", "VALUE", "TRACK", "POINTS", "SHADOW"
};

static const char *const kind_names[UI_KIND_COUNT] = {
    "ROW", "COLUMN", "BUTTON", "STACK", "TEXT", "IMAGE", "ICON", "RECT", "SPACER",
    "SEPARATOR", "PROGRESS", "CIRCULAR-PROGRESS", "SHAPE"
};

static const char *const word_names[UW_COUNT] = {
    "START", "CENTER", "END", "STRETCH", "HORIZONTAL", "VERTICAL"
};

static const uint8_t common_keys[] = {
    UK_WIDTH, UK_HEIGHT, UK_GROW, UK_BACKGROUND, UK_BORDER_COLOR, UK_RADIUS, UK_BORDER,
    UK_KEY, UK_ON_CLICK, UK_ON_HOVER, UK_SHADOW
};

static const uint8_t container_keys[] = { UK_CHILDREN, UK_GAP, UK_PADDING, UK_ALIGN, UK_JUSTIFY };
static const uint8_t stack_keys[] = { UK_CHILDREN };
static const uint8_t text_keys[] = { UK_TEXT, UK_SIZE, UK_LINE_HEIGHT, UK_COLOR, UK_FONT, UK_ROTATE };
static const uint8_t image_keys[] = { UK_SRC };
static const uint8_t icon_keys[] = { UK_NAME, UK_PATH, UK_SIZE, UK_COLOR };
static const uint8_t separator_keys[] = { UK_ORIENTATION, UK_THICKNESS, UK_COLOR };
static const uint8_t progress_keys[] = { UK_VALUE, UK_COLOR, UK_TRACK };
static const uint8_t circular_keys[] = { UK_SIZE, UK_THICKNESS, UK_VALUE, UK_COLOR, UK_TRACK };
static const uint8_t shape_keys[] = { UK_POINTS };

struct key_set {
    const uint8_t *keys;
    int count;
};

static const struct key_set kind_keys[UI_KIND_COUNT] = {
    [UI_ROW] = { container_keys, 5 },
    [UI_COLUMN] = { container_keys, 5 },
    [UI_BUTTON] = { container_keys, 5 },
    [UI_STACK] = { stack_keys, 1 },
    [UI_TEXT] = { text_keys, 6 },
    [UI_IMAGE] = { image_keys, 1 },
    [UI_ICON] = { icon_keys, 4 },
    [UI_RECT] = { NULL, 0 },
    [UI_SPACER] = { NULL, 0 },
    [UI_SEPARATOR] = { separator_keys, 3 },
    [UI_PROGRESS] = { progress_keys, 3 },
    [UI_CIRCULAR_PROGRESS] = { circular_keys, 5 },
    [UI_SHAPE] = { shape_keys, 1 },
};

static struct {
    lisp_value keys[UK_COUNT];
    lisp_value kinds[UI_KIND_COUNT];
    lisp_value words[UW_COUNT];
    lisp_value zero, one, two, four, sixteen;
    lisp_value padding, empty, sans;
    lisp_value separator_color, progress_color, track_color;
    lisp_value align_choices, justify_choices, orientation_choices, rotate_choices;
    lisp_value node_text, children_text, edges_text, properties_text;
    lisp_value points_text, shadow_text, stops_text;
} ui;

static lisp_value keyword(const char *name)
{
    return VAL(symbol_intern(PKG_KEYWORD, name, strlen(name), false));
}

void lisp_ui_init(void)
{
    for (int i = 0; i < UK_COUNT; i++)
        ui.keys[i] = keyword(key_names[i]);
    for (int i = 0; i < UI_KIND_COUNT; i++)
        ui.kinds[i] = keyword(kind_names[i]);
    for (int i = 0; i < UW_COUNT; i++)
        ui.words[i] = keyword(word_names[i]);
    ui.zero = make_double(0.0);
    ui.one = make_double(1.0);
    ui.two = make_double(2.0);
    ui.four = make_double(4.0);
    ui.sixteen = make_double(16.0);
    ui.padding = LIST4(ui.zero, ui.zero, ui.zero, ui.zero);
    ui.empty = make_string_len("", 0);
    ui.sans = make_string_len("sans", 4);
    ui.separator_color = make_int(0x45475aff);
    ui.progress_color = make_int(0x89b4faff);
    ui.track_color = make_int(0x313244ff);
    ui.align_choices = LIST4(ui.words[UW_START], ui.words[UW_CENTER], ui.words[UW_END], ui.words[UW_STRETCH]);
    ui.justify_choices = LIST3(ui.words[UW_START], ui.words[UW_CENTER], ui.words[UW_END]);
    ui.orientation_choices = LIST2(ui.words[UW_HORIZONTAL], ui.words[UW_VERTICAL]);
    ui.rotate_choices = LIST4(make_int(0), make_int(90), make_int(180), make_int(270));
    ui.node_text = make_string_len("UI node", 7);
    ui.children_text = make_string_len("UI children", 11);
    ui.edges_text = make_string_len("UI edges", 8);
    ui.properties_text = make_string_len("UI properties", 13);
    ui.points_text = make_string_len("UI shape points", 15);
    ui.shadow_text = make_string_len("UI shape shadow", 15);
    ui.stops_text = make_string_len("UI gradient stops", 17);
}

static bool finite_double(double v)
{
    return v == v && fabs(v) <= DBL_MAX;
}

static bool finite_real(lisp_value v)
{
    uint8_t t = TYPE(v);
    if (t == LT_INT || t == LT_RATIO)
        return true;
    return t == LT_DOUBLE && finite_double(DBLVAL(v));
}

static lisp_value as_double(lisp_value value)
{
    if (!finite_real(value))
        lisp_fail_format("Expected a finite real number, got ~S.", 1, &value);
    double result = number_to_double(value);
    if (!finite_double(result))
        lisp_fail_format("Real number cannot be represented as a finite double-float: ~S.", 1, &value);
    return make_double(result);
}

static lisp_value ui_list(lisp_value value, int64_t limit, lisp_value description)
{
    int64_t count = 0;
    lisp_value tail = value;
    while (consp(tail)) {
        if (++count > limit)
            lisp_fail_format("~A exceeds its list budget.", 1, &description);
        tail = CDR(tail);
    }
    if (!nilp(tail))
        lisp_fail_format("~A must be a proper list.", 1, &description);
    return value;
}

static lisp_value ui_length(lisp_value value, bool nullable, bool is_signed)
{
    if (nullable && nilp(value))
        return NIL_VAL;
    lisp_value number = as_double(value);
    double d = DBLVAL(number);
    if (!((is_signed ? -16384.0 : 0.0) <= d && d <= 16384.0))
        lisp_fail_format("UI length is outside its supported range: ~S.", 1, &value);
    return number;
}

static lisp_value ui_string(lisp_value value, int64_t limit, bool empty)
{
    if (stringp(value)) {
        struct lisp_string *s = PTR(value, lisp_string);
        if ((int64_t)s->len <= limit && (empty || s->len > 0) && !memchr(s->data, 0, s->byte_len))
            return value;
    }
    lisp_fail_format("Expected a bounded UI string without NUL characters.", 0, NULL);
}

static lisp_value ui_key(lisp_value value, bool nullable)
{
    if (nullable && nilp(value))
        return NIL_VAL;
    if (!symbolp(value) || !(SYM(value)->h.flags & SYM_KEYWORD))
        lisp_fail_format("UI names and commands must be keywords.", 0, NULL);
    struct lisp_symbol *sym = SYM(value);
    if (utf8_strlen(sym->name, sym->name_len) > 128 || sym->name_len == 0 ||
            memchr(sym->name, 0, sym->name_len))
        lisp_fail_format("Expected a bounded UI string without NUL characters.", 0, NULL);
    return value;
}

static lisp_value ui_edges(lisp_value value, bool is_signed)
{
    if (finite_real(value)) {
        lisp_value edge = ui_length(value, false, is_signed);
        return LIST4(edge, edge, edge, edge);
    }
    ui_list(value, 4, ui.edges_text);
    if (list_length(value) != 4)
        lisp_fail_format("UI edges need top/right/bottom/left.", 0, NULL);
    lisp_value a = ui_length(CAR(value), false, is_signed);
    lisp_value b = ui_length(CAR(CDR(value)), false, is_signed);
    lisp_value c = ui_length(CAR(CDR(CDR(value))), false, is_signed);
    lisp_value d = ui_length(CAR(CDR(CDR(CDR(value)))), false, is_signed);
    return LIST4(a, b, c, d);
}

static lisp_value ui_choice(lisp_value value, lisp_value choices)
{
    for (lisp_value scan = choices; consp(scan); scan = CDR(scan))
        if (eql_value(CAR(scan), value))
            return value;
    lisp_value args[2] = { value, choices };
    lisp_fail_format("Unsupported UI value ~S; expected one of ~S.", 2, args);
}

static int hex_digit(uint32_t cp)
{
    if (cp >= '0' && cp <= '9')
        return (int)(cp - '0');
    if (cp >= 'a' && cp <= 'f')
        return (int)(cp - 'a' + 10);
    if (cp >= 'A' && cp <= 'F')
        return (int)(cp - 'A' + 10);
    return -1;
}

static lisp_value ui_color(lisp_value value, bool nullable)
{
    if (nullable && nilp(value))
        return NIL_VAL;
    if (intp(value) && INTVAL(value) >= 0 && INTVAL(value) <= 4294967295LL)
        return value;
    if (stringp(value)) {
        uint32_t len = PTR(value, lisp_string)->len;
        if ((len == 4 || len == 7 || len == 9) && string_char_at(value, 0) == '#') {
            int64_t number = 0;
            bool hex = true;
            for (uint32_t i = 1; i < len && hex; i++) {
                int digit = hex_digit(string_char_at(value, i));
                if (digit < 0)
                    hex = false;
                else
                    number = number * 16 + digit;
            }
            if (hex) {
                if (len == 4)
                    return make_int(((17 * ((number >> 8) & 15)) << 24) | ((17 * ((number >> 4) & 15)) << 16) |
                        ((17 * (number & 15)) << 8) | 255);
                if (len == 7)
                    return make_int((number << 8) | 255);
                return make_int(number);
            }
        }
    }
    lisp_fail_format("UI color must be RRGGBBAA or #rgb/#rrggbb/#rrggbbaa.", 0, NULL);
}

static lisp_value ui_gradient(lisp_value value)
{
    ui_list(value, 14, ui.stops_text);
    lisp_value kind = CAR(value);
    bool linear = eql_value(kind, keyword("LINEAR"));
    if (!linear && !eql_value(kind, keyword("RADIAL")))
        lisp_fail_format("UI background must be a color, (:linear ANGLE (POS COLOR)...) or (:radial CX CY RX RY (POS COLOR)...).", 0, NULL);
    int axes = linear ? 1 : 4;
    lisp_value scan = CDR(value);
    lisp_value head = NIL_VAL, last = NIL_VAL;
#define UI_CONS(value) do { lisp_value cell_ = cons((value), NIL_VAL); \
        if (nilp(head)) head = cell_; else CDR(last) = cell_; last = cell_; } while (0)
    UI_CONS(kind);
    for (int i = 0; i < axes; i++) {
        if (!consp(scan))
            lisp_fail_format("UI gradient is missing its axis.", 0, NULL);
        UI_CONS(as_double(CAR(scan)));
        scan = CDR(scan);
    }
    int stops = 0;
    double previous = -1.0;
    while (consp(scan)) {
        if (++stops > UI_SHAPE_STOPS)
            lisp_fail_format("UI gradients take at most 6 stops.", 0, NULL);
        lisp_value stop = CAR(scan), at_value, color_value;
        if (consp(stop)) {
            ui_list(stop, 2, ui.stops_text);
            if (list_length(stop) != 2)
                lisp_fail_format("UI gradient stops are (POSITION COLOR).", 0, NULL);
            at_value = CAR(stop);
            color_value = CAR(CDR(stop));
            scan = CDR(scan);
        } else {
            if (!consp(CDR(scan)))
                lisp_fail_format("UI gradient stops are (POSITION COLOR).", 0, NULL);
            at_value = stop;
            color_value = CAR(CDR(scan));
            scan = CDR(CDR(scan));
        }
        double at = DBLVAL(as_double(at_value));
        if (!(at >= 0.0 && at <= 1.0))
            lisp_fail_format("UI gradient stop positions are fractions from 0 to 1.", 0, NULL);
        if (at < previous)
            lisp_fail_format("UI gradient stop positions must not decrease.", 0, NULL);
        previous = at;
        UI_CONS(make_double(at));
        UI_CONS(ui_color(color_value, false));
    }
    if (!nilp(scan) || stops < 2)
        lisp_fail_format("UI gradients take at least 2 stops.", 0, NULL);
    return head;
#undef UI_CONS
}

static lisp_value ui_points(lisp_value value)
{
    int64_t count = 0;
    lisp_value tail = value;
    for (; consp(tail); tail = CDR(tail)) {
        if (++count > 2 * UI_SHAPE_POINTS)
            lisp_fail_format("A UI shape takes at most 1024 points.", 0, NULL);
        if (!finite_real(CAR(tail)))
            lisp_fail_format("UI shape points are finite real numbers.", 1, &tail);
    }
    if (!nilp(tail) || count % 2 != 0 || count < 6)
        lisp_fail_format("UI shape points are x/y pairs, at least three of them, in a proper list.", 0, NULL);
    lisp_value head = NIL_VAL, last = NIL_VAL;
    for (lisp_value scan = value; consp(scan); scan = CDR(scan)) {
        lisp_value cell = cons(as_double(CAR(scan)), NIL_VAL);
        if (nilp(head)) head = cell; else CDR(last) = cell;
        last = cell;
    }
    return head;
}

static lisp_value ui_shadow(lisp_value value)
{
    ui_list(value, UI_SHAPE_SHADOWS, ui.shadow_text);
    lisp_value head = NIL_VAL, last = NIL_VAL;
    for (lisp_value scan = value; consp(scan); scan = CDR(scan)) {
        lisp_value entry = CAR(scan);
        ui_list(entry, 5, ui.shadow_text);
        if (list_length(entry) != 5)
            lisp_fail_format("UI shadows are (X Y BLUR SPREAD COLOR).", 0, NULL);
        lisp_value shadow = NIL_VAL, tail = NIL_VAL;
        lisp_value numbers = entry;
        for (int i = 0; i < 4; i++) {
            lisp_value number = as_double(CAR(numbers));
            if (i >= 2 && DBLVAL(number) < 0.0)
                lisp_fail_format("UI shadow blur and spread are not negative.", 1, &number);
            lisp_value cell = cons(number, NIL_VAL);
            if (nilp(shadow)) shadow = cell; else CDR(tail) = cell;
            tail = cell;
            numbers = CDR(numbers);
        }
        lisp_value color = cons(ui_color(CAR(numbers), false), NIL_VAL);
        CDR(tail) = color;
        lisp_value wrapper = cons(shadow, NIL_VAL);
        if (nilp(head)) head = wrapper; else CDR(last) = wrapper;
        last = wrapper;
    }
    return head;
}

static int kind_index(lisp_value kind)
{
    for (int i = 0; i < UI_KIND_COUNT; i++)
        if (ui.kinds[i] == kind)
            return i;
    return -1;
}

static int key_index(lisp_value key)
{
    for (int i = 0; i < UK_COUNT; i++)
        if (ui.keys[i] == key)
            return i;
    return -1;
}

static lisp_value ui_default(int kind, int key)
{
    switch (key) {
    case UK_HEIGHT: return kind == UI_PROGRESS ? ui.four : NIL_VAL;
    case UK_GROW: return kind == UI_SPACER ? ui.one : ui.zero;
    case UK_RADIUS: return kind == UI_PROGRESS ? ui.two : ui.zero;
    case UK_BORDER: return ui.zero;
    case UK_GAP: return kind == UI_BUTTON ? ui.four : ui.zero;
    case UK_PADDING: return ui.padding;
    case UK_ALIGN: return ui.words[kind == UI_ROW || kind == UI_BUTTON ? UW_CENTER : UW_START];
    case UK_JUSTIFY: return ui.words[UW_START];
    case UK_TEXT: return ui.empty;
    case UK_FONT: return ui.sans;
    case UK_ROTATE: return make_int(0);
    case UK_SIZE: return kind == UI_ICON || kind == UI_CIRCULAR_PROGRESS ? ui.sixteen : NIL_VAL;
    case UK_ORIENTATION: return ui.words[UW_HORIZONTAL];
    case UK_THICKNESS: return kind == UI_CIRCULAR_PROGRESS ? ui.two : ui.one;
    case UK_COLOR:
        if (kind == UI_SEPARATOR)
            return ui.separator_color;
        return kind == UI_PROGRESS || kind == UI_CIRCULAR_PROGRESS ? ui.progress_color : NIL_VAL;
    case UK_TRACK: return ui.track_color;
    case UK_VALUE: return ui.zero;
    default: return NIL_VAL;
    }
}

static bool key_allowed(int kind, int key)
{
    if (key == UK_KIND)
        return true;
    for (size_t i = 0; i < sizeof(common_keys); i++)
        if (common_keys[i] == key)
            return true;
    const struct key_set *set = &kind_keys[kind];
    for (int i = 0; i < set->count; i++)
        if (set->keys[i] == key)
            return true;
    return false;
}

struct ui_walk {
    int count;
};

static lisp_value walk_node(struct ui_walk *walk, lisp_value node, int depth);

static lisp_value normalize(struct ui_walk *walk, int kind, int key, lisp_value value, int depth)
{
    switch (key) {
    case UK_WIDTH:
    case UK_HEIGHT:
    case UK_LINE_HEIGHT:
        return ui_length(value, true, false);
    case UK_SIZE:
        return ui_length(value, kind == UI_TEXT, false);
    case UK_GROW:
    case UK_RADIUS:
    case UK_BORDER:
    case UK_GAP:
    case UK_THICKNESS:
        return ui_length(value, false, false);
    case UK_BACKGROUND:
        return consp(value) ? ui_gradient(value) : ui_color(value, true);
    case UK_BORDER_COLOR:
    case UK_COLOR:
        return ui_color(value, true);
    case UK_POINTS:
        return ui_points(value);
    case UK_SHADOW:
        return ui_shadow(value);
    case UK_TRACK:
        return ui_color(value, false);
    case UK_KEY:
    case UK_ON_CLICK:
    case UK_ON_HOVER:
        return ui_key(value, true);
    case UK_PADDING:
        return ui_edges(value, false);
    case UK_ALIGN:
        return ui_choice(value, ui.align_choices);
    case UK_JUSTIFY:
        return ui_choice(value, ui.justify_choices);
    case UK_ORIENTATION:
        return ui_choice(value, ui.orientation_choices);
    case UK_ROTATE:
        return ui_choice(value, ui.rotate_choices);
    case UK_TEXT:
        return ui_string(value, 65536, true);
    case UK_FONT:
        return ui_string(value, 128, false);
    case UK_SRC:
    case UK_NAME:
        return ui_string(value, 65536, false);
    case UK_PATH:
        return nilp(value) ? NIL_VAL : ui_string(value, 65536, true);
    case UK_VALUE: {
        double d = DBLVAL(as_double(value));
        if (!(d < 1.0))
            d = 1.0;
        if (!(d > 0.0))
            d = 0.0;
        return make_double(d);
    }
    case UK_CHILDREN: {
        ui_list(value, 512, ui.children_text);
        lisp_value head = NIL_VAL, last = NIL_VAL;
        for (lisp_value scan = value; consp(scan); scan = CDR(scan)) {
            lisp_value cell = cons(walk_node(walk, CAR(scan), depth + 1), NIL_VAL);
            if (nilp(head))
                head = cell;
            else
                CDR(last) = cell;
            last = cell;
        }
        return head;
    }
    default:
        return NIL_VAL;
    }
}

static void check_properties(lisp_value properties, int kind)
{
    lisp_value seen[32];
    int seen_count = 0;
    for (lisp_value scan = properties; consp(scan); scan = consp(CDR(scan)) ? CDR(CDR(scan)) : NIL_VAL) {
        lisp_value key = CAR(scan);
        int index = key_index(key);
        if (index < 0 || !key_allowed(kind, index))
            lisp_fail_format("Unsupported UI property ~S.", 1, &key);
        for (int i = 0; i < seen_count; i++)
            if (seen[i] == key)
                lisp_fail_format("Duplicate UI property ~S.", 1, &key);
        seen[seen_count++] = key;
    }
}

static lisp_value walk_node(struct ui_walk *walk, lisp_value node, int depth)
{
    if (++walk->count > 512 || depth > 32)
        lisp_fail_format("UI tree exceeds 512 nodes or depth 32.", 0, NULL);
    ui_list(node, 64, ui.node_text);
    lisp_value kind_value = keyword_get(node, ui.keys[UK_KIND], NIL_VAL);
    int kind = kind_index(kind_value);
    if (kind < 0)
        lisp_fail_format("Unsupported UI kind ~S.", 1, &kind_value);
    if (list_length(node) % 2 != 0)
        lisp_fail_format("UI properties need key/value pairs.", 0, NULL);
    check_properties(node, kind);
    lisp_value result = LIST2(ui.keys[UK_KIND], kind_value);
    lisp_value last = CDR(result);
    const struct key_set *extra = &kind_keys[kind];
    int total = (int)sizeof(common_keys) + extra->count;
    for (int i = 0; i < total; i++) {
        int key = i < (int)sizeof(common_keys) ? common_keys[i] : extra->keys[i - (int)sizeof(common_keys)];
        lisp_value fallback = ui_default(kind, key);
        lisp_value value = keyword_get(node, ui.keys[key], fallback);
        lisp_value normalized = normalize(walk, kind, key, value, depth);
        if (!equal_value(normalized, fallback)) {
            lisp_value tail = LIST2(ui.keys[key], normalized);
            CDR(last) = tail;
            last = CDR(tail);
        }
    }
    return result;
}

lisp_value builtin_canonical_ui(int argc, lisp_value *argv)
{
    struct ui_walk walk = { 0 };
    return walk_node(&walk, argv[0], 1);
}

lisp_value builtin_ui(int argc, lisp_value *argv)
{
    lisp_value node = NIL_VAL;
    for (int i = argc - 1; i >= 1; i--)
        node = cons(argv[i], node);
    node = cons(ui.keys[UK_KIND], cons(argv[0], node));
    struct ui_walk walk = { 0 };
    return walk_node(&walk, node, 1);
}

lisp_value builtin_ui_list(int argc, lisp_value *argv)
{
    return ui_list(argv[0], require_int(argv[1], "limit"), argv[2]);
}

lisp_value builtin_ui_properties(int argc, lisp_value *argv)
{
    lisp_value properties = argv[0];
    ui_list(properties, 64, ui.properties_text);
    if (list_length(properties) % 2 != 0)
        lisp_fail_format("UI properties need key/value pairs.", 0, NULL);
    lisp_value seen = NIL_VAL;
    for (lisp_value scan = properties; consp(scan); scan = consp(CDR(scan)) ? CDR(CDR(scan)) : NIL_VAL) {
        lisp_value key = CAR(scan);
        bool allowed = false;
        for (lisp_value a = argv[1]; consp(a) && !allowed; a = CDR(a))
            allowed = eql_value(CAR(a), key);
        if (!allowed)
            lisp_fail_format("Unsupported UI property ~S.", 1, &key);
        for (lisp_value s = seen; consp(s); s = CDR(s))
            if (eql_value(CAR(s), key))
                lisp_fail_format("Duplicate UI property ~S.", 1, &key);
        seen = cons(key, seen);
    }
    return properties;
}

lisp_value builtin_ui_length(int argc, lisp_value *argv)
{
    return ui_length(argv[0], argc > 1 && truthy(argv[1]), argc > 2 && truthy(argv[2]));
}

lisp_value builtin_ui_string(int argc, lisp_value *argv)
{
    return ui_string(argv[0], require_int(argv[1], "limit"), argc < 3 || truthy(argv[2]));
}

lisp_value builtin_ui_key(int argc, lisp_value *argv)
{
    return ui_key(argv[0], argc > 1 && truthy(argv[1]));
}

lisp_value builtin_ui_edges(int argc, lisp_value *argv)
{
    return ui_edges(argv[0], argc > 1 && truthy(argv[1]));
}

lisp_value builtin_ui_choice(int argc, lisp_value *argv)
{
    return ui_choice(argv[0], argv[1]);
}

lisp_value builtin_ui_color(int argc, lisp_value *argv)
{
    return ui_color(argv[0], argc > 1 && truthy(argv[1]));
}

lisp_value builtin_ui_default(int argc, lisp_value *argv)
{
    int kind = kind_index(argv[0]);
    int key = key_index(argv[1]);
    return key < 0 ? NIL_VAL : ui_default(kind < 0 ? UI_RECT : kind, key);
}

lisp_value builtin_double_float(int argc, lisp_value *argv)
{
    return as_double(argv[0]);
}

lisp_value builtin_finite_real_p(int argc, lisp_value *argv)
{
    return truth(finite_real(argv[0]));
}

lisp_value builtin_finite_float_p(int argc, lisp_value *argv)
{
    return truth(doublep(argv[0]) && finite_double(DBLVAL(argv[0])));
}
