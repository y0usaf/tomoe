#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "json.h"

static char *slurp(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "speedlab-palette: cannot read %s\n", path);
        exit(2);
    }
    struct buffer text = { 0 };
    buffer_text(&text, "");
    char chunk[1 << 16];
    ssize_t got;
    while ((got = read(fd, chunk, sizeof(chunk))) > 0) buffer_append(&text, chunk, (size_t)got);
    close(fd);
    if (got < 0) {
        fprintf(stderr, "speedlab-palette: cannot read %s\n", path);
        exit(2);
    }
    return text.data;
}

static void spill(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "speedlab-palette: cannot write %s\n", path);
        exit(2);
    }
    size_t left = strlen(text);
    while (left) {
        ssize_t written = write(fd, text, left);
        if (written <= 0) {
            fprintf(stderr, "speedlab-palette: cannot write %s\n", path);
            exit(2);
        }
        text += written;
        left -= (size_t)written;
    }
    close(fd);
}

static int space(char byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\f' || byte == '\v';
}

static int hex(char byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'F') || (byte >= 'a' && byte <= 'f');
}

struct colour {
    char *name;
    char *value;
};

static struct colour *colours;
static size_t colour_count, colour_capacity;

static void remember(const char *name, size_t name_length, const char *value) {
    for (size_t index = 0; index < colour_count; index++) {
        if (strlen(colours[index].name) != name_length || memcmp(colours[index].name, name, name_length)) continue;
        free(colours[index].value);
        colours[index].value = allocate(8);
        memcpy(colours[index].value, value, 7);
        colours[index].value[7] = '\0';
        return;
    }
    colours = allocate_more(colours, &colour_capacity, colour_count + 1, sizeof(*colours));
    colours[colour_count].name = allocate(name_length + 1);
    memcpy(colours[colour_count].name, name, name_length);
    colours[colour_count].name[name_length] = '\0';
    colours[colour_count].value = allocate(8);
    memcpy(colours[colour_count].value, value, 7);
    colours[colour_count].value[7] = '\0';
    colour_count++;
}

static void scan(const char *text) {
    static const char marker[] = "@define-color";
    size_t marker_length = sizeof(marker) - 1;
    for (size_t at = 0; text[at]; at++) {
        if (strncmp(text + at, marker, marker_length)) continue;
        size_t cursor = at + marker_length;
        if (!space(text[cursor])) continue;
        while (space(text[cursor])) cursor++;
        size_t name_at = cursor;
        while (text[cursor] && !space(text[cursor])) cursor++;
        if (cursor == name_at) continue;
        size_t name_end = cursor;
        if (!space(text[cursor])) continue;
        while (space(text[cursor])) cursor++;
        if (text[cursor] != '#' || !hex(text[cursor + 1]) || !hex(text[cursor + 2]) || !hex(text[cursor + 3]) ||
            !hex(text[cursor + 4]) || !hex(text[cursor + 5]) || !hex(text[cursor + 6]))
            continue;
        remember(text + name_at, name_end - name_at, text + cursor);
    }
}

static const char *value_of(const char *name) {
    for (size_t index = 0; index < colour_count; index++)
        if (!strcmp(colours[index].name, name)) return colours[index].value;
    fprintf(stderr, "speedlab-palette: KeyError: '%s'\n", name);
    exit(1);
}

static double luminance(const char *colour) {
    static const double weights[3] = { 0.2126, 0.7152, 0.0722 };
    static const int offsets[3] = { 1, 3, 5 };
    double total = 0;
    for (int index = 0; index < 3; index++) {
        char pair[3] = { colour[offsets[index]], colour[offsets[index] + 1], '\0' };
        total += weights[index] * (double)strtol(pair, NULL, 16);
    }
    return total;
}

static char *replace(const char *text, const char *from, const char *to) {
    struct buffer out = { 0 };
    buffer_text(&out, "");
    size_t from_length = strlen(from), to_length = strlen(to);
    if (!from_length) {
        buffer_text(&out, text);
        return out.data;
    }
    const char *at = text;
    while (1) {
        const char *found = strstr(at, from);
        if (!found) break;
        buffer_append(&out, at, (size_t)(found - at));
        buffer_append(&out, to, to_length);
        at = found + from_length;
    }
    buffer_text(&out, at);
    return out.data;
}

static char *swap(const char *text, const char *first, const char *second) {
    char *exchanged = replace(text, first, "@SWAP@");
    char *again = replace(exchanged, second, first);
    char *last = replace(again, "@SWAP@", second);
    free(exchanged);
    free(again);
    return last;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("usage: speedlab-palette DARK LIGHT\n", stderr);
        return 2;
    }
    char *text = slurp(argv[1]);
    scan(text);
    const char *bg = value_of("bg"), *fg = value_of("fg");
    int light_on_dark = luminance(bg) < luminance(fg);
    const char *dark_background = light_on_dark ? bg : fg;
    const char *light_background = light_on_dark ? fg : bg;
    char *dark = !strcmp(dark_background, bg) ? text : swap(text, bg, fg);
    char *light = swap(dark, dark_background, light_background);
    spill(argv[1], dark);
    spill(argv[2], light);
    return 0;
}
