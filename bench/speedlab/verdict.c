#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

#define TILES_X 16
#define TILES_Y 4

static const char *MARKERS[] = {
    "speedlab", "SPEEDLAB", "TOMOE_ISOLATED", "LD_PRELOAD", "ld.so.preload", "/proc/self/maps",
    "Seccomp", "/run/speedlab", "vclock", "TracerPid", "/proc/self/status", "schedstat",
    "__vdso_clock_gettime", "linux-vdso", "AT_SYSINFO",
};

static const char *SERIALS[][2] = {
    { "wl_keyboard", "enter" }, { "wl_keyboard", "leave" }, { "wl_keyboard", "key" },
    { "wl_keyboard", "modifiers" }, { "wl_pointer", "enter" }, { "wl_pointer", "leave" },
    { "wl_pointer", "button" }, { "xdg_surface", "configure" }, { "xdg_surface", "ack_configure" },
    { "xdg_wm_base", "ping" }, { "xdg_wm_base", "pong" }, { "wl_touch", "down" }, { "wl_touch", "up" },
    { "wl_data_device", "enter" },
};

static const char *SPLIT_KEYS[] = { "instructions", "cpu", "raster", "children" };
static const char *MEMORY_KEYS[] = { "peak_private_dirty_kB", "settled_private_dirty_kB", "settled_rss_kB",
    "hwm_kB", "settled_malloc_used_kB", "peak_pss_kB", "settled_pss_kB" };

struct texts {
    char **items;
    size_t count, capacity;
};

struct span {
    const char *data;
    size_t length;
};

struct spans {
    struct span *items;
    size_t count, capacity;
};

struct split {
    long long instructions, cpu, raster, children;
};

struct event_side {
    const char *name;
    char **lines;
    size_t count;
};

static void text_add(struct texts *list, const char *text) {
    list->items = allocate_more(list->items, &list->capacity, list->count + 1, sizeof(*list->items));
    size_t length = strlen(text);
    char *copy = allocate(length + 1);
    memcpy(copy, text, length + 1);
    list->items[list->count++] = copy;
}

static const char *buffer_end(struct buffer *data) {
    return data->data ? data->data : "";
}

static char *buffer_own(struct buffer *data) {
    if (!data->data) buffer_text(data, "");
    return data->data;
}

static struct buffer read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "speedlab: cannot read %s\n", path);
        exit(1);
    }
    struct buffer data = { 0 };
    char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file))) buffer_append(&data, chunk, got);
    if (ferror(file)) {
        fprintf(stderr, "speedlab: cannot read %s\n", path);
        exit(1);
    }
    fclose(file);
    return data;
}

static struct json *load_json(const char *path) {
    struct buffer data = read_file(path);
    char error[256];
    struct json *value = json_parse(buffer_end(&data), data.length, error, sizeof(error));
    if (!value) {
        fprintf(stderr, "speedlab: %s: %s\n", path, error);
        exit(1);
    }
    free(data.data);
    return value;
}

static int node_truth(const struct json *run) {
    return json_truth(json_get(run, "ok"));
}

static void error_text(struct buffer *out, const struct json *run) {
    const struct json *error = json_get(run, "error");
    if (!error) {
        buffer_text(out, "?");
        return;
    }
    switch (error->kind) {
    case JSON_STRING: buffer_append(out, error->as.string.text, error->as.string.length); break;
    case JSON_NULL: buffer_text(out, "None"); break;
    case JSON_TRUE: buffer_text(out, "True"); break;
    case JSON_FALSE: buffer_text(out, "False"); break;
    case JSON_INTEGER: buffer_printf(out, "%lld", error->as.integer); break;
    case JSON_FLOAT: json_format_number(out, error->as.number); break;
    default: json_format(out, error, 0); break;
    }
}

static void py_quote(struct buffer *out, const char *text) {
    int single = !((strchr(text, '\'') != NULL) && (strchr(text, '"') == NULL));
    char quote = single ? '\'' : '"';
    buffer_append(out, &quote, 1);
    for (const char *cursor = text; *cursor; cursor++) {
        unsigned char byte = (unsigned char)*cursor;
        if (byte == (unsigned char)quote || byte == '\\') {
            buffer_append(out, "\\", 1);
            buffer_append(out, (const char *)&byte, 1);
        } else if (byte == '\n') {
            buffer_text(out, "\\n");
        } else if (byte == '\r') {
            buffer_text(out, "\\r");
        } else if (byte == '\t') {
            buffer_text(out, "\\t");
        } else if (byte < 0x20 || byte == 0x7f) {
            buffer_printf(out, "\\x%02x", byte);
        } else {
            buffer_append(out, (const char *)&byte, 1);
        }
    }
    buffer_append(out, &quote, 1);
}

static void py_list(struct buffer *out, const struct json *array, size_t limit) {
    buffer_text(out, "[");
    size_t count = json_length(array);
    if (count > limit) count = limit;
    for (size_t index = 0; index < count; index++) {
        if (index) buffer_text(out, ", ");
        py_quote(out, json_text(json_at(array, index)));
    }
    buffer_text(out, "]");
}

static void py_counts(struct buffer *out, const struct json *object) {
    buffer_text(out, "{");
    for (size_t index = 0; index < json_length(object); index++) {
        const struct json_member *member = &object->as.object.members[index];
        if (index) buffer_text(out, ", ");
        py_quote(out, member->key);
        buffer_printf(out, ": %lld", json_int_value(member->value));
    }
    buffer_text(out, "}");
}

static int spans_equal(struct span value, const char *text, size_t length) {
    return value.length == length && !memcmp(value.data, text, length);
}

