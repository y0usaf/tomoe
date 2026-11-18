#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SUBSCRIBE "{\"id\":1,\"method\":\"subscribe\",\"params\":{\"events\":[\"pointer_motion\",\"pointer_button\",\"key\"]}}\n"

struct output {
    char *text;
    size_t used, capacity;
    int valid;
    int has_event;
    int id_one;
};

static double now_seconds(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (double)time.tv_sec + (double)time.tv_nsec / 1e9;
}

static void append(struct output *out, const char *text, size_t size) {
    if (!out->valid) return;
    if (out->used + size + 1 > out->capacity) {
        size_t capacity = out->capacity ? out->capacity : 256;
        while (out->used + size + 1 > capacity) capacity *= 2;
        char *grown = realloc(out->text, capacity);
        if (!grown) {
            out->valid = 0;
            return;
        }
        out->text = grown;
        out->capacity = capacity;
    }
    memcpy(out->text + out->used, text, size);
    out->used += size;
    out->text[out->used] = '\0';
}

struct parser {
    const char *at;
    const char *end;
    int depth;
    struct output *out;
};

static void skip_space(struct parser *parser) {
    while (parser->at < parser->end) {
        char value = *parser->at;
        if (value != ' ' && value != '\t' && value != '\n' && value != '\r') break;
        parser->at++;
    }
}

static int parse_string(struct parser *parser, const char **text, size_t *size) {
    if (parser->at >= parser->end || *parser->at != '"') return 0;
    const char *start = parser->at++;
    while (parser->at < parser->end) {
        char value = *parser->at;
        if (value == '\\') {
            parser->at += 2;
            continue;
        }
        if (value == '"') {
            parser->at++;
            *text = start;
            *size = (size_t)(parser->at - start);
            return 1;
        }
        if ((unsigned char)value < 0x20) return 0;
        parser->at++;
    }
    return 0;
}

static int parse_value(struct parser *parser);

static int parse_object(struct parser *parser) {
    struct output *out = parser->out;
    int outer = parser->depth;
    parser->depth++;
    append(out, "{", 1);
    skip_space(parser);
    if (parser->at < parser->end && *parser->at == '}') {
        parser->at++;
        parser->depth--;
        append(out, "}", 1);
        return 1;
    }
    int count = 0;
    for (;;) {
        skip_space(parser);
        const char *key = NULL;
        size_t key_size = 0;
        if (!parse_string(parser, &key, &key_size)) return 0;
        skip_space(parser);
        if (parser->at >= parser->end || *parser->at != ':') return 0;
        parser->at++;
        if (count++) append(out, ", ", 2);
        append(out, key, key_size);
        append(out, ": ", 2);
        size_t before = out->used;
        if (!parse_value(parser)) return 0;
        if (outer == 0) {
            if (key_size == 7 && memcmp(key, "\"event\"", 7) == 0) out->has_event = 1;
            if (key_size == 4 && memcmp(key, "\"id\"", 4) == 0 &&
                    out->used - before == 1 && out->text[before] == '1')
                out->id_one = 1;
        }
        skip_space(parser);
        if (parser->at >= parser->end) return 0;
        if (*parser->at == ',') {
            parser->at++;
            continue;
        }
        if (*parser->at == '}') {
            parser->at++;
            break;
        }
        return 0;
    }
    parser->depth--;
    append(out, "}", 1);
    return 1;
}

static int parse_array(struct parser *parser) {
    struct output *out = parser->out;
    parser->depth++;
    append(out, "[", 1);
    skip_space(parser);
    if (parser->at < parser->end && *parser->at == ']') {
        parser->at++;
        parser->depth--;
        append(out, "]", 1);
        return 1;
    }
    int count = 0;
    for (;;) {
        if (count++) append(out, ", ", 2);
        if (!parse_value(parser)) return 0;
        skip_space(parser);
        if (parser->at >= parser->end) return 0;
        if (*parser->at == ',') {
            parser->at++;
            continue;
        }
        if (*parser->at == ']') {
            parser->at++;
            break;
        }
        return 0;
    }
    parser->depth--;
    append(out, "]", 1);
    return 1;
}

