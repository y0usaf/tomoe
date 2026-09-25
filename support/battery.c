#define _GNU_SOURCE

#include "battery.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#define BATTERY_UPOWER "org.freedesktop.UPower"
#define BATTERY_PATH "/org/freedesktop/UPower/devices/DisplayDevice"
#define BATTERY_IFACE "org.freedesktop.UPower.Device"
#define BATTERY_PROPERTIES "org.freedesktop.DBus.Properties"
#define BATTERY_DBUS "org.freedesktop.DBus"
#define BATTERY_DBUS_PATH "/org/freedesktop/DBus"
#define BATTERY_DEFAULT_ROOT "/sys/class/power_supply"
#define BATTERY_SYSTEM_ADDRESS "unix:path=/run/dbus/system_bus_socket"

#define BATTERY_SETUP_USEC 2000000ULL
#define BATTERY_REQUEST_USEC 2000000ULL
#define BATTERY_SYSFS_INTERVAL_USEC 30000000ULL
#define BATTERY_POLL_MESSAGES 64U
#define BATTERY_POLL_BUDGET_USEC 4000ULL
#define BATTERY_MAX_TEXT 256U
#define BATTERY_MAX_DICT 32U
#define BATTERY_MAX_DIRS 256U
#define BATTERY_MAX_ROOT 4096U
#define BATTERY_MAX_PENDING 32U

enum battery_call_kind {
    BATTERY_CALL_OWNER,
    BATTERY_CALL_GET_ALL,
};

struct battery_values {
    int present;
    int present_value;
    int percentage;
    double percentage_value;
    int state;
    uint32_t state_value;
};

struct battery_invalidated {
    int present;
    int percentage;
    int state;
};

struct battery_call {
    enum battery_call_kind kind;
    struct tomoe_battery *battery;
    struct battery_call *next;
    sd_bus_slot *slot;
    uint64_t owner_epoch;
    uint64_t present_epoch;
    uint64_t percentage_epoch;
    uint64_t state_epoch;
    uint64_t request_serial;
    uint64_t deadline_usec;
};

struct text_buffer {
    char *data;
    size_t length;
    size_t capacity;
};

struct tomoe_battery {
    sd_bus *bus;
    sd_bus_slot *matches[2];
    struct battery_call *calls;
    size_t pending_count;
    char sysfs_root[BATTERY_MAX_ROOT + 1U];
    char sysfs_selected[BATTERY_MAX_ROOT + 1U];
    int sysfs_active;
    uint64_t sysfs_next_usec;
    int bus_disabled;
    int upower_authoritative;
    char owner[256];
    uint64_t owner_epoch;
    uint64_t present_epoch;
    uint64_t percentage_epoch;
    uint64_t state_epoch;
    uint64_t next_request_serial;
    uint64_t applied_present_serial;
    uint64_t applied_percentage_serial;
    uint64_t applied_state_serial;
    int need_owner;
    int need_get_all;
    struct battery_values raw;
    struct battery_values sysfs_raw;
    int available;
    uint8_t percent;
    int charging;
    uint64_t revision;
    int changed;
    char *snapshot;
};

static void cancel_stale_calls(struct tomoe_battery *battery);

int tomoe_battery_abi(void) { return 1; }

static uint64_t monotonic_usec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    if ((uint64_t)ts.tv_sec > UINT64_MAX / 1000000ULL) return UINT64_MAX;
    uint64_t seconds = (uint64_t)ts.tv_sec * 1000000ULL;
    uint64_t micros = (uint64_t)ts.tv_nsec / 1000ULL;
    return seconds > UINT64_MAX - micros ? UINT64_MAX : seconds + micros;
}