static void span_push(struct spans *list, struct span value) {
    list->items = allocate_more(list->items, &list->capacity, list->count + 1, sizeof(*list->items));
    list->items[list->count++] = value;
}

static void span_add_unique(struct spans *list, struct span value) {
    for (size_t index = 0; index < list->count; index++)
        if (spans_equal(list->items[index], value.data, value.length)) return;
    span_push(list, value);
}

static void span_discard(struct spans *list, struct span value) {
    for (size_t index = 0; index < list->count; index++) {
        if (!spans_equal(list->items[index], value.data, value.length)) continue;
        memmove(list->items + index, list->items + index + 1, (list->count - index - 1) * sizeof(*list->items));
        list->count--;
        return;
    }
}

static int span_matches(struct span value, const char *text) {
    return spans_equal(value, text, strlen(text));
}

static int parse_message(const char *line, struct span *head, struct span *interface, struct span *number,
    struct span *method, struct span *args) {
    size_t length = strlen(line), at = 1, start;
    if (!length || line[0] != '[') return 0;
    while (at < length && isspace((unsigned char)line[at])) at++;
    start = at;
    while (at < length && (isdigit((unsigned char)line[at]) || line[at] == '.')) at++;
    if (at == start) return 0;
    if (at + 1 >= length || line[at] != ']' || line[at + 1] != ' ') return 0;
    at += 2;
    if (at >= length || line[at] != '{') return 0;
    at++;
    while (at < length && line[at] != '}') at++;
    if (at >= length) return 0;
    at++;
    start = at;
    while (at < length && isspace((unsigned char)line[at])) at++;
    if (at == start) return 0;
    if (at + 2 < length && line[at] == '-' && line[at + 1] == '>' && line[at + 2] == ' ') at += 3;
    head->data = line;
    head->length = at;
    start = at;
    while (at < length && (isalnum((unsigned char)line[at]) || line[at] == '_')) at++;
    if (at == start) return 0;
    interface->data = line + start;
    interface->length = at - start;
    if (at >= length || line[at] != '#') return 0;
    at++;
    start = at;
    while (at < length && isdigit((unsigned char)line[at])) at++;
    if (at == start) return 0;
    number->data = line + start;
    number->length = at - start;
    if (at >= length || line[at] != '.') return 0;
    at++;
    start = at;
    while (at < length && (isalnum((unsigned char)line[at]) || line[at] == '_')) at++;
    if (at == start) return 0;
    method->data = line + start;
    method->length = at - start;
    if (at >= length || line[at] != '(') return 0;
    if (line[length - 1] != ')' || at > length - 1) return 0;
    args->data = line + at + 1;
    args->length = length - 1 - at - 1;
    return 1;
}

static int is_serial(struct span interface, struct span method) {
    for (size_t index = 0; index < sizeof(SERIALS) / sizeof(*SERIALS); index++)
        if (span_matches(interface, SERIALS[index][0]) && span_matches(method, SERIALS[index][1])) return 1;
    return 0;
}

static long long name_index(struct spans *names, struct span value) {
    for (size_t index = 0; index < names->count; index++)
        if (spans_equal(names->items[index], value.data, value.length)) return (long long)index;
    span_push(names, value);
    return (long long)names->count - 1;
}

static char *copy_text(const char *text) {
    size_t length = strlen(text);
    char *copy = allocate(length + 1);
    memcpy(copy, text, length + 1);
    return copy;
}

static char **canonical_lines(const struct json *lines, size_t *count) {
    size_t total = json_length(lines);
    char **out = allocate((total ? total : 1) * sizeof(*out));
    struct spans names = { 0 }, syncs = { 0 };
    for (size_t index = 0; index < total; index++) {
        const char *line = json_text(json_at(lines, index));
        struct span head, interface, number, method, args;
        if (!line || !parse_message(line, &head, &interface, &number, &method, &args)) {
            out[index] = copy_text(line ? line : "");
            continue;
        }
        if (span_matches(interface, "wl_display") && span_matches(method, "sync")) {
            size_t start = 0;
            for (size_t at = 0; at < args.length; at++)
                if (args.data[at] == '#') start = at + 1;
            struct span value = { args.data + start, args.length - start };
            span_add_unique(&syncs, value);
        }
        struct spans parts = { 0 };
        if (args.length) {
            size_t start = 0, at = 0;
            while (at + 1 < args.length) {
                if (args.data[at] == ',' && args.data[at + 1] == ' ') {
                    struct span part = { args.data + start, at - start };
                    span_push(&parts, part);
                    at += 2;
                    start = at;
                    continue;
                }
                at++;
            }
            struct span part = { args.data + start, args.length - start };
            span_push(&parts, part);
        }
        int serial = is_serial(interface, method);
        if (!serial && span_matches(interface, "wl_callback") && span_matches(method, "done")) {
            for (size_t slot = 0; slot < syncs.count; slot++)
                if (spans_equal(syncs.items[slot], number.data, number.length)) {
                    serial = 1;
                    break;
                }
        }
        if (span_matches(interface, "wl_callback") && span_matches(method, "done"))
            span_discard(&syncs, number);
        struct buffer text = { 0 };
        buffer_append(&text, head.data, head.length);
        buffer_append(&text, interface.data, interface.length);
        buffer_append(&text, "#", 1);
        buffer_append(&text, number.data, number.length);
        buffer_append(&text, ".", 1);
        buffer_append(&text, method.data, method.length);
        buffer_text(&text, "(");
        for (size_t slot = 0; slot < parts.count; slot++) {
            if (slot) buffer_text(&text, ", ");
            if (!slot && serial) {
                buffer_printf(&text, "serial%lld", name_index(&names, parts.items[slot]));
            } else {
                buffer_append(&text, parts.items[slot].data, parts.items[slot].length);
            }
        }
        buffer_text(&text, ")");
        out[index] = buffer_own(&text);
    }
    *count = total;
    return out;
}

