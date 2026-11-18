#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "json.h"

#define PERIOD 10000000

struct span {
    size_t start, length;
};

struct lines {
    size_t *starts, *lengths;
    size_t count;
};

struct row {
    long long value;
    char *fields[3];
};

struct entry {
    char *key;
    long long value;
};

struct table {
    struct entry *entries;
    size_t count, capacity;
};

struct symbols {
    long long *starts;
    char **names;
    size_t count;
};

struct frame {
    char *dso, *symbol;
};

struct inclusive_row {
    char *function;
    double inclusive_percent, self_percent;
    long long instructions;
};

typedef int (*order)(const void *, const void *);

static void fail(const char *what) {
    fprintf(stderr, "speedlab-report: %s\n", what);
    exit(1);
}

static void fail_errno(const char *what) {
    fprintf(stderr, "speedlab-report: %s: %s\n", what, strerror(errno));
    exit(1);
}

static int is_space(char byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\v' || byte == '\f' || byte == '\r';
}

static int is_digit(char byte) {
    return byte >= '0' && byte <= '9';
}

static int is_hex(char byte) {
    return is_digit(byte) || (byte >= 'a' && byte <= 'f');
}

static char *duplicate_text(const char *text) {
    size_t length = strlen(text);
    char *copy = allocate(length + 1);
    memcpy(copy, text, length + 1);
    return copy;
}

static char *span_text(const char *text, struct span span) {
    char *copy = allocate(span.length + 1);
    memcpy(copy, text + span.start, span.length);
    copy[span.length] = '\0';
    return copy;
}

static int span_equals(const char *text, struct span span, const char *literal) {
    size_t length = strlen(literal);
    return span.length == length && !memcmp(text + span.start, literal, length);
}

static void trim_span(const char *text, struct span *span) {
    while (span->length && is_space(text[span->start])) {
        span->start++;
        span->length--;
    }
    while (span->length && is_space(text[span->start + span->length - 1])) span->length--;
}

static long long span_integer(const char *text, struct span span) {
    char local[64];
    size_t length = span.length < sizeof(local) - 1 ? span.length : sizeof(local) - 1;
    memcpy(local, text + span.start, length);
    local[length] = '\0';
    return strtoll(local, NULL, 10);
}

static long long span_hex(const char *text, struct span span) {
    char local[64];
    size_t length = span.length < sizeof(local) - 1 ? span.length : sizeof(local) - 1;
    memcpy(local, text + span.start, length);
    local[length] = '\0';
    return strtoll(local, NULL, 16);
}

static double span_double(const char *text, struct span span) {
    char local[64];
    size_t length = span.length < sizeof(local) - 1 ? span.length : sizeof(local) - 1;
    memcpy(local, text + span.start, length);
    local[length] = '\0';
    return strtod(local, NULL);
}

static long long floor_kilobytes(long long value) {
    return value / 1024 - (value % 1024 < 0);
}

static size_t character_width(const char *text, size_t length, size_t at) {
    unsigned char byte = (unsigned char)text[at];
    size_t width = byte >= 0xf0 ? 4 : byte >= 0xe0 ? 3 : byte >= 0xc0 ? 2 : 1;
    return at + width <= length ? width : 1;
}

static size_t character_count(const char *text, size_t length) {
    size_t at = 0, count = 0;
    while (at < length) {
        at += character_width(text, length, at);
        count++;
    }
    return count;
}

static size_t character_prefix(const char *text, size_t length, size_t characters) {
    size_t at = 0, count = 0;
    while (at < length && count < characters) {
        at += character_width(text, length, at);
        count++;
    }
    return at;
}

static size_t character_suffix(const char *text, size_t length, size_t characters) {
    size_t total = character_count(text, length);
    if (total <= characters) return 0;
    size_t target = total - characters, at = 0;
    while (target--) at += character_width(text, length, at);
    return at;
}

static size_t line_break(const char *text, size_t at, size_t length) {
    unsigned char byte = (unsigned char)text[at];
    if (byte == '\n' || byte == '\v' || byte == '\f' || byte == 0x1c || byte == 0x1d || byte == 0x1e) return 1;
    if (byte == '\r') return at + 1 < length && text[at + 1] == '\n' ? 2 : 1;
    if (byte == 0xc2 && at + 1 < length && (unsigned char)text[at + 1] == 0x85) return 2;
    if (byte == 0xe2 && at + 2 < length && (unsigned char)text[at + 1] == 0x80 &&
        ((unsigned char)text[at + 2] == 0xa8 || (unsigned char)text[at + 2] == 0xa9))
        return 3;
    return 0;
}

