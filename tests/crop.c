#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "crop: cannot open %s\n", path);
        exit(1);
    }
    size_t capacity = 1 << 20, size = 0;
    unsigned char *data = malloc(capacity);
    if (!data) exit(1);
    for (;;) {
        if (size == capacity) {
            capacity *= 2;
            unsigned char *grown = realloc(data, capacity);
            if (!grown) exit(1);
            data = grown;
        }
        size_t got = fread(data + size, 1, capacity - size, file);
        size += got;
        if (got == 0) break;
    }
    fclose(file);
    *length = size;
    return data;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s PPM X,Y,W,H\n", argv[0]);
        return 2;
    }
    size_t size;
    unsigned char *raw = slurp(argv[1], &size);
    int newlines = 0;
    size_t body = 0;
    for (size_t index = 0; index < size; index++) {
        if (raw[index] != '\n') continue;
        if (++newlines == 3) {
            body = index + 1;
            break;
        }
    }
    if (newlines < 3) {
        fprintf(stderr, "crop: %s is not a PPM with a header\n", argv[1]);
        return 1;
    }
    size_t header = body;
    int width = 0, height = 0;
    {
        size_t at = 0;
        while (at < header && raw[at] != '\n') at++;
        at++;
        width = (int)strtol((const char *)raw + at, NULL, 10);
        while (at < header && raw[at] != ' ') at++;
        at++;
        height = (int)strtol((const char *)raw + at, NULL, 10);
    }
    if (width <= 0 || height <= 0) {
        fprintf(stderr, "crop: %s has no size\n", argv[1]);
        return 1;
    }
    int x = 0, y = 0, w = 0, h = 0;
    if (sscanf(argv[2], "%d,%d,%d,%d", &x, &y, &w, &h) != 4) {
        fprintf(stderr, "crop: bad box %s\n", argv[2]);
        return 2;
    }
    size_t stride = (size_t)width * 3;
    for (int row = 0; row < h; row++) {
        size_t start = (size_t)(y + row) * stride + (size_t)x * 3;
        size_t count = (size_t)w * 3;
        if (start >= size - header) continue;
        if (start + count > size - header) count = size - header - start;
        fwrite(raw + header + start, 1, count, stdout);
    }
    fflush(stdout);
    free(raw);
    return 0;
}