static int read_attribute(int directory, const char *name, char *output,
                          size_t capacity) {
    int fd = openat(directory, name,
                    O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return -errno;
    struct stat status;
    if (fstat(fd, &status) < 0) {
        int result = -errno;
        close(fd);
        return result;
    }
    if (!S_ISREG(status.st_mode)) {
        close(fd);
        return -EINVAL;
    }
    ssize_t length = read(fd, output, capacity - 1U);
    int saved = errno;
    if (length < 0) {
        close(fd);
        return -saved;
    }
    if (memchr(output, '\0', (size_t)length)) {
        close(fd);
        return -EBADMSG;
    }
    output[length] = '\0';
    if ((size_t)length == capacity - 1U) {
        char extra;
        ssize_t more = read(fd, &extra, 1);
        saved = errno;
        if (more > 0) {
            close(fd);
            return -E2BIG;
        }
        if (more < 0) {
            close(fd);
            return -saved;
        }
    }
    close(fd);
    return 0;
}

static uint64_t deadline_after(uint64_t now, uint64_t duration) {
    return now > UINT64_MAX - duration ? UINT64_MAX : now + duration;
}

static int bounded_string(const char *value, size_t limit) {
    return value && strnlen(value, limit + 1U) <= limit;
}

static int valid_unique_name(const char *value) {
    return bounded_string(value, 255U) && value[0] == ':';
}

static void public_default(struct tomoe_battery *battery) {
    battery->available = 0;
    battery->percent = 100U;
    battery->charging = 0;
}

static void public_from_raw(struct tomoe_battery *battery) {
    const struct battery_values *values = battery->upower_authoritative
        ? &battery->raw : &battery->sysfs_raw;
    if (!values->present) {
        public_default(battery);
        return;
    }
    battery->available = 1;
    double percentage = values->percentage ? values->percentage_value : 100.0;
    if (!isfinite(percentage)) percentage = 100.0;
    if (percentage < 0.0) percentage = 0.0;
    if (percentage > 100.0) percentage = 100.0;
    battery->percent = (uint8_t)(percentage + 0.5);
    battery->charging = values->state && values->state_value == 1U;
}

static void revision_bump(struct tomoe_battery *battery) {
    battery->revision++;
    if (!battery->revision) battery->revision = 1;
    battery->changed = 1;
}

static void refresh_public(struct tomoe_battery *battery) {
    int old_available = battery->available;
    uint8_t old_percent = battery->percent;
    int old_charging = battery->charging;
    public_from_raw(battery);
    if (old_available != battery->available ||
        old_percent != battery->percent ||
        old_charging != battery->charging)
        revision_bump(battery);
}

static int text_buffer_reserve(struct text_buffer *buffer, size_t extra) {
    if (extra > SIZE_MAX - buffer->length - 1U) return -E2BIG;
    size_t required = buffer->length + extra + 1U;
    if (required <= buffer->capacity) return 0;
    size_t capacity = buffer->capacity ? buffer->capacity : 128U;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }
    char *data = realloc(buffer->data, capacity);
    if (!data) return -ENOMEM;
    buffer->data = data;
    buffer->capacity = capacity;
    return 0;
}

static int text_buffer_append(struct text_buffer *buffer, const char *data,
                              size_t length) {
    int result = text_buffer_reserve(buffer, length);
    if (result < 0) return result;
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 0;
}

static int text_buffer_literal(struct text_buffer *buffer, const char *text) {
    return text_buffer_append(buffer, text, strlen(text));
}

static int text_buffer_uint(struct text_buffer *buffer, unsigned value) {
    char text[16];
    int length = snprintf(text, sizeof(text), "%u", value);
    if (length < 0 || (size_t)length >= sizeof(text)) return -EOVERFLOW;
    return text_buffer_append(buffer, text, (size_t)length);
}

static int snapshot_build(struct tomoe_battery *battery, char **output) {
    struct text_buffer buffer = {0};
    int result = text_buffer_literal(&buffer, "(:available ");
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, battery->available ? "t" : "nil");
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, " :percent ");
    if (result < 0) goto failed;
    result = text_buffer_uint(&buffer, battery->percent);
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, " :charging ");
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, battery->charging ? "t" : "nil");
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, ")");
    if (result < 0) goto failed;
    *output = buffer.data;
    return 0;
failed:
    free(buffer.data);
    return result;
}

const char *tomoe_battery_snapshot(struct tomoe_battery *battery) {
    if (!battery) return NULL;
    char *snapshot = NULL;
    if (snapshot_build(battery, &snapshot) < 0) return NULL;
    free(battery->snapshot);
    battery->snapshot = snapshot;
    return snapshot;
}

static const char *trimmed_start(const char *text, const char **end_out) {
    while (*text == ' ' || *text == '\t' || *text == '\n' ||
           *text == '\r' || *text == '\f' || *text == '\v')
        text++;
    const char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\n' || end[-1] == '\r' ||
                          end[-1] == '\f' || end[-1] == '\v'))
        end--;
    *end_out = end;
    return text;
}

static int text_equals(const char *text, const char *expected) {
    const char *end = NULL;
    const char *start = trimmed_start(text, &end);
    size_t length = (size_t)(end - start);
    return length == strlen(expected) && !memcmp(start, expected, length);
}

static uint8_t parse_capacity(const char *text) {
    const char *end = NULL;
    const char *start = trimmed_start(text, &end);
    if (start == end || *start == '-') return 100U;
    char *after = NULL;
    errno = 0;
    unsigned long value = strtoul(start, &after, 10);
    if (errno || after != end || value > 255UL) return 100U;
    return value > 100UL ? 100U : (uint8_t)value;
}

