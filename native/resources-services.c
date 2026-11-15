#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "resources-internal.h"
#include "../support/battery.h"
#include "../support/mpris.h"
#include "../support/network.h"
#include "../support/notifications.h"
#include "../support/tray.h"

#define SOURCE_RETRY_MS 1000
#define SOURCE_SCAN_MS 250

static const char *producer_default(int kind) {
    switch (kind) {
    case PRODUCER_NETWORK: return "(:connected nil :ssid nil :strength 0)";
    case PRODUCER_MPRIS:
        return "(:available nil :player-name \"\" :status \"\" :title \"\" :artist \"\""
               " :album \"\" :art-url \"\" :length 0 :position 0 :volume 1.0d0)";
    case PRODUCER_BATTERY: return "(:available nil :percent 100 :charging nil)";
    case PRODUCER_TRAY: return "(:items nil)";
    default: return "(:available nil :notifications nil)";
    }
}

static void services_text(struct res_buffer *text) {
    rb_append(text, "(:notifications ", 16);
    rb_append(text, res.producers[PRODUCER_NOTIFICATIONS].snapshot,
        strlen(res.producers[PRODUCER_NOTIFICATIONS].snapshot));
    rb_append(text, " :mpris ", 8);
    rb_append(text, res.producers[PRODUCER_MPRIS].snapshot,
        strlen(res.producers[PRODUCER_MPRIS].snapshot));
    rb_append(text, " :battery ", 10);
    rb_append(text, res.producers[PRODUCER_BATTERY].snapshot,
        strlen(res.producers[PRODUCER_BATTERY].snapshot));
    rb_append(text, " :network ", 10);
    rb_append(text, res.producers[PRODUCER_NETWORK].snapshot,
        strlen(res.producers[PRODUCER_NETWORK].snapshot));
    rb_append(text, " :tray ", 7);
    rb_append(text, res.producers[PRODUCER_TRAY].snapshot,
        strlen(res.producers[PRODUCER_TRAY].snapshot));
    rb_append(text, " :audio (:volume 1.0d0 :muted nil))", 35);
}

static void services_push(void) {
    if (!policy_reads(res.server, EV_SERVICES)) return;
    struct res_buffer text = {0};
    services_text(&text);
    char *message = rb_take(&text);
    if (!message) return;
    res_push_event(res.server, EV_SERVICES, 0, message, strlen(message));
    free(message);
}