static int json_equal(const struct json *first, const struct json *second) {
    if (!first || !second) return first == second;
    if ((first->kind == JSON_INTEGER || first->kind == JSON_FLOAT) &&
        (second->kind == JSON_INTEGER || second->kind == JSON_FLOAT)) {
        if (first->kind == JSON_INTEGER && second->kind == JSON_INTEGER)
            return first->as.integer == second->as.integer;
        return json_double_value(first) == json_double_value(second);
    }
    if (first->kind != second->kind) return 0;
    switch (first->kind) {
    case JSON_NULL:
    case JSON_TRUE:
    case JSON_FALSE: return 1;
    case JSON_STRING:
        return first->as.string.length == second->as.string.length &&
               !memcmp(first->as.string.text, second->as.string.text, first->as.string.length);
    case JSON_INTEGER: return first->as.integer == second->as.integer;
    case JSON_FLOAT: return first->as.number == second->as.number;
    case JSON_ARRAY:
        if (first->as.array.count != second->as.array.count) return 0;
        for (size_t index = 0; index < first->as.array.count; index++)
            if (!json_equal(first->as.array.items[index], second->as.array.items[index])) return 0;
        return 1;
    case JSON_OBJECT:
        if (first->as.object.count != second->as.object.count) return 0;
        for (size_t index = 0; index < first->as.object.count; index++) {
            const struct json_member *member = &first->as.object.members[index];
            const struct json *other = json_get(second, member->key);
            if (!other || !json_equal(member->value, other)) return 0;
        }
        return 1;
    }
    return 0;
}

static struct json *frame_detail(const struct json *want, const struct json *got, size_t index) {
    struct json *detail = json_object();
    json_set(detail, "frame", json_integer((long long)index));
    json_set(detail, "virtual_time", json_at(got, 1));
    json_set(detail, "reference_time", json_at(want, 1));
    json_set(detail, "size", json_at(got, 2));
    if (json_equal(json_at(want, 2), json_at(got, 2)) && json_equal(json_at(want, 1), json_at(got, 1))) {
        const char *size = json_text(json_at(want, 2));
        long width = 0, height = 0;
        char *end = NULL;
        if (size) {
            width = strtol(size, &end, 10);
            if (end && *end == 'x') height = strtol(end + 1, NULL, 10);
        }
        const struct json *first = json_at(want, 4), *second = json_at(got, 4);
        size_t common = json_length(first);
        if (json_length(second) < common) common = json_length(second);
        size_t changed = 0;
        long long first_x = 0, first_y = 0, last_x = 0, last_y = 0;
        for (size_t tile = 0; tile < common; tile++) {
            const char *left = json_text(json_at(first, tile)), *right = json_text(json_at(second, tile));
            if (left && right && !strcmp(left, right)) continue;
            if (!left && !right) continue;
            long long x = (long long)(tile % TILES_X), y = (long long)(tile / TILES_X);
            if (!changed) {
                first_x = last_x = x;
                first_y = last_y = y;
            }
            if (x < first_x) first_x = x;
            if (x > last_x) last_x = x;
            if (y < first_y) first_y = y;
            if (y > last_y) last_y = y;
            changed++;
        }
        json_set(detail, "tiles_changed", json_integer((long long)changed));
        if (changed) {
            long tile_width = width / TILES_X, tile_height = height / TILES_Y;
            long long x0 = first_x * tile_width, y0 = first_y * tile_height;
            long long x1 = last_x == TILES_X - 1 ? width : (last_x + 1) * tile_width;
            long long y1 = last_y == TILES_Y - 1 ? height : (last_y + 1) * tile_height;
            struct json *box = json_array();
            json_push(box, json_integer(x0));
            json_push(box, json_integer(y0));
            json_push(box, json_integer(x1 - x0));
            json_push(box, json_integer(y1 - y0));
            json_set(detail, "box", box);
        }
    }
    return detail;
}

static struct json *compare_frames(const struct json *want, const struct json *got) {
    size_t want_count = json_length(want), got_count = json_length(got);
    size_t common = want_count < got_count ? want_count : got_count;
    for (size_t index = 0; index < common; index++) {
        const struct json *first = json_at(want, index), *second = json_at(got, index);
        int same = 1;
        for (size_t field = 0; field < 4 && same; field++)
            if (!json_equal(json_at(first, field), json_at(second, field))) same = 0;
        if (!same) return frame_detail(first, second, index);
    }
    if (want_count != got_count) {
        struct json *detail = json_object();
        json_set(detail, "frame", json_integer((long long)common));
        json_set(detail, "reference_frames", json_integer((long long)want_count));
        json_set(detail, "frames", json_integer((long long)got_count));
        return detail;
    }
    return NULL;
}

static const struct event_side *find_side(const struct event_side *sides, size_t count, const char *name) {
    for (size_t index = 0; index < count; index++)
        if (!strcmp(sides[index].name, name)) return &sides[index];
    return NULL;
}