static int battery_name_compare(const void *left, const void *right) {
    const char *const *one = left;
    const char *const *two = right;
    return strcmp(*one, *two);
}

static int find_battery_path(struct tomoe_battery *battery,
                             char *path, size_t capacity) {
    int root = open(battery->sysfs_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                    O_NONBLOCK);
    if (root < 0) return 0;
    int scan_fd = dup(root);
    DIR *directory = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!directory) {
        if (scan_fd >= 0) close(scan_fd);
        close(root);
        return 0;
    }
    char *names[BATTERY_MAX_DIRS] = {0};
    size_t count = 0;
    size_t visited = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (++visited > BATTERY_MAX_DIRS) break;
        if (!bounded_string(entry->d_name, 255U)) continue;
        if (count >= BATTERY_MAX_DIRS) continue;
        names[count] = strdup(entry->d_name);
        if (names[count]) count++;
    }
    closedir(directory);
    qsort(names, count, sizeof(names[0]), battery_name_compare);
    int found = 0;
    for (size_t index = 0; index < count; index++) {
        if (!names[index]) continue;
        int device = openat(root, names[index],
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK);
        if (device < 0) continue;
        struct stat status;
        char type[BATTERY_MAX_TEXT + 1U] = {0};
        int valid = fstat(device, &status) == 0 && S_ISDIR(status.st_mode) &&
                    read_attribute(device, "type", type, sizeof(type)) >= 0 &&
                    text_equals(type, "Battery");
        close(device);
        if (!valid) continue;
        int length = snprintf(path, capacity, "%s/%s",
                              battery->sysfs_root, names[index]);
        if (length >= 0 && (size_t)length < capacity) found = 1;
        break;
    }
    for (size_t index = 0; index < count; index++) free(names[index]);
    close(root);
    return found;
}

static int read_sysfs_sample(struct tomoe_battery *battery) {
    char path[BATTERY_MAX_ROOT + 1U] = {0};
    if (!find_battery_path(battery, path, sizeof(path))) return 0;
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                         O_NONBLOCK);
    if (directory < 0) return -EAGAIN;
    struct stat pinned, current;
    if (fstat(directory, &pinned) < 0) {
        close(directory);
        return -EAGAIN;
    }
    char type[BATTERY_MAX_TEXT + 1U] = {0};
    char capacity[BATTERY_MAX_TEXT + 1U] = {0};
    char status[BATTERY_MAX_TEXT + 1U] = {0};
    int type_result = read_attribute(directory, "type", type, sizeof(type));
    if (type_result < 0 || !text_equals(type, "Battery")) {
        close(directory);
        return -EAGAIN;
    }
    int capacity_result = read_attribute(directory, "capacity", capacity,
                                          sizeof(capacity));
    int status_result = read_attribute(directory, "status", status,
                                       sizeof(status));
    int same = stat(path, &current) == 0 && pinned.st_dev == current.st_dev &&
               pinned.st_ino == current.st_ino;
    close(directory);
    if (!same) return -EAGAIN;
    memcpy(battery->sysfs_selected, path, strlen(path) + 1U);
    battery->sysfs_active = 1;
    memset(&battery->sysfs_raw, 0, sizeof(battery->sysfs_raw));
    battery->sysfs_raw.present = 1;
    battery->sysfs_raw.percentage = capacity_result >= 0;
    battery->sysfs_raw.percentage_value = capacity_result >= 0
        ? parse_capacity(capacity) : 100.0;
    battery->sysfs_raw.state = status_result >= 0;
    battery->sysfs_raw.state_value =
        status_result >= 0 && text_equals(status, "Charging") ? 1U : 0U;
    return 1;
}

static void read_sysfs(struct tomoe_battery *battery) {
    int result = read_sysfs_sample(battery);
    if (result == -EAGAIN) result = read_sysfs_sample(battery);
    if (result <= 0) {
        battery->sysfs_selected[0] = '\0';
        battery->sysfs_active = 0;
        battery->sysfs_next_usec = 0;
        memset(&battery->sysfs_raw, 0, sizeof(battery->sysfs_raw));
    }
    refresh_public(battery);
}

static void start_sysfs(struct tomoe_battery *battery) {
    battery->upower_authoritative = 0;
    read_sysfs(battery);
    if (battery->sysfs_active)
        battery->sysfs_next_usec =
            deadline_after(monotonic_usec(), BATTERY_SYSFS_INTERVAL_USEC);
}