static bool network_ssid(const char *snapshot, char **out) {
    const char *marker = strstr(snapshot, ":ssid ");
    if (!marker) { *out = res_dup(snapshot); return *out != NULL; }
    const char *list = marker + 6;
    while (*list == ' ') list++;
    if (*list != '(') { *out = res_dup(snapshot); return *out != NULL; }
    const char *cursor = list + 1;
    struct res_buffer ssid = {0};
    bool first = true;
    for (;;) {
        while (*cursor == ' ') cursor++;
        if (*cursor == ')') { cursor++; break; }
        char *end = NULL;
        long value = strtol(cursor, &end, 10);
        if (end == cursor) { rb_free(&ssid); return false; }
        if (value < 0 || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
            rb_free(&ssid);
            return false;
        }
        unsigned code = (unsigned)value;
        char encoded[4];
        size_t width = 0;
        if (code < 0x80) encoded[width++] = (char)code;
        else if (code < 0x800) {
            encoded[width++] = (char)(0xC0 | (code >> 6));
            encoded[width++] = (char)(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            encoded[width++] = (char)(0xE0 | (code >> 12));
            encoded[width++] = (char)(0x80 | ((code >> 6) & 0x3F));
            encoded[width++] = (char)(0x80 | (code & 0x3F));
        } else {
            encoded[width++] = (char)(0xF0 | (code >> 18));
            encoded[width++] = (char)(0x80 | ((code >> 12) & 0x3F));
            encoded[width++] = (char)(0x80 | ((code >> 6) & 0x3F));
            encoded[width++] = (char)(0x80 | (code & 0x3F));
        }
        rb_append(&ssid, encoded, width);
        first = false;
        cursor = end;
    }
    (void)first;
    struct res_buffer text = {0};
    rb_append(&text, snapshot, (size_t)(marker - snapshot));
    rb_append(&text, marker, 6);
    rb_quoted(&text, ssid.data ? ssid.data : "", ssid.length);
    rb_append(&text, cursor, strlen(cursor));
    rb_free(&ssid);
    *out = rb_take(&text);
    return *out != NULL;
}

static void *producer_open(struct res_producer *producer, int *error) {
    const char *root = NULL;
    switch (producer->kind) {
    case PRODUCER_NETWORK:
        root = getenv("TOMOE_NETWORK_SYSFS_ROOT");
        if (!root || !*root) root = "/sys/class/net";
        return tomoe_network_open(root, error);
    case PRODUCER_MPRIS:
        return tomoe_mpris_open(error);
    case PRODUCER_BATTERY:
        root = getenv("TOMOE_POWER_SUPPLY_ROOT");
        if (!root || !*root) root = "/sys/class/power_supply";
        return tomoe_battery_open(root, error);
    case PRODUCER_TRAY:
        return tomoe_tray_open(error);
    default:
        return tomoe_notifications_open(error);
    }
}

static int producer_poll(struct res_producer *producer) {
    switch (producer->kind) {
    case PRODUCER_NETWORK: return tomoe_network_poll(producer->native);
    case PRODUCER_MPRIS: return tomoe_mpris_poll(producer->native);
    case PRODUCER_BATTERY: return tomoe_battery_poll(producer->native);
    case PRODUCER_TRAY: return tomoe_tray_poll(producer->native);
    default: return tomoe_notifications_poll(producer->native);
    }
}

static const char *producer_snapshot(struct res_producer *producer) {
    switch (producer->kind) {
    case PRODUCER_NETWORK: return tomoe_network_snapshot(producer->native);
    case PRODUCER_MPRIS: return tomoe_mpris_snapshot(producer->native);
    case PRODUCER_BATTERY: return tomoe_battery_snapshot(producer->native);
    case PRODUCER_TRAY: return tomoe_tray_snapshot(producer->native);
    default: return tomoe_notifications_snapshot(producer->native);
    }
}

static int producer_fd(struct res_producer *producer) {
    switch (producer->kind) {
    case PRODUCER_NETWORK: return tomoe_network_fd(producer->native);
    case PRODUCER_MPRIS: return tomoe_mpris_fd(producer->native);
    case PRODUCER_BATTERY: return tomoe_battery_fd(producer->native);
    case PRODUCER_TRAY: return tomoe_tray_fd(producer->native);
    default: return tomoe_notifications_fd(producer->native);
    }
}

static unsigned producer_mask(struct res_producer *producer) {
    switch (producer->kind) {
    case PRODUCER_NETWORK: return tomoe_network_mask(producer->native);
    case PRODUCER_MPRIS: return tomoe_mpris_mask(producer->native);
    case PRODUCER_BATTERY: return tomoe_battery_mask(producer->native);
    case PRODUCER_TRAY: return tomoe_tray_mask(producer->native);
    default: return tomoe_notifications_mask(producer->native);
    }
}

static int producer_timeout(struct res_producer *producer) {
    switch (producer->kind) {
    case PRODUCER_NETWORK: return tomoe_network_timeout(producer->native, -1);
    case PRODUCER_MPRIS: return tomoe_mpris_timeout(producer->native, -1);
    case PRODUCER_BATTERY: return tomoe_battery_timeout(producer->native, -1);
    case PRODUCER_TRAY: return tomoe_tray_timeout(producer->native, -1);
    default: return tomoe_notifications_timeout(producer->native, -1);
    }
}

static void producer_close(struct res_producer *producer) {
    if (producer->source) { loop_remove(producer->source); producer->source = NULL; }
    if (!producer->native) return;
    switch (producer->kind) {
    case PRODUCER_NETWORK: tomoe_network_close(producer->native); break;
    case PRODUCER_MPRIS: tomoe_mpris_close(producer->native); break;
    case PRODUCER_BATTERY: tomoe_battery_close(producer->native); break;
    case PRODUCER_TRAY: tomoe_tray_close(producer->native); break;
    default: tomoe_notifications_close(producer->native); break;
    }
    producer->native = NULL;
    free(producer->snapshot);
    producer->snapshot = res_dup(producer_default(producer->kind));
}

static void producer_callback(void *data, int fd, uint32_t mask);

static void producer_refresh(struct res_producer *producer) {
    const char *snapshot = producer_snapshot(producer);
    if (!snapshot) return;
    char *decoded = NULL;
    if (producer->kind == PRODUCER_NETWORK) {
        if (!network_ssid(snapshot, &decoded)) {
            fprintf(stderr, "tomoe: network producer stopped: malformed SSID\n");
            producer_close(producer);
            return;
        }
    } else {
        decoded = res_dup(snapshot);
    }
    if (!decoded) return;
    free(producer->snapshot);
    producer->snapshot = decoded;
}

static void producer_update_fd(struct res_producer *producer) {
    if (!producer->native) return;
    int descriptor = producer_fd(producer);
    unsigned mask = producer_mask(producer);
    if (producer->source) {
        loop_update_fd(producer->source, mask);
        return;
    }
    if (descriptor < 0) return;
    producer->source = loop_watch_fd(res.server, descriptor, mask, producer_callback, producer);
}

static void producer_ready(struct res_producer *producer, uint64_t now) {
    int status = producer_poll(producer);
    if (status < 0) {
        fprintf(stderr, "tomoe: producer stopped: errno %d.\n", -status);
        producer_close(producer);
        services_push();
        return;
    }
    int timeout = producer_timeout(producer);
    producer->due = now + (timeout > 0 ? (uint64_t)timeout : 0);
    producer_update_fd(producer);
    if (status > 0) {
        producer_refresh(producer);
        services_push();
    }
}

static void producer_callback(void *data, int fd, uint32_t mask) {
    struct res_producer *producer = data;
    (void)fd;
    (void)mask;
    if (!res.started || !producer->native) return;
    uint64_t now = res_now();
    producer_ready(producer, now);
    res_engine_kick();
}

static void producer_start(struct res_producer *producer) {
    int error = 0;
    void *native = producer_open(producer, &error);
    if (!native) {
        if (error)
            fprintf(stderr, "tomoe: %s unavailable: errno %d.\n", producer->key, error);
        return;
    }
    producer->native = native;
    producer_refresh(producer);
    producer->due = 0;
    int descriptor = producer_fd(producer);
    if (descriptor >= 0)
        producer->source = loop_watch_fd(res.server, descriptor, producer_mask(producer),
            producer_callback, producer);
}

bool res_system_read(void) {
    return res.server && policy_reads(res.server, EV_SYSTEM);
}

static bool read_first_line(const char *path, char *buffer, size_t size) {
    FILE *file = fopen(path, "r");
    if (!file) return false;
    bool found = fgets(buffer, (int)size, file) != NULL;
    fclose(file);
    if (found) {
        size_t length = strlen(buffer);
        while (length && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r'))
            buffer[--length] = 0;
    }
    return found;
}

static bool read_integer(const char *path, int64_t *out) {
    char line[256];
    if (!read_first_line(path, line, sizeof(line))) return false;
    char *end = NULL;
    long long value = strtoll(line, &end, 10);
    if (end == line) return false;
    *out = value;
    return true;
}

static int compare_names(const void *first, const void *second) {
    return strcmp(*(const char *const *)first, *(const char *const *)second);
}

static char **list_directory(const char *path, bool directories, size_t *count) {
    *count = 0;
    DIR *directory = opendir(path);
    if (!directory) return NULL;
    size_t capacity = 16, length = 0;
    char **names = malloc(capacity * sizeof(char *));
    struct dirent *entry = NULL;
    while (names && (entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat status;
        char full[8192];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        if (directories && (stat(full, &status) < 0 || !S_ISDIR(status.st_mode))) continue;
        if (length == capacity) {
            capacity *= 2;
            char **grown = realloc(names, capacity * sizeof(char *));
            if (!grown) break;
            names = grown;
        }
        names[length] = res_dup(entry->d_name);
        if (!names[length]) break;
        length++;
    }
    closedir(directory);
    if (!names) return NULL;
    qsort(names, length, sizeof(char *), compare_names);
    *count = length;
    return names;
}

static void free_names(char **names, size_t count) {
    for (size_t index = 0; index < count; index++) free(names[index]);
    free(names);
}

static bool all_digits(const char *text) {
    if (!*text) return false;
    for (const char *cursor = text; *cursor; cursor++)
        if (!isdigit((unsigned char)*cursor)) return false;
    return true;
}

static void append_gpus(struct res_buffer *text) {
    rb_append(text, "(", 1);
    bool first = true;
    if (access("/proc/driver/nvidia/version", F_OK) == 0) {
        unsigned devices = tomoe_nvml_count();
        for (unsigned index = 0; index < devices; index++) {
            unsigned busy = 0, used = 0, total = 0, celsius = 0;
            if (tomoe_nvml_sample(index, &busy, &used, &total, &celsius) != 1) continue;
            if (!first) rb_append(text, " ", 1);
            first = false;
            struct res_buffer name = {0};
            rb_printf(&name, "nvidia%u", index);
            rb_append(text, "(:name ", 7);
            rb_quoted_cstring(text, name.data ? name.data : "");
            rb_free(&name);
            rb_printf(text, " :busy %u :vram-used %u :vram-total %u :temperature %u)",
                      busy, used, total, celsius);
        }
    }
    size_t count = 0;
    char **names = list_directory("/sys/class/drm", true, &count);
    for (size_t index = 0; index < count; index++) {
        const char *name = names[index];
        if (strncmp(name, "card", 4) || !all_digits(name + 4)) continue;
        char device[4096];
        snprintf(device, sizeof(device), "/sys/class/drm/%s/device", name);
        char path[8192];
        int64_t busy = 0;
        snprintf(path, sizeof(path), "%s/gpu_busy_percent", device);
        if (!read_integer(path, &busy)) continue;
        int64_t used = 0, total = 0, celsius = 0;
        bool have_celsius = false;
        snprintf(path, sizeof(path), "%s/mem_info_vram_used", device);
        if (!read_integer(path, &used)) used = 0;
        snprintf(path, sizeof(path), "%s/mem_info_vram_total", device);
        if (!read_integer(path, &total)) total = 0;
        char probe[8192];
        snprintf(probe, sizeof(probe), "%s/hwmon", device);
        size_t sensors = 0;
        char **sensor_names = list_directory(probe, true, &sensors);
        for (size_t sensor = 0; sensor < sensors; sensor++) {
            char input[16384];
            snprintf(input, sizeof(input), "%s/hwmon/%s/temp1_input", device,
                     sensor_names[sensor]);
            int64_t value = 0;
            if (read_integer(input, &value)) { celsius = value / 1000; have_celsius = true; break; }
        }
        free_names(sensor_names, sensors);
        if (!first) rb_append(text, " ", 1);
        first = false;
        rb_append(text, "(:name ", 7);
        rb_quoted_cstring(text, name);
        rb_printf(text, " :busy %lld :vram-used %lld :vram-total %lld :temperature ",
                  (long long)busy, (long long)(used / 1048576), (long long)(total / 1048576));
        if (have_celsius) rb_printf(text, "%lld)", (long long)celsius);
        else rb_append(text, "nil)", 4);
    }
    free_names(names, count);
    rb_append(text, ")", 1);
}

static void append_cpu_temperature(struct res_buffer *text) {
    size_t count = 0;
    char **names = list_directory("/sys/class/hwmon", true, &count);
    for (size_t index = 0; index < count; index++) {
        char path[8192], name[256];
        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", names[index]);
        if (!read_first_line(path, name, sizeof(name))) continue;
        const char *want = NULL;
        if (!strcmp(name, "k10temp")) want = "Tctl";
        else if (!strcmp(name, "coretemp")) want = "Package id 0";
        else if (!strcmp(name, "zenpower")) want = "Tdie";
        if (!want) continue;
        char directory[4096];
        snprintf(directory, sizeof(directory), "/sys/class/hwmon/%s", names[index]);
        size_t labels = 0;
        char **files = list_directory(directory, false, &labels);
        int64_t milli = 0;
        bool found = false;
        for (size_t file = 0; file < labels && !found; file++) {
            size_t length = strlen(files[file]);
            if (length < 11 || strncmp(files[file], "temp", 4) ||
                    strcmp(files[file] + length - 6, "_label")) continue;
            char label_path[8192], label[256];
            snprintf(label_path, sizeof(label_path), "%s/%s", directory, files[file]);
            if (!read_first_line(label_path, label, sizeof(label))) continue;
            if (strcmp(label, want)) continue;
            char input[16384];
            snprintf(input, sizeof(input), "%s/%.*sinput", directory,
                     (int)(length - 5), files[file]);
            if (read_integer(input, &milli)) found = true;
        }
        free_names(files, labels);
        if (!found) {
            char input[16384];
            snprintf(input, sizeof(input), "%s/temp1_input", directory);
            found = read_integer(input, &milli);
        }
        if (found) {
            rb_printf(text, "%lld", (long long)(milli / 1000));
            free_names(names, count);
            return;
        }
    }
    free_names(names, count);
    rb_append(text, "nil", 3);
}

static char *sample_system(void) {
    struct res_buffer text = {0};
    int64_t total = 0, idle = 0;
    char line[8192];
    if (read_first_line("/proc/stat", line, sizeof(line))) {
        int64_t values[10];
        size_t count = 0;
        char *cursor = line;
        while (count < 10) {
            while (*cursor && !isdigit((unsigned char)*cursor) && *cursor != '-') cursor++;
            if (!*cursor) break;
            char *end = NULL;
            values[count++] = strtoll(cursor, &end, 10);
            cursor = end;
        }
        for (size_t index = 0; index < count; index++) total += values[index];
        if (count > 3) idle += values[3];
        if (count > 4) idle += values[4];
    }
    int64_t memory_total = 0, memory_available = 0;
    FILE *memory = fopen("/proc/meminfo", "r");
    if (memory) {
        while (fgets(line, sizeof(line), memory)) {
            long long value = 0;
            if (!strncmp(line, "MemTotal:", 9) && sscanf(line + 9, "%lld", &value) == 1)
                memory_total = value;
            else if (!strncmp(line, "MemAvailable:", 13) &&
                    sscanf(line + 13, "%lld", &value) == 1)
                memory_available = value;
        }
        fclose(memory);
    }
    rb_printf(&text, "(:cpu-total %lld :cpu-idle %lld :cpu-temperature ",
              (long long)total, (long long)idle);
    append_cpu_temperature(&text);
    rb_printf(&text, " :memory-total %lld :memory-available %lld :gpus ",
              (long long)memory_total, (long long)memory_available);
    append_gpus(&text);
    rb_append(&text, ")", 1);
    return rb_take(&text);
}

static void system_sample(uint64_t now) {
    char *text = sample_system();
    res.system_due = now + 1000;
    if (!text) return;
    free(res.system_text);
    res.system_text = text;
    struct res_buffer event = {0};
    rb_append(&event, "(:TYPE :SYSTEM :SYSTEM ", 23);
    rb_append(&event, text, strlen(text));
    rb_append(&event, ")", 1);
    char *message = rb_take(&event);
    if (message) {
        res_push_event(res.server, EV_SYSTEM, 0, message, strlen(message));
        free(message);
    }
}

lisp_value res_services_value(void) {
    struct res_buffer text = {0};
    services_text(&text);
    char *message = rb_take(&text);
    if (!message) return lisp_nil();
    lisp_value value = resources_read_text(message, strlen(message));
    free(message);
    return value;
}

lisp_value res_system_value(void) {
    if (!res.system_text) {
        char *text = sample_system();
        if (!text) return lisp_nil();
        res.system_text = text;
    }
    return resources_read_text(res.system_text, strlen(res.system_text));
}

bool res_media(const char *method, char *error, size_t size) {
    struct res_producer *producer = &res.producers[PRODUCER_MPRIS];
    if (!producer->native) return true;
    if (tomoe_mpris_control(producer->native, method) >= 0) return true;
    const char *action = !strcmp(method, "PlayPause") ? "play-pause"
        : (!strcmp(method, "Next") ? "next" : "previous");
    res_error_text(error, size, "Cannot send %s to the media player.", action);
    return false;
}

static int64_t backlight_value(const char *path, int64_t fallback) {
    int64_t value = 0;
    if (!read_integer(path, &value)) return fallback;
    return value;
}

bool res_backlight(int64_t percent, char *error, size_t size) {
    const char *root = getenv("TOMOE_BACKLIGHT_ROOT");
    if (!root || !*root) root = "/sys/class/backlight";
    size_t count = 0;
    char **names = list_directory(root, true, &count);
    if (!names || !count) {
        free_names(names, count);
        res_error_text(error, size, "No backlight under %s.", root);
        return false;
    }
    char maximum_path[8192], value_path[8192];
    snprintf(maximum_path, sizeof(maximum_path), "%s/%s/max_brightness", root, names[0]);
    snprintf(value_path, sizeof(value_path), "%s/%s/brightness", root, names[0]);
    int64_t maximum = backlight_value(maximum_path, -1);
    int64_t current = backlight_value(value_path, -1);
    if (maximum < 0 || current < 0) {
        res_error_text(error, size, "Cannot read backlight %s.", names[0]);
        free_names(names, count);
        return false;
    }
    double step = nearbyint((double)maximum * (double)percent / 100.0);
    int64_t value = current + (int64_t)step;
    if (value < 0) value = 0;
    if (value > maximum) value = maximum;
    char device[256];
    snprintf(device, sizeof(device), "%s", names[0]);
    int status = tomoe_backlight_set(device, (unsigned)value);
    free_names(names, count);
    if (status) {
        res_error_text(error, size, "logind did not set %s to %lld: errno %d.",
                       device, (long long)value, -status);
        return false;
    }
    return true;
}

void res_services_start(void) {
    static const char *keys[] = { "network", "mpris", "battery", "tray", "notifications" };
    for (int kind = 0; kind < 5; kind++) {
        struct res_producer *producer = &res.producers[kind];
        producer->key = keys[kind];
        producer->kind = kind;
        producer->snapshot = res_dup(producer_default(kind));
    }
    for (int kind = 0; kind < 5; kind++) producer_start(&res.producers[kind]);
    res.system_due = res_now();
}

void res_services_stop(void) {
    for (int kind = 0; kind < 5; kind++) producer_close(&res.producers[kind]);
    for (int kind = 0; kind < 5; kind++) {
        free(res.producers[kind].snapshot);
        res.producers[kind].snapshot = NULL;
    }
    free(res.system_text);
    res.system_text = NULL;
}

void res_services_service(uint64_t now, uint64_t *next) {
    for (int kind = 0; kind < 5; kind++) {
        struct res_producer *producer = &res.producers[kind];
        if (!producer->native) continue;
        if (now >= producer->due) {
            producer_ready(producer, now);
            if (!producer->native) continue;
        }
        if (producer->due < *next) *next = producer->due;
    }
    if (res_system_read()) {
        if (now >= res.system_due) system_sample(now);
        if (res.system_due < *next) *next = res.system_due;
    }
}

static uint64_t fnv1a(const unsigned char *data, size_t length) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t index = 0; index < length; index++)
        hash = (hash ^ data[index]) * 1099511628211ULL;
    return hash;
}

static bool source_stamp(const char *path, uint64_t stamp[3]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat status;
    if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode)) { close(fd); return false; }
    size_t limit = status.st_size > 1048576 ? 1048576 : (size_t)status.st_size;
    unsigned char *buffer = malloc(limit ? limit : 1);
    if (!buffer) { close(fd); return false; }
    size_t total = 0;
    while (total < limit) {
        ssize_t count = read(fd, buffer + total, limit - total);
        if (count < 0) { if (errno == EINTR) continue; break; }
        if (!count) break;
        total += (size_t)count;
    }
    close(fd);
    stamp[0] = (uint64_t)status.st_mtime;
    stamp[1] = (uint64_t)status.st_size;
    stamp[2] = fnv1a(buffer, total);
    free(buffer);
    return true;
}