static struct event_side *canonical_events(const struct json *events, size_t *count) {
    size_t total = json_length(events);
    struct event_side *sides = allocate((total ? total : 1) * sizeof(*sides));
    for (size_t index = 0; index < total; index++) {
        const struct json_member *member = &events->as.object.members[index];
        sides[index].name = member->key;
        sides[index].lines = canonical_lines(member->value, &sides[index].count);
    }
    *count = total;
    return sides;
}

static struct json *compare_events(const struct json *want, const struct json *got) {
    size_t want_count = 0, got_count = 0;
    struct event_side *want_sides = canonical_events(want, &want_count);
    struct event_side *got_sides = canonical_events(got, &got_count);
    const char **names = allocate((want_count + got_count + 1) * sizeof(*names));
    size_t names_count = 0;
    for (size_t index = 0; index < want_count; index++) names[names_count++] = want_sides[index].name;
    for (size_t index = 0; index < got_count; index++) {
        int seen = 0;
        for (size_t other = 0; other < names_count && !seen; other++)
            if (!strcmp(names[other], got_sides[index].name)) seen = 1;
        if (!seen) names[names_count++] = got_sides[index].name;
    }
    for (size_t index = 0; index < names_count; index++)
        for (size_t other = index + 1; other < names_count; other++)
            if (strcmp(names[index], names[other]) > 0) {
                const char *swap = names[index];
                names[index] = names[other];
                names[other] = swap;
            }
    for (size_t index = 0; index < names_count; index++) {
        const struct event_side *first = find_side(want_sides, want_count, names[index]);
        const struct event_side *second = find_side(got_sides, got_count, names[index]);
        if (!first || !second) {
            struct json *detail = json_object();
            json_set(detail, "client", json_string(names[index]));
            json_set(detail, "missing_in", json_string(second ? "reference" : "candidate"));
            return detail;
        }
        size_t common = first->count < second->count ? first->count : second->count;
        for (size_t line = 0; line < common; line++) {
            if (!strcmp(first->lines[line], second->lines[line])) continue;
            struct json *detail = json_object();
            json_set(detail, "client", json_string(names[index]));
            json_set(detail, "line", json_integer((long long)line));
            json_set(detail, "reference", json_string(first->lines[line]));
            json_set(detail, "candidate", json_string(second->lines[line]));
            return detail;
        }
        if (first->count != second->count) {
            struct json *detail = json_object();
            json_set(detail, "client", json_string(names[index]));
            json_set(detail, "line", json_integer((long long)common));
            json_set(detail, "reference", common < first->count ? json_string(first->lines[common]) : json_null());
            json_set(detail, "candidate", common < second->count ? json_string(second->lines[common]) : json_null());
            return detail;
        }
    }
    return NULL;
}

static void equivalence(const struct json *reference, const struct json *run, const char *label,
    struct texts *problems, struct json **frames_out, struct json **events_out) {
    struct json *frames = compare_frames(json_get(reference, "frames"), json_get(run, "frames"));
    if (frames) {
        struct buffer text = { 0 };
        struct buffer detail = { 0 };
        json_format(&detail, frames, 0);
        buffer_printf(&text, "%s: output frame %lld differs from the reference: %s", label,
            json_int_value(json_get(frames, "frame")), buffer_end(&detail));
        text_add(problems, buffer_end(&text));
    }
    struct json *events = compare_events(json_get(reference, "events"), json_get(run, "events"));
    if (events) {
        struct buffer text = { 0 };
        struct buffer detail = { 0 };
        json_format(&detail, events, 0);
        buffer_printf(&text, "%s: a client saw different protocol traffic: %s", label, buffer_end(&detail));
        text_add(problems, buffer_end(&text));
    }
    const struct json *want = json_get(reference, "capture"), *got = json_get(run, "capture");
    if (!json_equal(want, got)) {
        struct buffer text = { 0 };
        buffer_printf(&text, "%s: the capture session behaved differently: ", label);
        if (got && got->kind == JSON_ARRAY)
            py_list(&text, got, 5);
        else
            buffer_text(&text, "None");
        text_add(problems, buffer_end(&text));
    }
    if (frames_out) *frames_out = frames;
    if (events_out) *events_out = events;
}

static struct split split_run(const struct json *run) {
    struct split result;
    long long instructions = json_int_value(json_get(run, "instructions"));
    long long main = json_int_value(json_get(run, "main_instructions"));
    long long process = json_int_value(json_get(run, "process_instructions"));
    result.instructions = instructions;
    result.cpu = main;
    result.raster = process - main;
    result.children = instructions - process;
    return result;
}

static double split_value(struct split value, const char *key) {
    if (!strcmp(key, "instructions")) return (double)value.instructions;
    if (!strcmp(key, "cpu")) return (double)value.cpu;
    if (!strcmp(key, "raster")) return (double)value.raster;
    return (double)value.children;
}

static double band(const struct json *bands, const char *key) {
    return json_double_value(json_get(bands, key));
}

static struct json *memory_node(const struct json *memory, const char *key) {
    if (!strcmp(key, "settled_private_dirty_kB"))
        return json_get(json_get(memory, "settled"), "Private_Dirty");
    if (!strcmp(key, "settled_rss_kB")) return json_get(json_get(memory, "settled"), "Rss");
    if (!strcmp(key, "settled_malloc_used_kB")) return json_get(json_get(memory, "settled"), "malloc_used_kB");
    if (!strcmp(key, "settled_pss_kB")) return json_get(json_get(memory, "settled"), "Pss");
    return json_get(memory, key);
}

static const char *MEMORY_DELTA_KEYS[] = { "private_dirty", "peak_private_dirty" };

static const char *memory_source(const char *key) {
    return !strcmp(key, "private_dirty") ? "settled_private_dirty_kB" : "peak_private_dirty_kB";
}