static int read_property_dict(sd_bus_message *message,
                              struct battery_values *values) {
    int result = sd_bus_message_enter_container(message, 'a', "{sv}");
    if (result < 0) return result;
    if (!result) return -EBADMSG;
    size_t entries = 0;
    for (;;) {
        result = sd_bus_message_enter_container(message, 'e', "sv");
        if (result < 0) return result;
        if (!result) break;
        if (++entries > BATTERY_MAX_DICT) return -E2BIG;
        const char *key = NULL;
        result = sd_bus_message_read(message, "s", &key);
        if (result < 0 || !bounded_string(key, BATTERY_MAX_TEXT))
            return -EBADMSG;
        char type = 0;
        const char *contents = NULL;
        result = sd_bus_message_peek_type(message, &type, &contents);
        if (result <= 0 || type != 'v' || !contents)
            return result < 0 ? result : -EBADMSG;
        result = sd_bus_message_enter_container(message, 'v', contents);
        if (result < 0) return result;
        char value_type = 0;
        const char *value_contents = NULL;
        result = sd_bus_message_peek_type(message, &value_type, &value_contents);
        if (result <= 0) return result < 0 ? result : -EBADMSG;
        if (!strcmp(key, "IsPresent")) {
            if (value_type != 'b') return -EBADMSG;
            result = sd_bus_message_read(message, "b", &values->present_value);
            if (result < 0) return result;
            values->present = 1;
        } else if (!strcmp(key, "Percentage")) {
            if (value_type != 'd') return -EBADMSG;
            result = sd_bus_message_read(message, "d",
                                         &values->percentage_value);
            if (result < 0 || !isfinite(values->percentage_value))
                return -EBADMSG;
            values->percentage = 1;
        } else if (!strcmp(key, "State")) {
            if (value_type != 'u') return -EBADMSG;
            result = sd_bus_message_read(message, "u", &values->state_value);
            if (result < 0) return result;
            values->state = 1;
        } else {
            result = sd_bus_message_skip(message, NULL);
            if (result < 0) return result;
        }
        result = sd_bus_message_exit_container(message);
        if (result < 0) return result;
        result = sd_bus_message_exit_container(message);
        if (result < 0) return result;
    }
    return sd_bus_message_exit_container(message);
}

static int read_invalidated(sd_bus_message *message,
                            struct battery_invalidated *invalidated) {
    int result = sd_bus_message_enter_container(message, 'a', "s");
    if (result < 0) return result;
    if (!result) return -EBADMSG;
    size_t count = 0;
    for (;;) {
        const char *key = NULL;
        result = sd_bus_message_read(message, "s", &key);
        if (result < 0) return result;
        if (!result) break;
        if (++count > BATTERY_MAX_DICT ||
            !bounded_string(key, BATTERY_MAX_TEXT))
            return -E2BIG;
        if (!strcmp(key, "IsPresent")) invalidated->present = 1;
        else if (!strcmp(key, "Percentage")) invalidated->percentage = 1;
        else if (!strcmp(key, "State")) invalidated->state = 1;
    }
    return sd_bus_message_exit_container(message);
}

static void clear_upower(struct tomoe_battery *battery) {
    battery->owner[0] = '\0';
    battery->owner_epoch++;
    if (!battery->owner_epoch) battery->owner_epoch++;
    cancel_stale_calls(battery);
    memset(&battery->raw, 0, sizeof(battery->raw));
    memset(&battery->sysfs_raw, 0, sizeof(battery->sysfs_raw));
    battery->need_owner = 0;
    battery->need_get_all = 0;
    battery->upower_authoritative = 0;
    refresh_public(battery);
}

static void reset_upower_values(struct tomoe_battery *battery) {
    memset(&battery->raw, 0, sizeof(battery->raw));
    memset(&battery->sysfs_raw, 0, sizeof(battery->sysfs_raw));
    battery->upower_authoritative = 0;
    battery->sysfs_active = 0;
    refresh_public(battery);
}

static void apply_signal_values(struct tomoe_battery *battery,
                                const struct battery_values *values,
                                const struct battery_invalidated *invalidated) {
    if (values->present || invalidated->present) battery->present_epoch++;
    if (values->percentage || invalidated->percentage)
        battery->percentage_epoch++;
    if (values->state || invalidated->state) battery->state_epoch++;
    if (values->present) {
        battery->raw.present = values->present_value;
    }
    if (values->percentage) {
        battery->raw.percentage = 1;
        battery->raw.percentage_value = values->percentage_value;
    }
    if (values->state) {
        battery->raw.state = 1;
        battery->raw.state_value = values->state_value;
    }
    if (invalidated->present || invalidated->percentage ||
        invalidated->state)
        battery->need_get_all = 1;
    if (values->present || values->percentage || values->state)
        battery->upower_authoritative = 1;
    if (battery->upower_authoritative) battery->sysfs_active = 0;
    refresh_public(battery);
}