static void sources_scan(uint64_t now, uint64_t *next);

static void source_callback(void *data, int fd, uint32_t mask) {
    (void)data;
    (void)fd;
    (void)mask;
    if (!res.started) return;
    int bits = 0;
    for (size_t index = 0; index < res.source_count; index++) {
        struct res_source *source = &res.sources[index];
        if (!source->native) continue;
        bits |= tomoe_watch_poll(source->native);
    }
    (void)bits;
    res.source_scanning = true;
    res.source_scans = 2;
    res.source_due = res_now() + SOURCE_SCAN_MS;
    res_engine_kick();
}

static void sources_watch(struct res_source *source) {
    if (source->source) { loop_remove(source->source); source->source = NULL; }
    if (source->native) { tomoe_watch_close(source->native); source->native = NULL; }
    int error = 0;
    source->native = tomoe_watch_open(source->path, &error);
    if (!source->native) return;
    int descriptor = tomoe_watch_descriptor(source->native);
    if (descriptor >= 0)
        source->source = loop_watch_fd(res.server, descriptor, LOOP_READABLE,
            source_callback, source);
}

bool res_sources_set(lisp_value paths, char *error, size_t size) {
    size_t count = 0;
    for (lisp_value cursor = paths; lisp_consp(cursor); cursor = lisp_cdr(cursor)) count++;
    struct res_source *sources = calloc(count ? count : 1, sizeof(*sources));
    if (!sources) { res_error_text(error, size, "Cannot allocate resources."); return false; }
    size_t length = 0;
    for (lisp_value cursor = paths; lisp_consp(cursor); cursor = lisp_cdr(cursor)) {
        size_t text_length = 0;
        const char *text = lisp_stringp(lisp_car(cursor))
            ? lisp_get_string(lisp_car(cursor), &text_length) : NULL;
        if (!text || text[0] != '/') {
            for (size_t index = 0; index < length; index++) free(sources[index].path);
            free(sources);
            res_error_text(error, size, "Extension sources must be absolute paths.");
            return false;
        }
        char *path = res_dup_span(text, text_length);
        struct res_source *source = &sources[length++];
        for (size_t old = 0; old < res.source_count; old++) {
            if (res.sources[old].path && !strcmp(res.sources[old].path, path)) {
                *source = res.sources[old];
                memset(&res.sources[old], 0, sizeof(res.sources[old]));
                break;
            }
        }
        if (!source->path) {
            source->path = path;
            source->have_loaded = source_stamp(source->path, source->loaded);
            sources_watch(source);
        } else {
            free(path);
        }
    }
    for (size_t index = 0; index < res.source_count; index++) {
        if (res.sources[index].source) loop_remove(res.sources[index].source);
        if (res.sources[index].native) tomoe_watch_close(res.sources[index].native);
        free(res.sources[index].path);
    }
    free(res.sources);
    res.sources = sources;
    res.source_count = length;
    res.source_scanning = false;
    res.source_scans = 0;
    res.source_due = 0;
    res_engine_kick();
    return true;
}