static size_t memory_slot(const char *key) {
    for (size_t index = 0; index < 7; index++)
        if (!strcmp(MEMORY_KEYS[index], memory_source(key))) return index;
    return 0;
}

static size_t count_matches(const char *data, size_t length, const char *needle) {
    size_t width = strlen(needle), count = 0, at = 0;
    while (at + width <= length) {
        const char *found = memmem(data + at, length - at, needle, width);
        if (!found) break;
        count++;
        at = (size_t)(found - data) + width;
    }
    return count;
}

static struct json *markers(const char *reference, const char *candidate) {
    struct buffer want = read_file(reference), got = read_file(candidate);
    struct json *found = json_object();
    for (size_t index = 0; index < sizeof(MARKERS) / sizeof(*MARKERS); index++) {
        size_t before = count_matches(buffer_end(&want), want.length, MARKERS[index]);
        size_t after = count_matches(buffer_end(&got), got.length, MARKERS[index]);
        if (after > before) json_set(found, MARKERS[index], json_integer((long long)(after - before)));
    }
    return found;
}

static void print_json(const struct json *value, int indent) {
    struct buffer text = { 0 };
    json_format(&text, value, indent);
    fwrite(buffer_end(&text), 1, text.length, stdout);
    fputc('\n', stdout);
}

static int write_json(const struct json *value, const char *path) {
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "speedlab: cannot write %s\n", path);
        return 0;
    }
    struct buffer compact = { 0 };
    json_format(&compact, value, 0);
    size_t written = fwrite(buffer_end(&compact), 1, compact.length, file);
    int failed = written != compact.length || ferror(file);
    if (fclose(file)) failed = 1;
    if (failed) {
        fprintf(stderr, "speedlab: cannot write %s\n", path);
        return 0;
    }
    return 1;
}

static struct json *refusal(struct json *bases, const char *reference, const char *candidate) {
    size_t count = 0;
    if (bases && bases->kind == JSON_ARRAY) count = bases->as.array.count;
    struct json *verdict = json_object();
    json_set(verdict, "verdict", json_string("reject"));
    json_set(verdict, "base_runs", json_integer((long long)count));
    struct json *reasons = json_array();
    struct buffer text = { 0 };
    buffer_printf(&text, "the reference holds %zu runs, and a verdict needs at least 3", count);
    json_push(reasons, json_string(buffer_end(&text)));
    json_set(verdict, "reasons", reasons);
    json_set(verdict, "base", json_string(reference));
    json_set(verdict, "candidate", json_string(candidate));
    return verdict;
}

static struct json *score_block(struct json *a, struct json *b, struct json *bases, const struct json *bands) {
    size_t base_count = json_length(bases);
    long long sums[4] = { 0, 0, 0, 0 };
    for (size_t index = 0; index < base_count; index++) {
        struct split value = split_run(json_at(bases, index));
        sums[0] += value.instructions;
        sums[1] += value.cpu;
        sums[2] += value.raster;
        sums[3] += value.children;
    }
    double means[4];
    for (size_t index = 0; index < 4; index++) means[index] = (double)sums[index] / (double)base_count;
    struct split mine = split_run(a);
    struct json *score = json_object();
    for (size_t index = 0; index < 4; index++)
        json_set(score, SPLIT_KEYS[index], json_integer((long long)split_value(mine, SPLIT_KEYS[index])));
    struct json *reference = json_object();
    for (size_t index = 0; index < 4; index++)
        json_set(reference, SPLIT_KEYS[index], json_integer((long long)json_round(means[index], 0)));
    json_set(score, "reference", reference);
    struct json *delta = json_object();
    struct json *significant = json_object();
    for (size_t index = 0; index < 4; index++) {
        double value = split_value(mine, SPLIT_KEYS[index]);
        double floor = means[index] > 1.0 ? means[index] : 1.0;
        json_set(delta, SPLIT_KEYS[index], json_float(json_round((value - means[index]) / floor * 100, 4)));
        json_set(significant, SPLIT_KEYS[index],
            json_boolean(fabs(value - means[index]) / floor * 100 > band(bands, SPLIT_KEYS[index])));
    }
    json_set(score, "delta_percent", delta);
    json_set(score, "significant", significant);
    if (node_truth(b)) {
        struct split other = split_run(b);
        struct json *persona = json_object();
        for (size_t index = 0; index < 4; index++)
            json_set(persona, SPLIT_KEYS[index], json_integer((long long)split_value(other, SPLIT_KEYS[index])));
        json_set(score, "persona_b", persona);
    } else {
        json_set(score, "persona_b", json_null());
    }
    struct json *noise = json_object();
    for (size_t index = 0; index < 4; index++)
        json_set(noise, SPLIT_KEYS[index], json_float(band(bands, SPLIT_KEYS[index])));
    json_set(score, "noise_band_percent", noise);
    json_set(score, "cycles", json_get(a, "cycles"));
    return score;
}

