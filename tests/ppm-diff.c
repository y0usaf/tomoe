#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct image {
    int width, height;
    unsigned char *body;
    size_t length;
};

static void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static struct image load(const char *path) {
    struct image image = { 0, 0, NULL, 0 };
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "ppm-diff: cannot open %s\n", path);
        exit(1);
    }
    size_t capacity = 1 << 20, size = 0;
    unsigned char *raw = malloc(capacity);
    if (!raw) exit(1);
    for (;;) {
        if (size == capacity) {
            capacity *= 2;
            unsigned char *grown = realloc(raw, capacity);
            if (!grown) exit(1);
            raw = grown;
        }
        size_t got = fread(raw + size, 1, capacity - size, file);
        size += got;
        if (got == 0) break;
    }
    fclose(file);
    int newlines = 0;
    size_t body = 0;
    for (size_t index = 0; index < size; index++) {
        if (raw[index] != '\n') continue;
        if (++newlines == 3) {
            body = index + 1;
            break;
        }
    }
    if (newlines < 3) fail("ppm-diff: a frame is not a PPM with a header");
    size_t at = 0;
    while (at < body && raw[at] != '\n') at++;
    at++;
    image.width = (int)strtol((const char *)raw + at, NULL, 10);
    while (at < body && raw[at] != ' ') at++;
    at++;
    image.height = (int)strtol((const char *)raw + at, NULL, 10);
    image.length = size - body;
    image.body = raw + body;
    return image;
}

static int pixel(const struct image *image, int x, int y, int out[3]) {
    size_t index = ((size_t)y * image->width + x) * 3;
    int count = 0;
    for (int channel = 0; channel < 3; channel++)
        if (index + (size_t)channel < image->length) {
            out[channel] = image->body[index + channel];
            count++;
        }
    return count;
}

static void print_pixel(FILE *stream, const int *values, int count) {
    fputc('(', stream);
    for (int channel = 0; channel < count; channel++)
        fprintf(stream, channel ? ", %d" : "%d", values[channel]);
    fputc(')', stream);
}

static int same(const int *first, int first_count, const int *second, int second_count) {
    if (first_count != second_count) return 0;
    for (int channel = 0; channel < first_count; channel++)
        if (first[channel] != second[channel]) return 0;
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s FIRST SECOND X,Y,W,H\n", argv[0]);
        return 2;
    }
    struct image first = load(argv[1]);
    struct image second = load(argv[2]);
    if (first.width != second.width || first.height != second.height ||
            first.length != second.length)
        fail("ppm-diff: the frames do not have the same shape");
    unsigned long max = 0, sum = 0;
    for (size_t index = 0; index < first.length; index++) {
        int difference = (int)first.body[index] - (int)second.body[index];
        if (difference < 0) difference = -difference;
        if ((unsigned long)difference > max) max = (unsigned long)difference;
        sum += (unsigned long)difference;
    }
    double mean = first.length ? (double)sum / (double)first.length : 0.0;
    int hx = 0, hy = 0, hw = 0, hh = 0;
    if (sscanf(argv[3], "%d,%d,%d,%d", &hx, &hy, &hw, &hh) != 4) {
        fprintf(stderr, "ppm-diff: bad box %s\n", argv[3]);
        return 2;
    }
    int bar[3], corner[3], corner_other[3], video[3];
    int bar_count = pixel(&first, hx + hw / 2, hy + 10, bar);
    int corner_count = pixel(&first, hx, hy + hh - 1, corner);
    int other_count = pixel(&second, hx, hy + hh - 1, corner_other);
    int video_count = pixel(&first, hx + hw / 2, hy + hh / 2, video);
    printf("max_diff=%lu mean_diff=%.5f bar=", max, mean);
    print_pixel(stdout, bar, bar_count);
    fputs(" corner_out=", stdout);
    print_pixel(stdout, corner, corner_count);
    fputs(" video=", stdout);
    print_pixel(stdout, video, video_count);
    fputs("\n", stdout);
    fflush(stdout);
    if (max > 3) {
        fprintf(stderr, "max channel diff %lu > 3\n", max);
        return 1;
    }
    if (!(mean < 0.1)) {
        fprintf(stderr, "mean channel diff %f >= 0.1\n", mean);
        return 1;
    }
    int expected[3] = { 255, 136, 0 };
    if (!same(bar, bar_count, expected, 3)) {
        fputs("bar pixel is ", stderr);
        print_pixel(stdout, bar, bar_count);
        fputs(", not (255, 136, 0)\n", stderr);
        return 1;
    }
    if (!same(corner, corner_count, corner_other, other_count)) {
        fputs("corner pixel differs: ", stderr);
        print_pixel(stderr, corner, corner_count);
        fputs(" against ", stderr);
        print_pixel(stderr, corner_other, other_count);
        fputs("\n", stderr);
        return 1;
    }
    return 0;
}