static int parse_value(struct parser *parser) {
    skip_space(parser);
    if (parser->at >= parser->end) return 0;
    char value = *parser->at;
    if (value == '{') {
        parser->at++;
        return parse_object(parser);
    }
    if (value == '[') {
        parser->at++;
        return parse_array(parser);
    }
    if (value == '"') {
        const char *text = NULL;
        size_t size = 0;
        if (!parse_string(parser, &text, &size)) return 0;
        append(parser->out, text, size);
        return 1;
    }
    const char *start = parser->at;
    while (parser->at < parser->end && *parser->at != ',' && *parser->at != ']' &&
            *parser->at != '}' && *parser->at != ' ' && *parser->at != '\t' &&
            *parser->at != '\n' && *parser->at != '\r')
        parser->at++;
    size_t size = (size_t)(parser->at - start);
    if (!size) return 0;
    append(parser->out, start, size);
    return 1;
}

static int parse_json(const char *text, size_t size, struct output *out) {
    struct parser parser = { text, text + size, 0, out };
    out->valid = 1;
    out->has_event = 0;
    out->id_one = 0;
    if (!parse_value(&parser)) {
        out->valid = 0;
        return 0;
    }
    skip_space(&parser);
    return out->valid && parser.at == text + size;
}

static void write_flag(const char *path) {
    FILE *file = fopen(path, "w");
    if (!file) return;
    fputs("ready\n", file);
    fclose(file);
}

static int connect_to(const char *path, int retry) {
    int connection = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connection < 0) return -1;
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    size_t path_length = strlen(path);
    if (path_length >= sizeof address.sun_path) {
        fprintf(stderr, "socket path is too long\n");
        return 1;
    }
    memcpy(address.sun_path, path, path_length + 1);
    double deadline = now_seconds() + 30;
    for (;;) {
        if (connect(connection, (struct sockaddr *)&address, sizeof address) == 0) break;
        if (!retry || now_seconds() > deadline) {
            close(connection);
            return -1;
        }
        struct timespec pause = { 0, 50000000 };
        nanosleep(&pause, NULL);
    }
    struct timeval timeout = { 0, 100000 };
    setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    return connection;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s PATH LOG READY STOP [retry]\n", argv[0]);
        return 2;
    }
    int retry = argc > 5 && strcmp(argv[5], "retry") == 0;
    int connection = connect_to(argv[1], retry);
    if (connection < 0) {
        fprintf(stderr, "event-reader: cannot connect to %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    size_t sent = 0;
    size_t wanted = strlen(SUBSCRIBE);
    while (sent < wanted) {
        ssize_t used = send(connection, SUBSCRIBE + sent, wanted - sent, 0);
        if (used < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "event-reader: cannot subscribe: %s\n", strerror(errno));
            return 1;
        }
        sent += (size_t)used;
    }
    FILE *log = fopen(argv[2], "w");
    if (!log) {
        fprintf(stderr, "event-reader: cannot write %s\n", argv[2]);
        return 1;
    }
    setvbuf(log, NULL, _IOLBF, 0);
    char *buffer = malloc(1 << 20);
    size_t used = 0;
    double deadline = now_seconds() + 600;
    while (now_seconds() < deadline) {
        struct stat status;
        if (stat(argv[4], &status) == 0) break;
        unsigned char chunk[65536];
        ssize_t got = recv(connection, chunk, sizeof chunk, 0);
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        if (used + (size_t)got > (1 << 20)) used = 0;
        memcpy(buffer + used, chunk, (size_t)got);
        used += (size_t)got;
        size_t start = 0;
        for (size_t index = 0; index < used; index++) {
            if (buffer[index] != '\n') continue;
            struct output out = { 0 };
            if (parse_json(buffer + start, index - start, &out) && out.valid) {
                if (out.has_event) {
                    fwrite(out.text, 1, out.used, log);
                    fputc('\n', log);
                    fflush(log);
                } else if (out.id_one) {
                    write_flag(argv[3]);
                }
            }
            free(out.text);
            start = index + 1;
        }
        if (start) {
            memmove(buffer, buffer + start, used - start);
            used -= start;
        }
    }
    fclose(log);
    close(connection);
    free(buffer);
    return 0;
}