static struct lines split_lines(const char *text, size_t length) {
    struct lines lines = { 0 };
    size_t start = 0, at = 0, start_capacity = 0, length_capacity = 0;
    while (at < length) {
        size_t jump = line_break(text, at, length);
        if (!jump) {
            at++;
            continue;
        }
        lines.starts = allocate_more(lines.starts, &start_capacity, lines.count + 1, sizeof(*lines.starts));
        lines.lengths = allocate_more(lines.lengths, &length_capacity, lines.count + 1, sizeof(*lines.lengths));
        lines.starts[lines.count] = start;
        lines.lengths[lines.count] = at - start;
        lines.count++;
        at += jump;
        start = at;
    }
    if (start < length) {
        lines.starts = allocate_more(lines.starts, &start_capacity, lines.count + 1, sizeof(*lines.starts));
        lines.lengths = allocate_more(lines.lengths, &length_capacity, lines.count + 1, sizeof(*lines.lengths));
        lines.starts[lines.count] = start;
        lines.lengths[lines.count] = length - start;
        lines.count++;
    }
    return lines;
}

static size_t split_tokens(const char *text, size_t length, struct span **spans) {
    size_t count = 0, capacity = 0, at = 0;
    *spans = NULL;
    while (at < length) {
        while (at < length && is_space(text[at])) at++;
        if (at >= length) break;
        size_t start = at;
        while (at < length && !is_space(text[at])) at++;
        *spans = allocate_more(*spans, &capacity, count + 1, sizeof(**spans));
        (*spans)[count].start = start;
        (*spans)[count].length = at - start;
        count++;
    }
    return count;
}

static int row_match(const char *line, size_t length, struct span *samples, struct span *rest) {
    size_t at = 0;
    if (!length || !is_space(line[0])) return 0;
    while (at < length && is_space(line[at])) at++;
    size_t number = at;
    while (at < length && (is_digit(line[at]) || line[at] == '.')) at++;
    if (at == number || at >= length || line[at] != '%') return 0;
    at++;
    size_t gap = at;
    while (at < length && is_space(line[at])) at++;
    if (at == gap) return 0;
    size_t digits = at;
    while (at < length && is_digit(line[at])) at++;
    if (at == digits) return 0;
    samples->start = digits;
    samples->length = at - digits;
    gap = at;
    while (at < length && is_space(line[at])) at++;
    if (at == gap) return 0;
    rest->start = at;
    rest->length = length - at;
    return 1;
}

static size_t split_fields(const char *text, struct span rest, size_t columns, char *fields[3]) {
    trim_span(text, &rest);
    size_t count = 0, splits = 0;
    size_t start = rest.start, end = rest.start + rest.length, at = rest.start;
    while (at < end) {
        if (!is_space(text[at])) {
            at++;
            continue;
        }
        size_t run = at;
        while (run < end && is_space(text[run])) run++;
        if (run - at < 2) {
            at = run;
            continue;
        }
        if (splits + 1 >= columns) break;
        struct span field = { start, at - start };
        trim_span(text, &field);
        fields[count++] = span_text(text, field);
        splits++;
        at = run;
        start = at;
    }
    struct span last = { start, end - start };
    trim_span(text, &last);
    fields[count++] = span_text(text, last);
    return count;
}

static struct row *perf_rows(const char *text, size_t length, size_t columns, size_t *count_out) {
    struct lines lines = split_lines(text, length);
    struct row *rows = NULL;
    size_t count = 0, capacity = 0;
    for (size_t index = 0; index < lines.count; index++) {
        const char *line = text + lines.starts[index];
        struct span samples, rest;
        if (!row_match(line, lines.lengths[index], &samples, &rest)) continue;
        char *fields[3] = { NULL, NULL, NULL };
        size_t field_count = split_fields(line, rest, columns, fields);
        if (field_count < columns) continue;
        rows = allocate_more(rows, &capacity, count + 1, sizeof(*rows));
        rows[count].value = span_integer(line, samples) * PERIOD;
        for (size_t field = 0; field < 3; field++) rows[count].fields[field] = fields[field];
        count++;
    }
    *count_out = count;
    return rows;
}

static struct entry *table_find(struct table *table, const char *key) {
    for (size_t index = 0; index < table->count; index++)
        if (!strcmp(table->entries[index].key, key)) return &table->entries[index];
    return NULL;
}

static void table_add(struct table *table, const char *key, long long value) {
    struct entry *entry = table_find(table, key);
    if (entry) {
        entry->value += value;
        return;
    }
    table->entries = allocate_more(table->entries, &table->capacity, table->count + 1, sizeof(*table->entries));
    table->entries[table->count].key = duplicate_text(key);
    table->entries[table->count].value = value;
    table->count++;
}

static long long table_value(struct table *table, const char *key, long long fallback) {
    struct entry *entry = table_find(table, key);
    return entry ? entry->value : fallback;
}

