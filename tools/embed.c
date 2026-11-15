#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int replace_if_changed(const char *temporary, const char *target) {
    FILE *old = fopen(target, "rb");
    if (old) {
        FILE *fresh = fopen(temporary, "rb");
        int same = 0;
        if (fresh) {
            same = 1;
            for (;;) {
                int a = fgetc(old), b = fgetc(fresh);
                if (a != b) {
                    same = 0;
                    break;
                }
                if (a == EOF) break;
            }
            fclose(fresh);
        }
        fclose(old);
        if (same) {
            remove(temporary);
            return 0;
        }
    }
    return rename(temporary, target);
}

static char *read_file(const char *path, size_t *length) {
    FILE *in = fopen(path, "rb");
    if (!in) {
        fprintf(stderr, "embed: cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }
    size_t capacity = 65536, used = 0;
    char *data = malloc(capacity);
    if (!data) {
        fclose(in);
        return NULL;
    }
    for (;;) {
        if (used == capacity) {
            capacity *= 2;
            char *grown = realloc(data, capacity);
            if (!grown) {
                free(data);
                fclose(in);
                return NULL;
            }
            data = grown;
        }
        size_t count = fread(data + used, 1, capacity - used, in);
        used += count;
        if (count == 0) break;
    }
    if (ferror(in)) {
        fprintf(stderr, "embed: cannot read %s\n", path);
        free(data);
        fclose(in);
        return NULL;
    }
    fclose(in);
    *length = used;
    return data;
}

int main(int argc, char **argv) {
    if (argc < 2 || (argc - 2) % 2) {
        fprintf(stderr, "usage: embed OUTPUT [NAME PATH ...]\n");
        return 1;
    }
    size_t target_length = strlen(argv[1]);
    char *temporary = malloc(target_length + 5);
    if (!temporary) return 1;
    memcpy(temporary, argv[1], target_length);
    memcpy(temporary + target_length, ".tmp", 5);
    FILE *out = fopen(temporary, "w");
    if (!out) {
        fprintf(stderr, "embed: cannot create %s: %s\n", temporary, strerror(errno));
        free(temporary);
        return 1;
    }
    int count = (argc - 2) / 2;
    size_t *lengths = calloc((size_t)(count ? count : 1), sizeof(*lengths));
    if (!lengths) {
        fclose(out);
        return 1;
    }
    fprintf(out, "#include \"lisp.h\"\n\n");
    for (int i = 0; i < count; i++) {
        const char *name = argv[2 + i * 2];
        const char *path = argv[3 + i * 2];
        size_t length = 0;
        char *data = read_file(path, &length);
        if (!data) {
            fclose(out);
            free(lengths);
            remove(temporary);
            free(temporary);
            return 1;
        }
        lengths[i] = length;
        fprintf(out, "static const char source_%d[] = {", i);
        for (size_t j = 0; j < length; j++) {
            if (j % 16 == 0) fprintf(out, "\n    ");
            fprintf(out, "%u,", (unsigned char)data[j]);
        }
        if (!length) fprintf(out, "0");
        fprintf(out, "\n};\n");
        free(data);
        (void)name;
    }
    fprintf(out, "\nstatic const struct lisp_builtin_source tomoe_builtin_sources[] = {\n");
    if (!count) fprintf(out, "    { 0, 0, 0 }\n");
    for (int i = 0; i < count; i++) {
        fprintf(out, "    { \"%s\", source_%d, %zu },\n", argv[2 + i * 2], i, lengths[i]);
    }
    fprintf(out, "};\n");
    fprintf(out, "static const size_t tomoe_builtin_source_count = %d;\n", count);
    free(lengths);
    if (fclose(out)) {
        fprintf(stderr, "embed: cannot write %s: %s\n", temporary, strerror(errno));
        remove(temporary);
        free(temporary);
        return 1;
    }
    int status = replace_if_changed(temporary, argv[1]);
    if (status) fprintf(stderr, "embed: cannot replace %s: %s\n", argv[1], strerror(errno));
    free(temporary);
    return status ? 1 : 0;
}
