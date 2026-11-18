#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct category {
    char name[64];
    long long rss, pss, mapped, count;
    int rss_order;
    int has_rss;
};

struct row {
    int category;
    char *name;
    long long size, rss;
    int order;
};

static struct category categories[64];
static int category_count;
static int rss_sequence;
static struct row **rows;
static int row_count, row_capacity;

static int category_index(const char *name) {
    for (int index = 0; index < category_count; index++)
        if (strcmp(categories[index].name, name) == 0) return index;
    if (category_count == (int)(sizeof categories / sizeof categories[0])) {
        fprintf(stderr, "memory-attribute: too many categories\n");
        exit(1);
    }
    struct category *entry = &categories[category_count];
    memset(entry, 0, sizeof *entry);
    snprintf(entry->name, sizeof entry->name, "%s", name);
    return category_count++;
}

static int any(const char *haystack, const char *const *needles) {
    for (const char *const *needle = needles; *needle; needle++)
        if (strstr(haystack, *needle)) return 1;
    return 0;
}

static const char *base_name(const char *name) {
    const char *slash = strrchr(name, '/');
    return slash ? slash + 1 : name;
}

static int ends_with(const char *text, const char *suffix) {
    size_t length = strlen(text), wanted = strlen(suffix);
    return length >= wanted && strcmp(text + length - wanted, suffix) == 0;
}

static const char *categorise(const char *permission, const char *name, long long size,
        const char *binary) {
    static const char *const graphics[] = {
        "libGL", "libEGL", "libgbm", "libglapi", "libvulkan", "libdrm", "mesa",
        "llvmpipe", "swrast", "zink", "libLLVM", "libnvidia", "libcuda", NULL
    };
    static const char *const fonts[] = { "/share/fonts/", "/fontconfig/", NULL };
    if (!name[0])
        return size >= 100LL * 1048576 && strchr(permission, 'x') ? "lisp space" : "anon";
    const char *base = base_name(name);
    if (strcmp(base, "[heap]") == 0) return "malloc heap";
    if (strcmp(name, binary) == 0 || strcmp(base, "tomoe") == 0) return "tomoe image";
    if (strncmp(name, "/dev/shm", 8) == 0 || strstr(name, "memfd:") ||
            strncmp(name, "[shm", 4) == 0) return "shm";
    if (ends_with(base, ".ttf") || ends_with(base, ".otf") || ends_with(base, ".ttc") ||
            any(name, fonts)) return "fonts";
    if (any(base, graphics)) return "graphics";
    if (strstr(base, ".so")) return "libraries";
    return "files";
}

static int is_hex(char value) {
    return isdigit((unsigned char)value) || (value >= 'a' && value <= 'f');
}

static int parse_map(const char *line, long long *start, long long *end, char *permission,
        char *name) {
    const char *at = line;
    if (!is_hex(*at)) return 0;
    char *stop;
    *start = strtoll(at, &stop, 16);
    if (*stop != '-') return 0;
    at = stop + 1;
    if (!is_hex(*at)) return 0;
    *end = strtoll(at, &stop, 16);
    if (*stop != ' ') return 0;
    at = stop + 1;
    int width = 0;
    while (*at && *at != ' ' && width < 63) permission[width++] = *at++;
    permission[width] = '\0';
    if (!width || *at != ' ') return 0;
    for (int field = 0; field < 3; field++) {
        while (*at == ' ') at++;
        if (!*at) return field == 2;
        while (*at && *at != ' ') at++;
    }
    while (*at == ' ' || *at == '\t') at++;
    snprintf(name, 4096, "%s", at);
    size_t length = strlen(name);
    while (length && (name[length - 1] == '\n' || name[length - 1] == '\r'))
        name[--length] = '\0';
    return 1;
}

static int sort_categories(const void *left, const void *right) {
    const struct category *a = *(const struct category *const *)left;
    const struct category *b = *(const struct category *const *)right;
    if (a->pss != b->pss) return a->pss > b->pss ? -1 : 1;
    return a->rss_order - b->rss_order;
}

static int sort_rows(const void *left, const void *right) {
    const struct row *a = *(const struct row *const *)left;
    const struct row *b = *(const struct row *const *)right;
    if (a->rss != b->rss) return a->rss > b->rss ? -1 : 1;
    return a->order - b->order;
}