static int async_reply(sd_bus_message *message, void *userdata,
                       sd_bus_error *ret_error);

static void pending_remove(struct tomoe_battery *battery,
                           struct battery_call *call) {
    struct battery_call **cursor = &battery->calls;
    while (*cursor && *cursor != call) cursor = &(*cursor)->next;
    if (*cursor) {
        *cursor = call->next;
        if (battery->pending_count) battery->pending_count--;
    }
}

static void clear_pending(struct tomoe_battery *battery) {
    struct battery_call *call = battery->calls;
    while (call) {
        struct battery_call *next = call->next;
        if (call->slot) {
            sd_bus_slot_unref(call->slot);
            call->slot = NULL;
        }
        free(call);
        call = next;
    }
    battery->calls = NULL;
    battery->pending_count = 0;
}

static void cancel_stale_calls(struct tomoe_battery *battery) {
    struct battery_call **cursor = &battery->calls;
    while (*cursor) {
        struct battery_call *call = *cursor;
        if (call->owner_epoch == battery->owner_epoch) {
            cursor = &call->next;
            continue;
        }
        *cursor = call->next;
        if (battery->pending_count) battery->pending_count--;
        if (call->slot) sd_bus_slot_unref(call->slot);
        free(call);
    }
}

static int pending_owner(const struct tomoe_battery *battery) {
    for (const struct battery_call *call = battery->calls; call;
         call = call->next)
        if (call->kind == BATTERY_CALL_OWNER) return 1;
    return 0;
}

static int queue_call(struct tomoe_battery *battery, struct battery_call *call,
                      const char *destination, const char *interface,
                      const char *member, const char *types, ...) {
    if (!battery->bus || battery->bus_disabled ||
        battery->pending_count >= BATTERY_MAX_PENDING) {
        free(call);
        return -ENOSPC;
    }
    call->battery = battery;
    call->deadline_usec =
        deadline_after(monotonic_usec(), BATTERY_REQUEST_USEC);
    call->next = battery->calls;
    battery->calls = call;
    battery->pending_count++;
    va_list ap;
    va_start(ap, types);
    int result = sd_bus_call_method_asyncv(
        battery->bus, &call->slot, destination,
        !strcmp(interface, BATTERY_DBUS) ? BATTERY_DBUS_PATH : BATTERY_PATH,
        interface, member, async_reply, call, types, ap);
    va_end(ap);
    if (result < 0) {
        pending_remove(battery, call);
        if (call->slot) sd_bus_slot_unref(call->slot);
        free(call);
        return result;
    }
    return 0;
}

static int queue_owner(struct tomoe_battery *battery) {
    if (!battery->bus || pending_owner(battery)) return 0;
    struct battery_call *call = calloc(1, sizeof(*call));
    if (!call) return -ENOMEM;
    call->kind = BATTERY_CALL_OWNER;
    call->owner_epoch = battery->owner_epoch;
    int result = queue_call(battery, call, BATTERY_DBUS, BATTERY_DBUS,
                             "GetNameOwner", "s", BATTERY_UPOWER);
    if (result == -EAGAIN || result == -ENOSPC) {
        battery->need_owner = 1;
        return 0;
    }
    if (result >= 0) battery->need_owner = 0;
    return result;
}

static int queue_get_all(struct tomoe_battery *battery) {
    if (!battery->bus || !battery->need_get_all ||
        battery->pending_count >= BATTERY_MAX_PENDING)
        return -ENOSPC;
    struct battery_call *call = calloc(1, sizeof(*call));
    if (!call) return -ENOMEM;
    call->kind = BATTERY_CALL_GET_ALL;
    call->owner_epoch = battery->owner_epoch;
    call->present_epoch = battery->present_epoch;
    call->percentage_epoch = battery->percentage_epoch;
    call->state_epoch = battery->state_epoch;
    call->request_serial = ++battery->next_request_serial;
    if (!call->request_serial) call->request_serial = ++battery->next_request_serial;
    int result = queue_call(battery, call, BATTERY_UPOWER,
                            BATTERY_PROPERTIES, "GetAll", "s", BATTERY_IFACE);
    if (result >= 0) battery->need_get_all = 0;
    return result;
}

static int reply_is_error(sd_bus_message *message) {
    return !message || sd_bus_message_get_error(message) != NULL;
}