static struct json *memory_block(struct json *a, struct json *bases, const struct json *bands, double *deltas) {
    const struct json *memory = json_get(a, "memory");
    double values[7], means[7];
    long long sums[7] = { 0, 0, 0, 0, 0, 0, 0 };
    for (size_t index = 0; index < 7; index++)
        values[index] = (double)json_int_value(memory_node(memory, MEMORY_KEYS[index]));
    size_t base_count = json_length(bases);
    for (size_t run = 0; run < base_count; run++) {
        const struct json *base = json_get(json_at(bases, run), "memory");
        for (size_t index = 0; index < 7; index++)
            sums[index] += json_int_value(memory_node(base, MEMORY_KEYS[index]));
    }
    for (size_t index = 0; index < 7; index++) means[index] = (double)sums[index] / (double)base_count;
    struct json *block = json_object();
    for (size_t index = 0; index < 7; index++) {
        struct json *value = memory_node(memory, MEMORY_KEYS[index]);
        json_set(block, MEMORY_KEYS[index], value ? value : json_null());
    }
    struct json *reference = json_object();
    for (size_t index = 0; index < 7; index++)
        json_set(reference, MEMORY_KEYS[index], json_integer((long long)json_round(means[index], 0)));
    json_set(block, "reference", reference);
    struct json *delta = json_object();
    struct json *significant = json_object();
    for (size_t index = 0; index < 2; index++) {
        size_t slot = memory_slot(MEMORY_DELTA_KEYS[index]);
        double floor = means[slot] > 1.0 ? means[slot] : 1.0;
        deltas[index] = json_round((values[slot] - means[slot]) / floor * 100, 4);
        json_set(delta, MEMORY_DELTA_KEYS[index], json_float(deltas[index]));
        json_set(significant, MEMORY_DELTA_KEYS[index],
            json_boolean(fabs(deltas[index]) > band(bands, MEMORY_DELTA_KEYS[index])));
    }
    json_set(block, "delta_percent", delta);
    json_set(block, "significant", significant);
    struct json *noise = json_object();
    for (size_t index = 0; index < 2; index++)
        json_set(noise, MEMORY_DELTA_KEYS[index], json_float(band(bands, MEMORY_DELTA_KEYS[index])));
    json_set(block, "noise_band_percent", noise);
    const struct json *breakdown = json_get(memory, "breakdown");
    struct json *shortened = json_array();
    size_t keep = json_length(breakdown);
    if (keep > 16) keep = 16;
    for (size_t index = 0; index < keep; index++) json_push(shortened, json_at(breakdown, index));
    json_set(block, "breakdown", shortened);
    return block;
}

