#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "resources-internal.h"

#define WATCH_RETRY_MS 250

static const char *watch_status_name(int status) {
    switch (status) {
    case WATCH_CHANGED: return "CHANGED";
    case WATCH_OVERFLOW: return "OVERFLOW";
    case WATCH_UNAVAILABLE: return "UNAVAILABLE";
    case WATCH_RESTORED: return "RESTORED";
    case WATCH_READ_ERROR: return "READ-ERROR";
    case WATCH_MISSING: return "MISSING";
    default: return "READY";
    }
}

static bool watch_utf8(const unsigned char *text, size_t length, char **out, size_t *out_length) {
    char *decoded = malloc(length * 3 + 1);
    if (!decoded) return false;
    size_t written = 0;
    size_t index = 0;
    while (index < length) {
        unsigned char first = text[index];
        uint32_t code = 0;
        size_t width = 0;
        if (first < 0x80) { code = first; width = 1; }
        else if ((first & 0xE0) == 0xC0) { code = first & 0x1F; width = 2; }
        else if ((first & 0xF0) == 0xE0) { code = first & 0x0F; width = 3; }
        else if ((first & 0xF8) == 0xF0) { code = first & 0x07; width = 4; }
        else { free(decoded); return false; }
        if (index + width > length) { free(decoded); return false; }
        for (size_t next = 1; next < width; next++) {
            if ((text[index + next] & 0xC0) != 0x80) { free(decoded); return false; }
            code = (code << 6) | (text[index + next] & 0x3F);
        }
        if ((width == 2 && code < 0x80) || (width == 3 && code < 0x800) ||
                (width == 4 && code < 0x10000) || code > 0x10FFFF ||
                (code >= 0xD800 && code <= 0xDFFF)) {
            free(decoded);
            return false;
        }
        if (code < 0x80) decoded[written++] = (char)code;
        else if (code < 0x800) {
            decoded[written++] = (char)(0xC0 | (code >> 6));
            decoded[written++] = (char)(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            decoded[written++] = (char)(0xE0 | (code >> 12));
            decoded[written++] = (char)(0x80 | ((code >> 6) & 0x3F));
            decoded[written++] = (char)(0x80 | (code & 0x3F));
        } else {
            decoded[written++] = (char)(0xF0 | (code >> 18));
            decoded[written++] = (char)(0x80 | ((code >> 12) & 0x3F));
            decoded[written++] = (char)(0x80 | ((code >> 6) & 0x3F));
            decoded[written++] = (char)(0x80 | (code & 0x3F));
        }
        index += width;
    }
    decoded[written] = 0;
    *out = decoded;
    *out_length = written;
    return true;
}

static void watch_event(struct res_watch *watch, int status, const char *content,
        size_t content_length, const char *error, bool with_age) {
    struct res_buffer text = {0};
    rb_append(&text, "(:TYPE :WATCH :OWNER ", 21);
    rb_quoted_cstring(&text, watch->owner);
    rb_append(&text, " :SOURCE-ID ", 12);
    rb_printf(&text, "%llu", (unsigned long long)watch->source_id);
    rb_append(&text, " :NAME ", 7);
    res_append_keyword(&text, watch->name);
    rb_append(&text, " :PATH ", 7);
    rb_quoted_cstring(&text, watch->path);
    rb_append(&text, " :CONTENT ", 10);
    rb_quoted(&text, content ? content : "", content ? content_length : 0);
    rb_append(&text, " :STATUS ", 9);
    res_append_keyword(&text, watch_status_name(status));
    rb_append(&text, " :ERROR ", 8);
    if (error) rb_quoted_cstring(&text, error);
    else rb_append(&text, "NIL", 3);
    rb_append(&text, " :AGE ", 6);
    if (with_age) {
        struct stat status_buffer;
        if (watch->absolute && stat(watch->absolute, &status_buffer) == 0) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            rb_printf(&text, "%lld", (long long)((int64_t)now.tv_sec -
                (int64_t)status_buffer.st_mtime));
        } else {
            rb_append(&text, "NIL", 3);
        }
    } else {
        rb_append(&text, "NIL", 3);
    }
    rb_append(&text, ")", 1);
    char *message = rb_take(&text);
    if (message) {
        res_push_event(res.server, EV_WATCH, EVF_FACT, message, text.length);
        free(message);
    }
}

void res_watch_deliver(struct res_watch *watch, int status, const char *content,
        const char *error, bool with_age) {
    (void)with_age;
    watch->status = status;
    char *copy = error ? res_dup(error) : NULL;
    free(watch->error);
    watch->error = copy;
    watch_event(watch, status, content, content ? strlen(content) : 0, error, true);
}