static void stable_sort(void *base, size_t count, size_t size, order before) {
    if (count < 2) return;
    char *items = base;
    char *scratch = allocate(count * size);
    for (size_t width = 1; width < count; width *= 2) {
        for (size_t start = 0; start < count; start += 2 * width) {
            size_t middle = start + width < count ? start + width : count;
            size_t end = start + 2 * width < count ? start + 2 * width : count;
            size_t left = start, right = middle, out = start;
            while (left < middle && right < end) {
                if (before(items + right * size, items + left * size)) {
                    memcpy(scratch + out * size, items + right * size, size);
                    right++;
                } else {
                    memcpy(scratch + out * size, items + left * size, size);
                    left++;
                }
                out++;
            }
            while (left < middle) {
                memcpy(scratch + out * size, items + left * size, size);
                left++;
                out++;
            }
            while (right < end) {
                memcpy(scratch + out * size, items + right * size, size);
                right++;
                out++;
            }
        }
        memcpy(items, scratch, count * size);
    }
    free(scratch);
}

static int newer_entry(const void *left, const void *right) {
    const struct entry *const *first = left, *const *second = right;
    return (*first)->value > (*second)->value;
}

static int newer_inclusive(const void *left, const void *right) {
    const struct inclusive_row *first = left, *second = right;
    return first->inclusive_percent > second->inclusive_percent;
}

static struct entry **sorted_entries(struct table *table) {
    struct entry **items = allocate((table->count ? table->count : 1) * sizeof(*items));
    for (size_t index = 0; index < table->count; index++) items[index] = &table->entries[index];
    stable_sort(items, table->count, sizeof(*items), newer_entry);
    return items;
}

static struct json *ranked(struct table *table, long long total, size_t limit) {
    struct entry **items = sorted_entries(table);
    struct json *array = json_array();
    size_t count = table->count < limit ? table->count : limit;
    for (size_t index = 0; index < count; index++) {
        struct json *row = json_object();
        json_set(row, "what", json_string(items[index]->key));
        json_set(row, "instructions", json_integer(items[index]->value));
        json_set(row, "percent", json_float(json_round((double)items[index]->value / (double)total * 100.0, 3)));
        json_push(array, row);
    }
    return array;
}

static struct json *scaled_table(struct table *table, size_t limit) {
    struct entry **items = sorted_entries(table);
    struct json *object = json_object();
    size_t count = table->count < limit ? table->count : limit;
    for (size_t index = 0; index < count; index++)
        json_set(object, items[index]->key, json_integer(floor_kilobytes(items[index]->value)));
    return object;
}

static void run_nm(const char *binary, struct buffer *out) {
    int output[2], error[2];
    if (pipe2(output, O_CLOEXEC)) fail_errno("pipe");
    if (pipe2(error, O_CLOEXEC)) fail_errno("pipe");
    pid_t child = fork();
    if (child < 0) fail_errno("fork");
    if (!child) {
        int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
        dup2(output[1], 1);
        if (null >= 0) dup2(null, 2);
        close(output[0]);
        close(output[1]);
        close(error[0]);
        char *arguments[] = { "nm", "-n", "--defined-only", (char *)binary, NULL };
        execvp("nm", arguments);
        int code = errno;
        ssize_t written = write(error[1], &code, sizeof(code));
        (void)written;
        _exit(127);
    }
    close(output[1]);
    close(error[1]);
    char chunk[65536];
    while (1) {
        ssize_t got = read(output[0], chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR) continue;
            fail_errno("reading nm");
        }
        if (!got) break;
        buffer_append(out, chunk, (size_t)got);
    }
    close(output[0]);
    int code = 0;
    ssize_t reported = read(error[0], &code, sizeof(code));
    if (reported < 0 && errno == EINTR) reported = read(error[0], &code, sizeof(code));
    close(error[0]);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    if (reported == (ssize_t)sizeof(code)) {
        errno = code;
        fail_errno("cannot run nm");
    }
}

static int span_within(const char *text, struct span span, const char *haystack) {
    size_t length = strlen(haystack);
    if (!span.length || span.length > length) return 0;
    for (size_t at = 0; at + span.length <= length; at++)
        if (!memcmp(text + span.start, haystack + at, span.length)) return 1;
    return 0;
}

