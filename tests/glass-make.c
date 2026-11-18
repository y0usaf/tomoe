#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SIZE 32

static unsigned char *cursor(const unsigned char tint[3], unsigned alpha) {
    unsigned char *out = calloc(SIZE * SIZE * 4, 1);
    if (!out) exit(1);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            unsigned char *pixel = out + ((size_t)y * SIZE + x) * 4;
            double body_right = 2 + (y - 2) * 0.7;
            double shadow_right = 4 + (y - 5) * 0.7;
            int body = x >= 2 && (double)x <= body_right && y >= 2 && y <= 26;
            int shadow = x >= 4 && (double)x <= shadow_right && y >= 5 && y <= 29;
            if (body) {
                pixel[0] = (unsigned char)(tint[2] * alpha / 255);
                pixel[1] = (unsigned char)(tint[1] * alpha / 255);
                pixel[2] = (unsigned char)(tint[0] * alpha / 255);
                pixel[3] = alpha;
            } else if (shadow) {
                pixel[0] = 0;
                pixel[1] = 0;
                pixel[2] = 0;
                pixel[3] = 0x50;
            }
        }
    }
    return out;
}

static void put32(unsigned char *at, unsigned value) {
    at[0] = (unsigned char)(value & 0xff);
    at[1] = (unsigned char)((value >> 8) & 0xff);
    at[2] = (unsigned char)((value >> 16) & 0xff);
    at[3] = (unsigned char)((value >> 24) & 0xff);
}

static void write_theme(const char *icons, const char *variant, const unsigned char tint[3]) {
    char directory[2048], path[4096];
    snprintf(directory, sizeof directory, "%s/%s/cursors", icons, variant);
    char partial[2048];
    snprintf(partial, sizeof partial, "%s", directory);
    for (char *at = partial + 1; *at; at++) {
        if (*at != '/') continue;
        *at = '\0';
        mkdir(partial, 0755);
        *at = '/';
    }
    mkdir(partial, 0755);
    unsigned char *pixels = cursor(tint, 0x60);
    snprintf(path, sizeof path, "%s/default", directory);
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "glass-make: cannot write %s\n", path);
        exit(1);
    }
    unsigned char header[64];
    static const unsigned fields[16] = {
        0x72756358, 16, 0x10000, 1, 0xfffd0002, SIZE, 28, 36,
        0xfffd0002, SIZE, 1, SIZE, SIZE, 2, 2, 0
    };
    for (int index = 0; index < 16; index++) put32(header + index * 4, fields[index]);
    fwrite(header, 1, sizeof header, file);
    fwrite(pixels, 1, SIZE * SIZE * 4, file);
    if (fclose(file) != 0) exit(1);
    free(pixels);
    snprintf(path, sizeof path, "%s/left_ptr", directory);
    unlink(path);
    if (symlink("default", path) != 0) {
        fprintf(stderr, "glass-make: cannot link %s\n", path);
        exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s ICONS\n", argv[0]);
        return 2;
    }
    static const unsigned char dark[3] = { 0xDC, 0xE2, 0xFF };
    static const unsigned char light[3] = { 0x3A, 0x3E, 0x58 };
    write_theme(argv[1], "glass-dark", dark);
    write_theme(argv[1], "glass-light", light);
    FILE *file = fopen("/run/stripes.ppm", "wb");
    if (!file) {
        fprintf(stderr, "glass-make: cannot write /run/stripes.ppm\n");
        return 1;
    }
    fputs("P6\n1280 720\n255\n", file);
    unsigned char row[1280 * 3];
    for (int x = 0; x < 1280; x++) {
        int bright = (x / 3) % 2;
        row[x * 3 + 0] = bright ? 0xF0 : 0x10;
        row[x * 3 + 1] = bright ? 0xF0 : 0x10;
        row[x * 3 + 2] = bright ? 0xF0 : 0x30;
    }
    for (int y = 0; y < 720; y++) fwrite(row, 1, sizeof row, file);
    if (fclose(file) != 0) return 1;
    return 0;
}