static int main_verdict(int argc, char **argv) {
    const char *noise_path = "noise";
    int at = 1;
    if (argc > 2 && !strcmp(argv[1], "--noise")) {
        noise_path = argv[2];
        at = 3;
    }
    if (argc - at < 5) {
        fprintf(stderr, "usage: speedlab-verdict [--noise FILE] BASELINE A B REFERENCE CANDIDATE [OUT]\n");
        return 2;
    }
    const char *baseline_path = argv[at], *a_path = argv[at + 1], *b_path = argv[at + 2];
    const char *reference_binary = argv[at + 3], *candidate_binary = argv[at + 4];
    const char *out_path = argc - at > 5 ? argv[at + 5] : NULL;
    struct json *bands = load_json(noise_path);
    struct json *bases = load_json(baseline_path);
    struct json *a = load_json(a_path);
    struct json *b = load_json(b_path);
    if (!bases || bases->kind != JSON_ARRAY || bases->as.array.count < 3) {
        struct json *verdict = refusal(bases, reference_binary, candidate_binary);
        print_json(verdict, 1);
        return 1;
    }
    const struct json *reference = json_at(bases, 0);
    struct texts reasons = { 0 };
    struct buffer text = { 0 };
    for (size_t index = 0; index < json_length(bases); index++) {
        const struct json *run = json_at(bases, index);
        if (node_truth(run)) continue;
        text.length = 0;
        buffer_printf(&text, "base run %zu failed: ", index + 1);
        error_text(&text, run);
        text_add(&reasons, buffer_end(&text));
    }
    const struct json *runs[] = { a, b };
    const char *labels[] = { "run A", "run B" };
    for (size_t index = 0; index < 2; index++) {
        if (node_truth(runs[index])) continue;
        text.length = 0;
        buffer_printf(&text, "%s failed: ", labels[index]);
        error_text(&text, runs[index]);
        text_add(&reasons, buffer_end(&text));
    }
    struct json *frames_detail = NULL, *events_detail = NULL;
    if (!reasons.count) {
        for (size_t index = 1; index < json_length(bases); index++) {
            char label[64];
            snprintf(label, sizeof(label), "base run %zu", index + 1);
            equivalence(reference, json_at(bases, index), label, &reasons, NULL, NULL);
        }
        for (size_t index = 0; index < 2; index++) {
            const struct json *run = runs[index];
            long long status = json_int_value(json_get(run, "exit_status"));
            long long wanted = json_int_value(json_get(reference, "exit_status"));
            if (status != wanted) {
                text.length = 0;
                buffer_printf(&text, "%s: the compositor exited %lld, the reference %lld", labels[index], status,
                    wanted);
                text_add(&reasons, buffer_end(&text));
            }
            const struct json *violations = json_get(run, "violations");
            struct json *fresh = json_array();
            for (size_t slot = 0; slot < json_length(violations); slot++) {
                struct json *entry = json_at(violations, slot);
                const char *where = json_text(json_get(entry, "where"));
                int seen = 0;
                for (size_t base = 0; base < json_length(bases) && !seen; base++) {
                    const struct json *others = json_get(json_at(bases, base), "violations");
                    for (size_t other = 0; other < json_length(others) && !seen; other++) {
                        const char *known = json_text(json_get(json_at(others, other), "where"));
                        if (!where && !known) seen = 1;
                        else if (where && known && !strcmp(where, known)) seen = 1;
                    }
                }
                if (!seen) json_push(fresh, entry);
            }
            if (json_length(fresh)) {
                text.length = 0;
                buffer_printf(&text, "%s: read the real clock behind the rig's virtual one: ", labels[index]);
                struct json *shown = json_array();
                size_t keep = json_length(fresh);
                if (keep > 3) keep = 3;
                for (size_t slot = 0; slot < keep; slot++) json_push(shown, json_at(fresh, slot));
                json_format(&text, shown, 0);
                text_add(&reasons, buffer_end(&text));
            }
            struct json *frames = NULL, *events = NULL;
            equivalence(reference, run, labels[index], &reasons, &frames, &events);
            if (index == 0) {
                frames_detail = frames;
                events_detail = events;
            }
            const struct json *state = json_get(run, "state");
            struct json *fresh_state = json_array();
            for (size_t slot = 0; slot < json_length(state); slot++) {
                const struct json *entry = json_at(state, slot);
                int known = 0;
                for (size_t base = 0; base < json_length(bases) && !known; base++) {
                    const struct json *others = json_get(json_at(bases, base), "state");
                    for (size_t other = 0; other < json_length(others) && !known; other++)
                        if (json_equal(entry, json_at(others, other))) known = 1;
                }
                if (!known) json_push(fresh_state, json_at(entry, 0));
            }
            if (json_length(fresh_state)) {
                for (size_t first = 0; first < json_length(fresh_state); first++)
                    for (size_t second = first + 1; second < json_length(fresh_state); second++) {
                        const char *left = json_text(json_at(fresh_state, first));
                        const char *right = json_text(json_at(fresh_state, second));
                        if (left && right && strcmp(left, right) > 0) {
                            struct json *swap = json_at(fresh_state, first);
                            fresh_state->as.array.items[first] = fresh_state->as.array.items[second];
                            fresh_state->as.array.items[second] = swap;
                        }
                    }
                text.length = 0;
                buffer_printf(&text, "%s: leaves state the reference does not (new or changed files), "
                                      "which would carry between runs outside the rig: ",
                    labels[index]);
                py_list(&text, fresh_state, 8);
                text_add(&reasons, buffer_end(&text));
            }
        }
        if (node_truth(a) && node_truth(b)) {
            struct json *frames = compare_frames(json_get(a, "frames"), json_get(b, "frames"));
            struct json *events = compare_events(json_get(a, "events"), json_get(b, "events"));
            if (frames || events) {
                text.length = 0;
                buffer_text(&text, "it sees the rig: persona A and persona B produced different output, first at ");
                json_format(&text, frames ? frames : events, 0);
                text_add(&reasons, buffer_end(&text));
            }
        }
        int scaled = json_truth(json_get(a, "scaled")) || json_truth(json_get(b, "scaled"));
        for (size_t index = 0; index < json_length(bases) && !scaled; index++)
            scaled = json_truth(json_get(json_at(bases, index), "scaled"));
        if (scaled) text_add(&reasons, "the hardware counters were multiplexed, so the counts are estimates");
        struct split first = split_run(a), second = split_run(b);
        for (size_t index = 0; index < 4; index++) {
            double value = split_value(first, SPLIT_KEYS[index]);
            double other = split_value(second, SPLIT_KEYS[index]);
            double floor = value < other ? value : other;
            if (floor < 1.0) floor = 1.0;
            double spread = fabs(value - other) / floor * 100;
            double limit = band(bands, SPLIT_KEYS[index]);
            if (!(spread > limit)) continue;
            struct buffer shown = { 0 }, limit_text = { 0 };
            buffer_printf(&shown, "%.4f", spread);
            json_format_number(&limit_text, limit);
            text.length = 0;
            buffer_printf(&text, "it sees the rig: %s cost %lld instructions as persona A vs %lld as persona B "
                                 "(%s%% > its %s%% noise band)",
                SPLIT_KEYS[index], (long long)value, (long long)other, buffer_end(&shown), buffer_end(&limit_text));
            text_add(&reasons, buffer_end(&text));
        }
    }
    struct json *found = markers(reference_binary, candidate_binary);
    if (json_length(found)) {
        text.length = 0;
        buffer_text(&text, "the build looks for the rig: new references to ");
        py_counts(&text, found);
        text_add(&reasons, buffer_end(&text));
    }
    struct json *verdict = json_object();
    json_set(verdict, "verdict", json_string(reasons.count ? "reject" : "accept"));
    struct json *reason_array = json_array();
    for (size_t index = 0; index < reasons.count; index++) json_push(reason_array, json_string(reasons.items[index]));
    json_set(verdict, "reasons", reason_array);
    json_set(verdict, "base_runs", json_integer((long long)json_length(bases)));
    json_set(verdict, "base", json_string(reference_binary));
    json_set(verdict, "candidate", json_string(candidate_binary));
    int all_bases = 1;
    for (size_t index = 0; index < json_length(bases); index++)
        if (!node_truth(json_at(bases, index))) all_bases = 0;
    if (node_truth(a) && all_bases) {
        struct json *score = score_block(a, b, bases, bands);
        json_set(verdict, "score", score);
        double memory_deltas[2];
        struct json *memory = memory_block(a, bases, bands, memory_deltas);
        json_set(verdict, "memory", memory);
        const char *delta_names[] = { "instructions", "cpu", "raster", "children", "private_dirty",
            "peak_private_dirty" };
        double delta_values[6];
        const struct json *deltas = json_get(score, "delta_percent");
        for (size_t index = 0; index < 4; index++)
            delta_values[index] = json_double_value(json_get(deltas, delta_names[index]));
        delta_values[4] = memory_deltas[0];
        delta_values[5] = memory_deltas[1];
        const char *better[6], *worse[6];
        size_t better_count = 0, worse_count = 0;
        for (size_t index = 0; index < 6; index++) {
            double value = delta_values[index];
            if (!(fabs(value) > band(bands, delta_names[index]))) continue;
            if (!strcmp(delta_names[index], "instructions")) continue;
            if (value < 0) better[better_count++] = delta_names[index];
            else if (value > 0) worse[worse_count++] = delta_names[index];
        }
        for (size_t index = 0; index < better_count; index++)
            for (size_t other = index + 1; other < better_count; other++)
                if (strcmp(better[index], better[other]) > 0) {
                    const char *swap = better[index];
                    better[index] = better[other];
                    better[other] = swap;
                }
        for (size_t index = 0; index < worse_count; index++)
            for (size_t other = index + 1; other < worse_count; other++)
                if (strcmp(worse[index], worse[other]) > 0) {
                    const char *swap = worse[index];
                    worse[index] = worse[other];
                    worse[other] = swap;
                }
        struct json *outcome = json_object();
        struct json *better_list = json_array(), *worse_list = json_array();
        for (size_t index = 0; index < better_count; index++) json_push(better_list, json_string(better[index]));
        for (size_t index = 0; index < worse_count; index++) json_push(worse_list, json_string(worse[index]));
        json_set(outcome, "better", better_list);
        json_set(outcome, "worse", worse_list);
        const char *result = reasons.count ? "reject"
                                            : better_count && !worse_count ? "win"
                                            : worse_count && !better_count ? "loss"
                                            : better_count ? "mixed" : "neutral";
        json_set(outcome, "result", json_string(result));
        json_set(verdict, "outcome", outcome);
        struct json *equivalence_block = json_object();
        const struct json *frames = json_get(a, "frames");
        json_set(equivalence_block, "frames", json_integer((long long)json_length(frames)));
        json_set(equivalence_block, "reference_frames",
            json_integer((long long)json_length(json_get(reference, "frames"))));
        json_set(equivalence_block, "frames_match", json_boolean(frames_detail == NULL));
        json_set(equivalence_block, "first_frame_difference", frames_detail ? frames_detail : json_null());
        size_t event_lines = 0;
        const struct json *events = json_get(a, "events");
        for (size_t index = 0; events && index < json_length(events); index++)
            event_lines += json_length(events->as.object.members[index].value);
        json_set(equivalence_block, "event_lines", json_integer((long long)event_lines));
        json_set(equivalence_block, "events_match", json_boolean(events_detail == NULL));
        json_set(equivalence_block, "first_event_difference", events_detail ? events_detail : json_null());
        json_set(verdict, "equivalence", equivalence_block);
        struct json *phases = json_array();
        const struct json *source = json_get(a, "phases");
        for (size_t index = 0; index < json_length(source); index++) {
            const struct json *phase = json_at(source, index);
            struct json *entry = json_object();
            json_set(entry, "name", json_get(phase, "name"));
            json_set(entry, "t", json_get(phase, "t"));
            json_set(entry, "frames", json_get(phase, "frames"));
            json_set(entry, "cpu_instructions", json_get(phase, "main"));
            json_set(entry, "private_dirty_kB", json_get(phase, "private_dirty_kB"));
            json_push(phases, entry);
        }
        json_set(verdict, "phases", phases);
        struct json *run = json_object();
        json_set(run, "virtual_seconds", json_get(a, "virtual_seconds"));
        json_set(run, "real_seconds_a", json_get(a, "real_seconds"));
        struct json *other = json_get(b, "real_seconds");
        json_set(run, "real_seconds_b", other ? other : json_null());
        json_set(run, "steps", json_get(a, "steps"));
        json_set(verdict, "run", run);
    }
    print_json(verdict, 1);
    if (out_path) {
        json_set(verdict, "reference", bases);
        json_set(verdict, "a", a);
        json_set(verdict, "b", b);
        if (!write_json(verdict, out_path)) return 1;
    }
    return reasons.count ? 1 : 0;
}