static void symbols_load(struct symbols *symbols, const char *binary) {
    struct buffer text = { 0 };
    run_nm(binary, &text);
    if (!text.data) buffer_append(&text, "", 0);
    struct lines lines = split_lines(text.data, text.length);
    size_t start_capacity = 0, name_capacity = 0;
    for (size_t index = 0; index < lines.count; index++) {
        const char *line = text.data + lines.starts[index];
        struct span *spans = NULL;
        size_t count = split_tokens(line, lines.lengths[index], &spans);
        if (count != 3 || !span_within(line, spans[1], "tTwW")) {
            free(spans);
            continue;
        }
        symbols->starts =
            allocate_more(symbols->starts, &start_capacity, symbols->count + 1, sizeof(*symbols->starts));
        symbols->names = allocate_more(symbols->names, &name_capacity, symbols->count + 1, sizeof(*symbols->names));
        symbols->starts[symbols->count] = span_hex(line, spans[0]);
        symbols->names[symbols->count] = span_text(line, spans[2]);
        symbols->count++;
        free(spans);
    }
}

static char *symbols_name(const struct symbols *symbols, long long offset) {
    size_t low = 0, high = symbols->count;
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (symbols->starts[middle] <= offset) low = middle + 1;
        else high = middle;
    }
    if (!low) {
        struct buffer out = { 0 };
        buffer_printf(&out, "0x%llx", (unsigned long long)offset);
        return out.data;
    }
    return duplicate_text(symbols->names[low - 1]);
}

static const char *node_text(struct json *value, size_t *length) {
    if (value && value->kind == JSON_STRING) {
        *length = value->as.string.length;
        return value->as.string.text;
    }
    *length = 0;
    return NULL;
}

static char *number_text(struct json *value) {
    struct buffer out = { 0 };
    if (value) {
        switch (value->kind) {
        case JSON_INTEGER: buffer_printf(&out, "%lld", value->as.integer); break;
        case JSON_FLOAT: json_format_number(&out, value->as.number); break;
        case JSON_STRING: buffer_append(&out, value->as.string.text, value->as.string.length); break;
        case JSON_TRUE: buffer_text(&out, "True"); break;
        case JSON_FALSE: buffer_text(&out, "False"); break;
        default: buffer_text(&out, "None"); break;
        }
    } else {
        buffer_text(&out, "None");
    }
    return out.data;
}

static void thread_class(const char *thread, const char *server, struct buffer *out) {
    const char *colon = strchr(thread, ':');
    size_t tid_end = colon ? (size_t)(colon - thread) : strlen(thread);
    const char *comm = colon ? colon + 1 : thread + tid_end;
    size_t start = 0;
    while (start < tid_end && is_space(thread[start])) start++;
    while (tid_end > start && is_space(thread[tid_end - 1])) tid_end--;
    if (!strncmp(comm, "llvmpipe", 8)) {
        buffer_text(out, "raster (llvmpipe threads)");
        return;
    }
    size_t server_length = strlen(server);
    if (tid_end - start == server_length && !memcmp(thread + start, server, server_length)) {
        buffer_text(out, "compositor main thread");
        return;
    }
    if (!strcmp(comm, "tomoe")) {
        buffer_text(out, "other tomoe threads and processes (shader compiler copies, workers)");
        return;
    }
    buffer_text(out, "other: ");
    buffer_text(out, comm);
}

static char *stripped_symbol(const char *text) {
    size_t length = strlen(text);
    struct buffer out = { 0 };
    size_t at = 0;
    while (at < length) {
        if (at + 4 <= length && !memcmp(text + at, "[.] ", 4)) {
            at += 4;
            continue;
        }
        buffer_append(&out, text + at, 1);
        at++;
    }
    if (!out.data) buffer_text(&out, "");
    struct span span = { 0, out.length };
    trim_span(out.data, &span);
    char *result = span_text(out.data, span);
    free(out.data);
    return result;
}

static int inclusive_match(const char *line, size_t length, struct span *self, struct span *children,
    struct span *dso, struct span *symbol) {
    size_t at = 0;
    if (!length || !is_space(line[0])) return 0;
    while (at < length && is_space(line[at])) at++;
    size_t start = at;
    while (at < length && (is_digit(line[at]) || line[at] == '.')) at++;
    if (at == start || at >= length || line[at] != '%') return 0;
    *self = (struct span){ start, at - start };
    at++;
    size_t gap = at;
    while (at < length && is_space(line[at])) at++;
    if (at == gap) return 0;
    start = at;
    while (at < length && (is_digit(line[at]) || line[at] == '.')) at++;
    if (at == start || at >= length || line[at] != '%') return 0;
    *children = (struct span){ start, at - start };
    at++;
    gap = at;
    while (at < length && is_space(line[at])) at++;
    if (at == gap) return 0;
    start = at;
    while (at < length && is_digit(line[at])) at++;
    if (at == start) return 0;
    gap = at;
    while (at < length && is_space(line[at])) at++;
    if (at == gap) return 0;
    start = at;
    while (at < length && !is_space(line[at])) at++;
    if (at == start) return 0;
    *dso = (struct span){ start, at - start };
    gap = at;
    while (at < length && is_space(line[at])) at++;
    if (at == gap) return 0;
    if (length - at < 3 || line[at] != '[') return 0;
    size_t width = character_width(line, length, at + 1);
    if (at + 1 + width >= length || line[at + 1 + width] != ']') return 0;
    at += 1 + width + 1;
    gap = at;
    while (at < length && is_space(line[at])) at++;
    if (at == gap) return 0;
    *symbol = (struct span){ at, length - at };
    return 1;
}