static void status_value(const char *pid, const char *key, char *out, size_t size) {
    char path[256];
    snprintf(path, sizeof path, "/proc/%s/status", pid);
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "memory-attribute: cannot read %s\n", path);
        exit(1);
    }
    char line[4096];
    out[0] = '\0';
    size_t length = strlen(key);
    while (fgets(line, sizeof line, file)) {
        if (strncmp(line, key, length) != 0 || line[length] != ':') continue;
        snprintf(out, size, "%s", line + length + 1);
    }
    fclose(file);
    size_t at = strlen(out);
    while (at && (out[at - 1] == '\n' || out[at - 1] == '\r' || out[at - 1] == ' '))
        out[--at] = '\0';
    char *start = out;
    while (*start == ' ' || *start == '\t') start++;
    if (start != out) memmove(out, start, strlen(start) + 1);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s PID [BINARY]\n", argv[0]);
        return 2;
    }
    const char *pid = argv[1];
    const char *binary = argc > 2 ? argv[2] : "";
    char path[256];
    snprintf(path, sizeof path, "/proc/%s/smaps", pid);
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "memory-attribute: cannot read %s\n", path);
        return 1;
    }
    row_capacity = 4096;
    rows = malloc(sizeof rows[0] * (size_t)row_capacity);
    if (!rows) return 1;
    char line[8192];
    int current = -1;
    while (fgets(line, sizeof line, file)) {
        long long start, end;
        char permission[64], name[4096];
        if (parse_map(line, &start, &end, permission, name)) {
            int index = category_index(categorise(permission, name, end - start, binary));
            struct category *entry = &categories[index];
            entry->mapped += end - start;
            entry->count += 1;
            current = index;
            if (row_count == row_capacity) {
                row_capacity *= 2;
                struct row **grown = realloc(rows, sizeof rows[0] * (size_t)row_capacity);
                if (!grown) return 1;
                rows = grown;
            }
            struct row *row = malloc(sizeof *row);
            if (!row) return 1;
            row->category = index;
            row->name = strdup(name[0] ? name : "(anon)");
            row->size = end - start;
            row->rss = 0;
            row->order = row_count;
            rows[row_count++] = row;
        } else if (strncmp(line, "Rss:", 4) == 0) {
            if (current < 0) continue;
            long long value = strtoll(line + 4, NULL, 10);
            struct category *entry = &categories[current];
            entry->rss += value;
            if (!entry->has_rss) {
                entry->has_rss = 1;
                entry->rss_order = rss_sequence++;
            }
            rows[row_count - 1]->rss += value;
        } else if (strncmp(line, "Pss:", 4) == 0) {
            if (current < 0) continue;
            categories[current].pss += strtoll(line + 4, NULL, 10);
        }
    }
    fclose(file);
    struct category *ordered[64];
    int ordered_count = 0;
    long long rss_total = 0, pss_total = 0;
    for (int index = 0; index < category_count; index++) {
        if (!categories[index].has_rss) continue;
        ordered[ordered_count++] = &categories[index];
        rss_total += categories[index].rss;
        pss_total += categories[index].pss;
    }
    qsort(ordered, (size_t)ordered_count, sizeof ordered[0], sort_categories);
    printf("category rss_kb pss_kb mapped_kb mappings\n");
    for (int index = 0; index < ordered_count; index++) {
        struct category *entry = ordered[index];
        printf("%s %lld %lld %lld %lld\n", entry->name, entry->rss, entry->pss,
               entry->mapped / 1024, entry->count);
    }
    char size_text[256], threads[256];
    status_value(pid, "VmSize", size_text, sizeof size_text);
    status_value(pid, "Threads", threads, sizeof threads);
    size_text[strcspn(size_text, " \t")] = '\0';
    printf("total %lld %lld %s %s\n", rss_total, pss_total, size_text, threads);
    qsort(rows, (size_t)row_count, sizeof rows[0], sort_rows);
    int shown = 0;
    for (int index = 0; index < row_count && shown < 12; index++) {
        struct row *row = rows[index];
        if (row->rss < 1024) continue;
        printf("mapping %s %s %lld %lld\n", categories[row->category].name, row->name,
               row->size / 1024, row->rss);
        shown++;
    }
    return 0;
}