static int collect(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: speedlab-verdict --collect WORK RUNS OUT\n");
        return 2;
    }
    long runs = strtol(argv[3], NULL, 10);
    struct json *bases = json_array();
    struct buffer failed = { 0 };
    for (long index = 1; index <= runs; index++) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/reference-%ld.json", argv[2], index);
        struct json *run = load_json(path);
        json_push(bases, run);
        if (node_truth(run)) continue;
        if (failed.length) buffer_text(&failed, "; ");
        struct buffer line = { 0 };
        buffer_printf(&line, "run %ld: ", index);
        error_text(&line, run);
        buffer_append(&failed, buffer_end(&line), line.length);
    }
    if (failed.length) {
        fprintf(stderr, "%s\n", buffer_end(&failed));
        return 1;
    }
    if (!write_json(bases, argv[4])) return 1;
    return 0;
}

static int summary(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: speedlab-verdict --summary FILE\n");
        return 2;
    }
    struct json *verdict = load_json(argv[2]);
    const char *result = json_text(json_get(verdict, "verdict"));
    const struct json *reasons = json_get(verdict, "reasons");
    struct buffer joined = { 0 };
    for (size_t index = 0; index < json_length(reasons); index++) {
        if (index) buffer_text(&joined, " || ");
        const char *reason = json_text(json_at(reasons, index));
        buffer_text(&joined, reason ? reason : "");
    }
    printf("%s | %s\n", result ? result : "", joined.length ? buffer_end(&joined) : "-");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--collect")) return collect(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--summary")) return summary(argc, argv);
    return main_verdict(argc, argv);
}