static struct json *cost_report(struct json *profile, const char *inclusive, size_t inclusive_length) {
    size_t thread_length = 0, function_length = 0, main_length = 0;
    const char *thread_text = node_text(json_get(profile, "threads"), &thread_length);
    const char *function_text = node_text(json_get(profile, "functions"), &function_length);
    const char *main_text = node_text(json_get(profile, "main"), &main_length);
    size_t thread_count = 0, function_count = 0, main_count = 0;
    struct row *threads = perf_rows(thread_text ? thread_text : "", thread_length, 2, &thread_count);
    struct row *functions = perf_rows(function_text ? function_text : "", function_length, 3, &function_count);
    struct row *mains = perf_rows(main_text ? main_text : "", main_length, 3, &main_count);
    char *server = number_text(json_get(profile, "server"));
    struct table classes = { 0 }, libraries = { 0 }, by_function = { 0 }, raster = { 0 };
    long long total = 0;
    for (size_t index = 0; index < thread_count; index++) {
        total += threads[index].value;
        struct buffer group = { 0 };
        thread_class(threads[index].fields[0], server, &group);
        table_add(&classes, group.data, threads[index].value);
        if (!strcmp(group.data, "compositor main thread"))
            table_add(&libraries, threads[index].fields[1], threads[index].value);
        free(group.data);
    }
    for (size_t index = 0; index < function_count; index++) {
        struct buffer group = { 0 };
        thread_class(functions[index].fields[0], server, &group);
        if (!strcmp(group.data, "raster (llvmpipe threads)")) {
            char *symbol = stripped_symbol(functions[index].fields[2]);
            struct buffer key = { 0 };
            buffer_text(&key, functions[index].fields[1]);
            buffer_text(&key, ": ");
            buffer_text(&key, symbol);
            table_add(&raster, key.data, functions[index].value);
            free(key.data);
            free(symbol);
        }
        free(group.data);
    }
    for (size_t index = 0; index < main_count; index++) {
        char *symbol = stripped_symbol(mains[index].fields[2]);
        struct buffer key = { 0 };
        buffer_text(&key, mains[index].fields[1]);
        buffer_text(&key, ": ");
        buffer_text(&key, symbol);
        table_add(&by_function, key.data, mains[index].value);
        free(key.data);
        free(symbol);
    }
    free(server);
    long long main_total = table_value(&classes, "compositor main thread", 1);
    long long main_sampled = table_value(&classes, "compositor main thread", 0);
    struct inclusive_row *rows = NULL;
    size_t row_count = 0, capacity = 0;
    struct lines lines = split_lines(inclusive, inclusive_length);
    for (size_t index = 0; index < lines.count; index++) {
        const char *line = inclusive + lines.starts[index];
        struct span self, children, dso, symbol;
        if (!inclusive_match(line, lines.lengths[index], &self, &children, &dso, &symbol)) continue;
        trim_span(line, &symbol);
        rows = allocate_more(rows, &capacity, row_count + 1, sizeof(*rows));
        struct inclusive_row *row = &rows[row_count++];
        struct buffer key = { 0 };
        buffer_append(&key, line + dso.start, dso.length);
        buffer_text(&key, ": ");
        buffer_append(&key, line + symbol.start, symbol.length);
        row->function = key.data;
        row->self_percent = span_double(line, self);
        row->inclusive_percent = span_double(line, children);
        row->instructions = (long long)(span_double(line, children) / 100.0 * (double)main_sampled);
    }
    stable_sort(rows, row_count, sizeof(*rows), newer_inclusive);
    struct table tomoe_functions = { 0 };
    for (size_t index = 0; index < by_function.count; index++)
        if (!strncmp(by_function.entries[index].key, "tomoe:", 6))
            table_add(&tomoe_functions, by_function.entries[index].key, by_function.entries[index].value);
    long long raster_total = total - main_total;
    if (raster_total < 1) raster_total = 1;
    struct json *result = json_object();
    json_set(result, "sampled_instructions", json_integer(total));
    json_set(result, "by_thread", ranked(&classes, total, 12));
    if (row_count) {
        struct json *array = json_array();
        size_t count = row_count < 120 ? row_count : 120;
        for (size_t index = 0; index < count; index++) {
            struct json *item = json_object();
            json_set(item, "function", json_string(rows[index].function));
            json_set(item, "inclusive_percent", json_float(rows[index].inclusive_percent));
            json_set(item, "self_percent", json_float(rows[index].self_percent));
            json_set(item, "inclusive_instructions", json_integer(rows[index].instructions));
            json_push(array, item);
        }
        json_set(result, "main_thread_inclusive", array);
    } else {
        json_set(result, "main_thread_inclusive",
            json_string_length(inclusive, character_prefix(inclusive, inclusive_length, 3000)));
    }
    json_set(result, "main_thread_by_library", ranked(&libraries, main_total, 25));
    json_set(result, "main_thread_functions", ranked(&by_function, main_total, 80));
    json_set(result, "main_thread_tomoe_functions", ranked(&tomoe_functions, main_total, 40));
    json_set(result, "raster_functions", ranked(&raster, raster_total, 15));
    return result;
}