static int message_path_is(sd_bus_message *message, const char *path) {
    const char *actual = sd_bus_message_get_path(message);
    return actual && !strcmp(actual, path);
}

static int message_sender_matches(struct tomoe_battery *battery,
                                  sd_bus_message *message) {
    const char *sender = sd_bus_message_get_sender(message);
    if (!sender) return 0;
    if (!strcmp(sender, BATTERY_UPOWER)) return battery->owner[0] != '\0';
    return battery->owner[0] && valid_unique_name(sender) &&
           !strcmp(sender, battery->owner);
}

static void fallback_to_sysfs(struct tomoe_battery *battery) {
    clear_upower(battery);
    start_sysfs(battery);
}

static void handle_owner_reply(struct battery_call *call,
                               sd_bus_message *message) {
    struct tomoe_battery *battery = call->battery;
    if (call->owner_epoch != battery->owner_epoch) return;
    const sd_bus_error *error = message ? sd_bus_message_get_error(message) : NULL;
    if (error) {
        battery->need_get_all = 1;
        (void)queue_get_all(battery);
        return;
    }
    const char *owner = NULL;
    if (sd_bus_message_read(message, "s", &owner) < 0 ||
        !valid_unique_name(owner))
        return;
    if (battery->owner[0] && strcmp(battery->owner, owner)) return;
    battery->need_owner = 0;
    if (!battery->owner[0]) reset_upower_values(battery);
    memcpy(battery->owner, owner, strlen(owner) + 1U);
    battery->owner_epoch++;
    if (!battery->owner_epoch) battery->owner_epoch++;
    cancel_stale_calls(battery);
    battery->need_get_all = 1;
    (void)queue_get_all(battery);
}

static void handle_get_all_reply(struct battery_call *call,
                                 sd_bus_message *message) {
    struct tomoe_battery *battery = call->battery;
    if (call->owner_epoch != battery->owner_epoch) return;
    if (reply_is_error(message)) {
        if (!battery->upower_authoritative) start_sysfs(battery);
        return;
    }
    const char *sender = sd_bus_message_get_sender(message);
    if (!sender || !valid_unique_name(sender)) return;
    if (battery->owner[0] && strcmp(battery->owner, sender)) return;
    if (!battery->owner[0]) {
        reset_upower_values(battery);
        memcpy(battery->owner, sender, strlen(sender) + 1U);
        battery->owner_epoch++;
        if (!battery->owner_epoch) battery->owner_epoch++;
        cancel_stale_calls(battery);
    }
    struct battery_values values = {0};
    if (read_property_dict(message, &values) < 0) return;
    if (values.present && battery->present_epoch == call->present_epoch &&
        call->request_serial >= battery->applied_present_serial) {
        battery->raw.present = values.present_value;
        battery->applied_present_serial = call->request_serial;
    }
    if (values.percentage &&
        battery->percentage_epoch == call->percentage_epoch &&
        call->request_serial >= battery->applied_percentage_serial) {
        battery->raw.percentage = 1;
        battery->raw.percentage_value = values.percentage_value;
        battery->applied_percentage_serial = call->request_serial;
    }
    if (values.state && battery->state_epoch == call->state_epoch &&
        call->request_serial >= battery->applied_state_serial) {
        battery->raw.state = 1;
        battery->raw.state_value = values.state_value;
        battery->applied_state_serial = call->request_serial;
    }
    battery->upower_authoritative = 1;
    battery->sysfs_active = 0;
    refresh_public(battery);
    if (battery->need_get_all) (void)queue_get_all(battery);
}

static int owner_changed_signal(sd_bus_message *message, void *userdata,
                                sd_bus_error *ret_error) {
    struct tomoe_battery *battery = userdata;
    (void)ret_error;
    if (!message_path_is(message, BATTERY_DBUS_PATH)) return 1;
    const char *sender = sd_bus_message_get_sender(message);
    if (!sender || strcmp(sender, BATTERY_DBUS)) return 1;
    const char *name = NULL;
    const char *old_owner = NULL;
    const char *new_owner = NULL;
    int result = sd_bus_message_read(message, "sss", &name, &old_owner,
                                     &new_owner);
    if (result < 0 || !name || strcmp(name, BATTERY_UPOWER) ||
        (old_owner && *old_owner && !valid_unique_name(old_owner)) ||
        (new_owner && *new_owner && !valid_unique_name(new_owner)))
        return 1;
    if (battery->owner[0] && old_owner && *old_owner &&
        strcmp(battery->owner, old_owner))
        return 1;
    if (new_owner && *new_owner) {
        reset_upower_values(battery);
        memcpy(battery->owner, new_owner, strlen(new_owner) + 1U);
        battery->owner_epoch++;
        if (!battery->owner_epoch) battery->owner_epoch++;
        cancel_stale_calls(battery);
        battery->need_owner = 0;
        battery->need_get_all = 1;
        battery->upower_authoritative = 0;
        (void)queue_get_all(battery);
    } else {
        fallback_to_sysfs(battery);
    }
    return 1;
}