static bool watch_read(struct res_watch *watch, char **content, int *status, char **error) {
    *content = NULL;
    *error = NULL;
    size_t capacity = watch->limit + 1;
    unsigned char *buffer = malloc(capacity);
    if (!buffer) {
        *status = WATCH_READ_ERROR;
        *error = res_dup("Cannot allocate file watch buffer.");
        return false;
    }
    long count = tomoe_watch_read(watch->native, buffer, watch->limit);
    if (count < 0) {
        int code = (int)-count;
        struct res_buffer message = {0};
        if (code == EFBIG) {
            rb_printf(&message, "Watch content exceeds the %zu-byte limit.", watch->limit);
        } else if (code == ENOENT || code == ENOTDIR) {
            free(buffer);
            *status = WATCH_MISSING;
            return false;
        } else {
            rb_printf(&message, "Watch read failed: errno %d.", code);
        }
        *status = WATCH_READ_ERROR;
        *error = rb_take(&message);
        free(buffer);
        return false;
    }
    if ((size_t)count > watch->limit) {
        struct res_buffer message = {0};
        rb_printf(&message, "Watch content exceeds the %zu-byte limit.", watch->limit);
        *status = WATCH_READ_ERROR;
        *error = rb_take(&message);
        free(buffer);
        return false;
    }
    char *decoded = NULL;
    size_t decoded_length = 0;
    if (!watch_utf8(buffer, (size_t)count, &decoded, &decoded_length)) {
        *status = WATCH_READ_ERROR;
        *error = res_dup("Watch content is not valid UTF-8.");
        free(buffer);
        return false;
    }
    free(buffer);
    *content = decoded;
    return true;
}

static void watch_close_native(struct res_watch *watch) {
    if (watch->source) { loop_remove(watch->source); watch->source = NULL; }
    if (watch->native) { tomoe_watch_close(watch->native); watch->native = NULL; }
}

static void watch_fd_callback(void *data, int fd, uint32_t mask);

static void watch_ensure_source(struct res_watch *watch) {
    if (!watch->native || watch->source) return;
    int descriptor = tomoe_watch_descriptor(watch->native);
    if (descriptor < 0) return;
    watch->source = loop_watch_fd(res.server, descriptor, LOOP_READABLE,
        watch_fd_callback, watch);
}

static bool watch_attach(struct res_watch *watch) {
    int error = 0;
    struct file_watch *native = tomoe_watch_open(watch->absolute ? watch->absolute : "", &error);
    if (!native) {
        struct res_buffer message = {0};
        rb_printf(&message, "Cannot open file watch %s: errno %d.",
                  watch->path ? watch->path : "", error);
        free(watch->error);
        watch->error = rb_take(&message);
        watch->status = WATCH_MISSING;
        watch->active = false;
        watch->retry = res_now() + WATCH_RETRY_MS;
        return false;
    }
    watch->native = native;
    watch->active = true;
    watch->retry = 0;
    watch_ensure_source(watch);
    return true;
}

static void watch_poll(struct res_watch *watch, uint64_t now) {
    if (!watch->native) {
        if (watch->retry && now < watch->retry) return;
        if (!watch_attach(watch)) return;
        char *content = NULL;
        int status = WATCH_RESTORED;
        char *error = NULL;
        bool read = watch_read(watch, &content, &status, &error);
        if (read) {
            res_watch_deliver(watch, WATCH_RESTORED, content, NULL, true);
            free(content);
        } else {
            res_watch_deliver(watch, status, "", error, true);
            free(error);
        }
        return;
    }
    int bits = tomoe_watch_poll(watch->native);
    if (bits < 0) {
        struct res_buffer message = {0};
        rb_printf(&message, "File watch poll failed: errno %d.", -bits);
        char *error = rb_take(&message);
        free(watch->error);
        watch->error = error;
        watch->retry = now + WATCH_RETRY_MS;
        if (watch->source) { loop_remove(watch->source); watch->source = NULL; }
        return;
    }
    if (bits == 0) {
        if (!watch->active) watch->retry = now + WATCH_RETRY_MS;
        return;
    }
    if (bits & 4) {
        watch->active = false;
        watch->retry = now + WATCH_RETRY_MS;
        if (watch->source) { loop_remove(watch->source); watch->source = NULL; }
        res_watch_deliver(watch, WATCH_UNAVAILABLE, "", NULL, true);
        return;
    }
    watch->active = true;
    watch->retry = 0;
    char *content = NULL;
    char *error = NULL;
    int status = WATCH_READY;
    if (!watch_read(watch, &content, &status, &error)) {
        res_watch_deliver(watch, status, "", error, true);
        free(error);
        return;
    }
    if (bits & 8) res_watch_deliver(watch, WATCH_RESTORED, content, NULL, true);
    else res_watch_deliver(watch, (bits & 2) ? WATCH_OVERFLOW : WATCH_CHANGED, content, NULL, true);
    free(content);
    watch_ensure_source(watch);
}