static struct json *node_or_null(struct json *value) {
    return value ? value : json_null();
}

static struct json *subtract(struct json *left, struct json *right) {
    if (left && right && left->kind == JSON_INTEGER && right->kind == JSON_INTEGER)
        return json_integer(left->as.integer - right->as.integer);
    return json_float(json_double_value(left) - json_double_value(right));
}

static struct json *counted_report(struct json *run) {
    struct json *instructions = json_get(run, "instructions");
    struct json *main = json_get(run, "main_instructions");
    struct json *process = json_get(run, "process_instructions");
    struct json *memory = json_get(run, "memory");
    struct json *settled = memory ? json_get(memory, "settled") : NULL;
    struct json *object = json_object();
    json_set(object, "instructions", node_or_null(instructions));
    json_set(object, "cpu", node_or_null(main));
    json_set(object, "raster", subtract(process, main));
    json_set(object, "children", subtract(instructions, process));
    json_set(object, "settled_private_dirty_kB", node_or_null(settled ? json_get(settled, "Private_Dirty") : NULL));
    json_set(object, "peak_private_dirty_kB", node_or_null(memory ? json_get(memory, "peak_private_dirty_kB") : NULL));
    return object;
}

static struct frame frame_name(const char *text, size_t length, const struct symbols *symbols,
    const char *binary_name) {
    for (size_t plus = 1; plus + 3 <= length; plus++) {
        if (text[plus] != '+' || text[plus + 1] != '0' || text[plus + 2] != 'x') continue;
        size_t digits = plus + 3, at = digits;
        while (at < length && is_hex(text[at])) at++;
        if (at == digits) continue;
        struct span written = { 0, 0 };
        int has_symbol = 0;
        if (at < length) {
            if (text[at] != '(' || length - at < 2 || text[length - 1] != ')') continue;
            written.start = at + 1;
            written.length = length - 2 - at;
            has_symbol = 1;
        }
        struct span dso = { 0, plus };
        long long offset = span_hex(text, (struct span){ digits, at - digits });
        struct frame frame;
        frame.dso = span_text(text, dso);
        if (strlen(binary_name) == plus && !memcmp(text, binary_name, plus)) {
            frame.symbol = symbols_name(symbols, offset);
        } else if (has_symbol && written.length) {
            frame.symbol = span_text(text, written);
        } else {
            struct buffer name = { 0 };
            buffer_printf(&name, "+0x%llx", (unsigned long long)offset);
            frame.symbol = name.data;
        }
        return frame;
    }
    struct span whole = { 0, length };
    struct frame frame;
    frame.dso = span_text(text, whole);
    frame.symbol = span_text(text, whole);
    return frame;
}

static int allocator(const char *dso) {
    return !strcmp(dso, "libspeedlab-census.so") || !strcmp(dso, "libc.so.6");
}

static int wrapper(const char *symbol) {
    static const char *names[] = { "malloc", "calloc", "realloc", "free", "posix_memalign", "aligned_alloc" };
    for (size_t index = 0; index < sizeof(names) / sizeof(*names); index++)
        if (!strcmp(symbol, names[index])) return 1;
    return 0;
}