static int properties_signal(sd_bus_message *message, void *userdata,
                             sd_bus_error *ret_error) {
    struct tomoe_battery *battery = userdata;
    (void)ret_error;
    if (!message_path_is(message, BATTERY_PATH) ||
        !message_sender_matches(battery, message))
        return 1;
    const char *interface = NULL;
    int result = sd_bus_message_read(message, "s", &interface);
    if (result < 0 || !interface || strcmp(interface, BATTERY_IFACE))
        return 1;
    struct battery_values values = {0};
    struct battery_invalidated invalidated = {0};
    if (read_property_dict(message, &values) < 0 ||
        read_invalidated(message, &invalidated) < 0)
        return 1;
    if (values.present || values.percentage || values.state ||
        invalidated.present || invalidated.percentage || invalidated.state)
        apply_signal_values(battery, &values, &invalidated);
    return 1;
}

static int async_reply(sd_bus_message *message, void *userdata,
                       sd_bus_error *ret_error) {
    struct battery_call *call = userdata;
    (void)ret_error;
    if (!call || !call->battery) return 1;
    struct tomoe_battery *battery = call->battery;
    sd_bus_slot *slot = call->slot;
    call->slot = NULL;
    pending_remove(battery, call);
    if (call->kind == BATTERY_CALL_OWNER)
        handle_owner_reply(call, message);
    else
        handle_get_all_reply(call, message);
    if (slot) sd_bus_slot_unref(slot);
    free(call);
    return 1;
}

static void close_bus(struct tomoe_battery *battery) {
    if (!battery) return;
    for (size_t index = 0; index < sizeof(battery->matches) /
                                      sizeof(battery->matches[0]); index++) {
        if (battery->matches[index]) {
            sd_bus_slot_unref(battery->matches[index]);
            battery->matches[index] = NULL;
        }
    }
    clear_pending(battery);
    if (battery->bus) {
        sd_bus_close(battery->bus);
        sd_bus_unref(battery->bus);
        battery->bus = NULL;
    }
}

static int setup_ready(sd_bus *bus, uint64_t deadline) {
    for (;;) {
        if (sd_bus_is_ready(bus) > 0) return 0;
        uint64_t now = monotonic_usec();
        if (now >= deadline) return -ETIMEDOUT;
        int result = sd_bus_process(bus, NULL);
        if (result < 0 && result != -EAGAIN && result != -EINTR)
            return result;
        if (sd_bus_is_ready(bus) > 0) return 0;
        if (!result) {
            now = monotonic_usec();
            if (now >= deadline) return -ETIMEDOUT;
            result = sd_bus_wait(bus, deadline - now);
            if (result < 0 && result != -EINTR) return result;
        }
    }
}

static int set_remaining_timeout(sd_bus *bus, uint64_t deadline) {
    uint64_t now = monotonic_usec();
    if (now >= deadline) return -ETIMEDOUT;
    return sd_bus_set_method_call_timeout(bus, deadline - now);
}

static void disconnect_bus(struct tomoe_battery *battery) {
    battery->bus_disabled = 1;
    close_bus(battery);
    fallback_to_sysfs(battery);
}

struct tomoe_battery *tomoe_battery_open(const char *sysfs_root, int *error) {
    if (!error) return NULL;
    *error = EINVAL;
    struct tomoe_battery *battery = calloc(1, sizeof(*battery));
    if (!battery) {
        *error = ENOMEM;
        return NULL;
    }
    const char *root = sysfs_root && *sysfs_root
        ? sysfs_root : BATTERY_DEFAULT_ROOT;
    if (!bounded_string(root, BATTERY_MAX_ROOT)) {
        free(battery);
        *error = ENAMETOOLONG;
        return NULL;
    }
    memcpy(battery->sysfs_root, root, strlen(root) + 1U);
    public_default(battery);
    battery->revision = 1;