static void watch_fd_callback(void *data, int fd, uint32_t mask) {
    struct res_watch *watch = data;
    (void)fd;
    (void)mask;
    if (!res.started) return;
    uint64_t now = res_now();
    watch_poll(watch, now);
    watch_ensure_source(watch);
    res_engine_kick();
}

static char *watch_absolute_path(const struct res_watch *watch, const char *directory) {
    if (!watch->path) return res_dup("");
    if (watch->path[0] == '/') return res_dup(watch->path);
    if (!directory || !*directory) return res_dup(watch->path);
    size_t directory_length = strlen(directory);
    size_t path_length = strlen(watch->path);
    size_t separator = directory[directory_length - 1] == '/' ? 0 : 1;
    char *absolute = malloc(directory_length + separator + path_length + 1);
    if (!absolute) return NULL;
    memcpy(absolute, directory, directory_length);
    if (separator) absolute[directory_length] = '/';
    memcpy(absolute + directory_length + separator, watch->path, path_length + 1);
    return absolute;
}

void res_watches_apply(struct want_watch *wanted, size_t count) {
    bool *used = calloc(res.watch_count ? res.watch_count : 1, sizeof(bool));
    struct res_watch *watches = calloc(count ? count : 1, sizeof(struct res_watch));
    if (!used || !watches) {
        free(used);
        free(watches);
        for (size_t index = 0; index < count; index++) {
            free(wanted[index].owner);
            free(wanted[index].name);
            free(wanted[index].path);
            free(wanted[index].directory);
        }
        return;
    }
    for (size_t index = 0; index < count; index++) {
        ssize_t match = -1;
        for (size_t old = 0; old < res.watch_count; old++) {
            struct res_watch *existing = &res.watches[old];
            if (used[old]) continue;
            if (strcmp(existing->owner, wanted[index].owner) ||
                    strcmp(existing->name, wanted[index].name)) continue;
            if (wanted[index].path && existing->path &&
                    !strcmp(wanted[index].path, existing->path) &&
                    existing->limit == wanted[index].limit) {
                match = (ssize_t)old;
                break;
            }
        }
        free(wanted[index].directory);
        if (match >= 0) {
            watches[index] = res.watches[match];
            watches[index].source_id = wanted[index].source_id;
            used[match] = true;
            free(wanted[index].owner);
            free(wanted[index].name);
            free(wanted[index].path);
            continue;
        }
        watches[index].owner = wanted[index].owner;
        watches[index].name = wanted[index].name;
        watches[index].path = wanted[index].path;
        watches[index].limit = wanted[index].limit;
        watches[index].source_id = wanted[index].source_id;
        watches[index].native = NULL;
        watches[index].active = true;
        watches[index].status = WATCH_READY;
        watches[index].absolute = watch_absolute_path(&watches[index], wanted[index].directory);
        char *content = NULL;
        char *error = NULL;
        int status = WATCH_READY;
        if (!watch_attach(&watches[index])) {
            res_watch_deliver(&watches[index], WATCH_MISSING, "", watches[index].error, true);
        } else if (!watch_read(&watches[index], &content, &status, &error)) {
            res_watch_deliver(&watches[index], status, "", error, true);
            free(error);
        } else {
            res_watch_deliver(&watches[index], WATCH_READY, content, NULL, true);
            free(content);
        }
    }
    for (size_t old = 0; old < res.watch_count; old++) {
        if (used[old]) continue;
        watch_close_native(&res.watches[old]);
        free(res.watches[old].owner);
        free(res.watches[old].name);
        free(res.watches[old].path);
        free(res.watches[old].absolute);
        free(res.watches[old].error);
    }
    free(used);
    free(res.watches);
    res.watches = watches;
    res.watch_count = count;
}

void res_watches_service(uint64_t now, uint64_t *next) {
    for (size_t index = 0; index < res.watch_count; index++) {
        struct res_watch *watch = &res.watches[index];
        if (watch->retry && now >= watch->retry) watch_poll(watch, now);
        watch_ensure_source(watch);
        if (watch->retry && watch->retry < *next) *next = watch->retry;
    }
}

void res_watches_free(void) {
    for (size_t index = 0; index < res.watch_count; index++) {
        struct res_watch *watch = &res.watches[index];
        watch_close_native(watch);
        free(watch->owner);
        free(watch->name);
        free(watch->path);
        free(watch->absolute);
        free(watch->error);
    }
    free(res.watches);
    res.watches = NULL;
    res.watch_count = 0;
}