static void sources_scan(uint64_t now, uint64_t *next) {
    bool missing = false;
    for (size_t index = 0; index < res.source_count; index++) {
        struct res_source *source = &res.sources[index];
        if (!source->native) {
            sources_watch(source);
            if (!source->native) { missing = true; continue; }
        }
        uint64_t stamp[3];
        if (!source_stamp(source->path, stamp)) continue;
        if (!source->have_loaded) {
            source->loaded[0] = stamp[0]; source->loaded[1] = stamp[1];
            source->loaded[2] = stamp[2];
            source->have_loaded = true;
        }
        bool same_loaded = stamp[0] == source->loaded[0] && stamp[1] == source->loaded[1] &&
                           stamp[2] == source->loaded[2];
        if (same_loaded) {
            source->stable = 0;
            source->have_sample = false;
            continue;
        }
        if (source->have_sample && stamp[0] == source->sample[0] &&
                stamp[1] == source->sample[1] && stamp[2] == source->sample[2]) {
            source->stable++;
        } else {
            source->stable = 1;
        }
        source->sample[0] = stamp[0]; source->sample[1] = stamp[1]; source->sample[2] = stamp[2];
        source->have_sample = true;
        if (source->stable >= 2) {
            source->loaded[0] = stamp[0]; source->loaded[1] = stamp[1];
            source->loaded[2] = stamp[2];
            source->stable = 0;
            source->have_sample = false;
            struct res_buffer text = {0};
            rb_append(&text, "(:TYPE :SOURCE :PATH ", 21);
            rb_quoted_cstring(&text, source->path);
            rb_append(&text, ")", 1);
            char *message = rb_take(&text);
            if (message) {
                res_push_event(res.server, EV_SOURCE, EVF_FACT, message, strlen(message));
                free(message);
            }
        }
    }
    if (missing && now + SOURCE_RETRY_MS < *next) *next = now + SOURCE_RETRY_MS;
}

void res_sources_service(uint64_t now, uint64_t *next) {
    if (res.source_scanning) {
        if (now >= res.source_due) {
            sources_scan(now, next);
            if (--res.source_scans > 0) res.source_due = now + SOURCE_SCAN_MS;
            else res.source_scanning = false;
        }
        if (res.source_scanning && res.source_due < *next) *next = res.source_due;
        return;
    }
    bool missing = false;
    for (size_t index = 0; index < res.source_count; index++)
        if (!res.sources[index].native) missing = true;
    if (missing) {
        if (!res.source_due || now >= res.source_due) res.source_due = now + SOURCE_RETRY_MS;
        if (res.source_due < *next) *next = res.source_due;
    }
}

void res_sources_free(void) {
    for (size_t index = 0; index < res.source_count; index++) {
        if (res.sources[index].source) loop_remove(res.sources[index].source);
        if (res.sources[index].native) tomoe_watch_close(res.sources[index].native);
        free(res.sources[index].path);
    }
    free(res.sources);
    res.sources = NULL;
    res.source_count = 0;
}