    sd_bus *bus = NULL;
    int result = sd_bus_new(&bus);
    if (result < 0) goto fallback;
    result = sd_bus_set_bus_client(bus, 1);
    if (result < 0) goto fallback_bus;
    const char *address = getenv("DBUS_SYSTEM_BUS_ADDRESS");
    if (!address || !*address) address = BATTERY_SYSTEM_ADDRESS;
    result = sd_bus_set_address(bus, address);
    if (result < 0) goto fallback_bus;
    result = sd_bus_set_method_call_timeout(bus, BATTERY_SETUP_USEC);
    if (result < 0) goto fallback_bus;
    result = sd_bus_start(bus);
    if (result < 0) goto fallback_bus;
    uint64_t deadline = deadline_after(monotonic_usec(), BATTERY_SETUP_USEC);
    result = setup_ready(bus, deadline);
    if (result < 0) goto fallback_bus;
    battery->bus = bus;
    bus = NULL;
    result = set_remaining_timeout(battery->bus, deadline);
    if (result < 0) goto fallback_bus_handle;
    result = sd_bus_add_match(
        battery->bus, &battery->matches[0],
        "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.freedesktop.UPower'",
        owner_changed_signal, battery);
    if (result < 0) goto fallback_bus_handle;
    result = set_remaining_timeout(battery->bus, deadline);
    if (result < 0) goto fallback_bus_handle;
    result = sd_bus_add_match(
        battery->bus, &battery->matches[1],
        "type='signal',sender='org.freedesktop.UPower',path='/org/freedesktop/UPower/devices/DisplayDevice',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
        properties_signal, battery);
    if (result < 0) goto fallback_bus_handle;
    result = set_remaining_timeout(battery->bus, deadline);
    if (result < 0) goto fallback_bus_handle;
    result = queue_owner(battery);
    if (result < 0) goto fallback_bus_handle;
    result = sd_bus_set_method_call_timeout(battery->bus, BATTERY_REQUEST_USEC);
    if (result < 0) goto fallback_bus_handle;
    *error = 0;
    return battery;

fallback_bus_handle:
    close_bus(battery);
    battery->bus_disabled = 1;
    goto fallback;
fallback_bus:
    if (bus) {
        sd_bus_close(bus);
        sd_bus_unref(bus);
        bus = NULL;
    }
fallback:
    battery->bus_disabled = 1;
    start_sysfs(battery);
    *error = 0;
    return battery;
}

static void service_needs(struct tomoe_battery *battery) {
    if (battery->need_owner && !battery->owner[0])
        (void)queue_owner(battery);
    if (battery->need_get_all) (void)queue_get_all(battery);
}

int tomoe_battery_poll(struct tomoe_battery *battery) {
    if (!battery) return -EINVAL;
    battery->changed = 0;
    service_needs(battery);
    uint64_t start = monotonic_usec();
    size_t processed = 0;
    while (battery->bus && processed < BATTERY_POLL_MESSAGES) {
        uint64_t now = monotonic_usec();
        if (now >= start && now - start >= BATTERY_POLL_BUDGET_USEC) break;
        int result = sd_bus_process(battery->bus, NULL);
        if (result < 0) {
            if (result == -EAGAIN || result == -EINTR) break;
            disconnect_bus(battery);
            break;
        }
        if (!result) break;
        processed++;
        service_needs(battery);
    }
    if (battery->sysfs_active) {
        uint64_t now = monotonic_usec();
        if (now >= battery->sysfs_next_usec) {
            read_sysfs(battery);
            if (battery->sysfs_active)
                battery->sysfs_next_usec =
                    deadline_after(now, BATTERY_SYSFS_INTERVAL_USEC);
        }
    }
    service_needs(battery);
    return battery->changed ? 1 : 0;
}

uint64_t tomoe_battery_revision(const struct tomoe_battery *battery) {
    return battery ? battery->revision : 0;
}

int tomoe_battery_timeout(const struct tomoe_battery *battery, int max_ms) {
    if (!battery) return 0;
    if (max_ms < 0) max_ms = 0;
    uint64_t best = UINT64_MAX;
    for (const struct battery_call *call = battery->calls; call;
         call = call->next)
        if (call->deadline_usec < best) best = call->deadline_usec;
    if (battery->sysfs_active && battery->sysfs_next_usec < best)
        best = battery->sysfs_next_usec;
    if (best == UINT64_MAX) return max_ms;
    uint64_t now = monotonic_usec();
    if (best <= now) return 0;
    uint64_t milliseconds = (best - now + 999ULL) / 1000ULL;
    if (milliseconds >= (uint64_t)max_ms) return max_ms;
    return (int)milliseconds;
}

void tomoe_battery_close(struct tomoe_battery *battery) {
    if (!battery) return;
    close_bus(battery);
    free(battery->snapshot);
    free(battery);
}