static struct json *census_report(const char *text, size_t length, const struct symbols *symbols,
    const char *binary_name) {
    long long total = 0;
    struct table owners = { 0 }, functions = { 0 };
    struct json *sites = json_array();
    size_t site_count = 0;
    struct lines lines = split_lines(text, length);
    for (size_t index = 0; index < lines.count; index++) {
        const char *line = text + lines.starts[index];
        struct span *spans = NULL;
        size_t count = split_tokens(line, lines.lengths[index], &spans);
        if (!count) {
            free(spans);
            continue;
        }
        if (span_equals(line, spans[0], "total")) {
            if (count > 1) total = span_integer(line, spans[1]);
            free(spans);
            continue;
        }
        if (!span_equals(line, spans[0], "site") || count < 3) {
            free(spans);
            continue;
        }
        long long live = span_integer(line, spans[1]);
        long long blocks = span_integer(line, spans[2]);
        size_t frame_count = count > 4 ? count - 4 : 0;
        struct frame *frames = allocate((frame_count ? frame_count : 1) * sizeof(*frames));
        struct frame **meaningful = allocate((frame_count ? frame_count : 1) * sizeof(*meaningful));
        size_t meaningful_count = 0;
        for (size_t frame = 0; frame < frame_count; frame++) {
            frames[frame] = frame_name(line + spans[frame + 4].start, spans[frame + 4].length, symbols, binary_name);
            if (!allocator(frames[frame].dso) && !wrapper(frames[frame].symbol)) meaningful[meaningful_count++] = &frames[frame];
        }
        const char *owner = meaningful_count ? meaningful[0]->dso : "?";
        const char *caller = NULL;
        for (size_t frame = 0; frame < meaningful_count && !caller; frame++)
            if (!strcmp(meaningful[frame]->dso, binary_name)) caller = meaningful[frame]->symbol;
        table_add(&owners, owner, live);
        struct buffer key = { 0 };
        if (caller && caller[0]) {
            buffer_text(&key, caller);
        } else {
            buffer_text(&key, "(no Tomoe frame) ");
            buffer_text(&key, owner);
        }
        table_add(&functions, key.data, live);
        free(key.data);
        if (site_count < 20) {
            struct json *site = json_object();
            struct json *stack = json_array();
            size_t frames_on_stack = meaningful_count < 6 ? meaningful_count : 6;
            for (size_t frame = 0; frame < frames_on_stack; frame++) {
                struct buffer label = { 0 };
                buffer_text(&label, meaningful[frame]->dso);
                buffer_text(&label, ":");
                buffer_text(&label, meaningful[frame]->symbol);
                json_push(stack, json_string_length(label.data, label.length));
                free(label.data);
            }
            json_set(site, "live_kB", json_integer(floor_kilobytes(live)));
            json_set(site, "blocks", json_integer(blocks));
            json_set(site, "stack", stack);
            json_push(sites, site);
            site_count++;
        }
        free(meaningful);
        free(frames);
        free(spans);
    }
    struct json *result = json_object();
    json_set(result, "live_kB", json_integer(floor_kilobytes(total)));
    json_set(result, "by_allocating_library_kB", scaled_table(&owners, (size_t)-1));
    json_set(result, "by_tomoe_caller_kB", scaled_table(&functions, 30));
    json_set(result, "top_sites", sites);
    return result;
}

static struct json *slice_array(struct json *value, size_t limit) {
    if (!value || value->kind != JSON_ARRAY) return node_or_null(value);
    struct json *array = json_array();
    size_t count = value->as.array.count < limit ? value->as.array.count : limit;
    for (size_t index = 0; index < count; index++) json_push(array, value->as.array.items[index]);
    return array;
}

static struct json *log_slice(struct json *value) {
    if (!value) return json_array();
    if (value->kind == JSON_ARRAY) {
        struct json *array = json_array();
        size_t count = value->as.array.count;
        size_t start = count > 20 ? count - 20 : 0;
        for (size_t index = start; index < count; index++) json_push(array, value->as.array.items[index]);
        return array;
    }
    if (value->kind == JSON_STRING) {
        size_t start = character_suffix(value->as.string.text, value->as.string.length, 20);
        return json_string_length(value->as.string.text + start, value->as.string.length - start);
    }
    return value;
}

static char *read_file(const char *path, size_t *length) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) fail_errno(path);
    struct buffer out = { 0 };
    char chunk[65536];
    while (1) {
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR) continue;
            fail_errno(path);
        }
        if (!got) break;
        buffer_append(&out, chunk, (size_t)got);
    }
    close(fd);
    if (!out.data) buffer_append(&out, "", 0);
    *length = out.length;
    return out.data;
}

static struct json *load(const char *path) {
    size_t length = 0;
    char *text = read_file(path, &length);
    char error[256] = { 0 };
    struct json *value = json_parse(text, length, error, sizeof(error));
    if (!value) {
        fprintf(stderr, "speedlab-report: %s: %s\n", path, error[0] ? error : "invalid JSON");
        exit(1);
    }
    return value;
}

static void print_json(struct json *value) {
    struct buffer out = { 0 };
    json_format(&out, value, 1);
    if (out.length) fwrite(out.data, 1, out.length, stdout);
    fputc('\n', stdout);
}

int main(int argc, char **argv) {
    if (argc < 7) fail("usage: speedlab-report PERF CALLGRAPH CENSUS PLAIN BARE BINARY [OUT]");
    struct json *runs[5];
    for (int index = 0; index < 5; index++) runs[index] = load(argv[index + 1]);
    const char *binary = argv[6];
    const char *out = argc > 7 ? argv[7] : NULL;
    struct json *failed[5];
    size_t failed_count = 0;
    for (int index = 0; index < 5; index++)
        if (!json_truth(json_get(runs[index], "ok"))) failed[failed_count++] = runs[index];
    if (failed_count) {
        struct json *body = json_object();
        struct json *errors = json_array();
        struct json *log = json_array();
        for (size_t index = 0; index < failed_count; index++) {
            json_push(errors, node_or_null(json_get(failed[index], "error")));
            json_push(log, log_slice(json_get(failed[index], "log_tail")));
        }
        json_set(body, "ok", json_boolean(0));
        json_set(body, "errors", errors);
        json_set(body, "log", log);
        print_json(body);
        return 1;
    }
    struct symbols symbols = { 0 };
    symbols_load(&symbols, binary);
    const char *slash = strrchr(binary, '/');
    const char *binary_name = slash ? slash + 1 : binary;
    struct json *perf = runs[0], *calls = runs[1], *heap = runs[2];
    struct json *heap_profile = json_get(heap, "profile");
    struct json *phases = heap_profile ? json_get(heap_profile, "census_phases") : NULL;
    struct json *census_texts = heap_profile ? json_get(heap_profile, "census") : NULL;
    struct json *censuses = json_object();
    if (census_texts && census_texts->kind == JSON_OBJECT) {
        for (size_t index = 0; index < census_texts->as.object.count; index++) {
            const struct json_member *member = &census_texts->as.object.members[index];
            size_t dash = member->key_length;
            while (dash && member->key[dash - 1] != '-') dash--;
            char *generation = span_text(member->key, (struct span){ dash, member->key_length - dash });
            struct json *phase = phases && phases->kind == JSON_OBJECT ? json_get(phases, generation) : NULL;
            const char *label = phase && json_text(phase) ? json_text(phase) : generation;
            size_t text_length = 0;
            const char *text = node_text(member->value, &text_length);
            json_set(censuses, label, census_report(text ? text : "", text_length, &symbols, binary_name));
            free(generation);
        }
    }
    struct json *call_profile = json_get(calls, "profile");
    size_t inclusive_length = 0;
    const char *inclusive = node_text(call_profile ? json_get(call_profile, "inclusive") : NULL, &inclusive_length);
    if (!inclusive) {
        inclusive = "";
        inclusive_length = 0;
    }
    struct json *workload = json_object();
    json_set(workload, "virtual_seconds", node_or_null(json_get(perf, "virtual_seconds")));
    json_set(workload, "frames", json_integer((long long)json_length(json_get(perf, "frames"))));
    json_set(workload, "real_seconds", node_or_null(json_get(perf, "real_seconds")));
    struct json *counted = json_object();
    json_set(counted, "with_capture", counted_report(runs[3]));
    json_set(counted, "without_capture", counted_report(runs[4]));
    struct json *heap_memory = json_get(heap, "memory");
    struct json *memory = json_object();
    json_set(memory, "phases", node_or_null(json_get(heap, "phases")));
    json_set(memory, "peak_pss_kB", node_or_null(heap_memory ? json_get(heap_memory, "peak_pss_kB") : NULL));
    json_set(memory, "peak_private_dirty_kB",
        node_or_null(heap_memory ? json_get(heap_memory, "peak_private_dirty_kB") : NULL));
    json_set(memory, "settled", node_or_null(heap_memory ? json_get(heap_memory, "settled") : NULL));
    json_set(memory, "mappings", slice_array(heap_memory ? json_get(heap_memory, "breakdown") : NULL, 25));
    struct json *report = json_object();
    json_set(report, "ok", json_boolean(1));
    json_set(report, "workload", workload);
    json_set(report, "sample_period_instructions", json_integer(PERIOD));
    json_set(report, "counted", counted);
    json_set(report, "cost", cost_report(json_get(perf, "profile"), inclusive, inclusive_length));
    json_set(report, "memory", memory);
    json_set(report, "heap", censuses);
    print_json(report);
    if (out && *out) {
        struct buffer text = { 0 };
        json_format(&text, report, 1);
        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0) fail_errno(out);
        size_t written = 0;
        while (written < text.length) {
            ssize_t put = write(fd, text.data + written, text.length - written);
            if (put < 0) {
                if (errno == EINTR) continue;
                fail_errno(out);
            }
            written += (size_t)put;
        }
        if (close(fd)) fail_errno(out);
    }
    return 0;
}
