#define _GNU_SOURCE

#include "notifications.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-bus.h>

#define NOTIFICATION_NAME "org.freedesktop.Notifications"
#define NOTIFICATION_PATH "/org/freedesktop/Notifications"
#define NOTIFICATION_INTERFACE "org.freedesktop.Notifications"

#define NOTIFICATION_MAX_RECORDS 64U
#define NOTIFICATION_MAX_TEXT 4096U
#define NOTIFICATION_MAX_ACTIONS 64U
#define NOTIFICATION_MAX_HINTS 64U
#define NOTIFICATION_MAX_TEXT_BYTES (256U * 1024U)
#define NOTIFICATION_DEFAULT_TIMEOUT_USEC 5000000ULL
#define NOTIFICATION_SETUP_TIMEOUT_USEC 2000000ULL
#define NOTIFICATION_POLL_MESSAGES 64U
#define NOTIFICATION_POLL_BUDGET_USEC 4000ULL
#define NOTIFICATION_MAX_RUNTIME_PATH 4096U

struct notification_record {
    uint32_t id;
    int urgent;
    uint64_t generation;
    uint64_t deadline_usec;
    char app[NOTIFICATION_MAX_TEXT + 1U];
    char summary[NOTIFICATION_MAX_TEXT + 1U];
    char body[NOTIFICATION_MAX_TEXT + 1U];
};

struct tomoe_notifications {
    sd_bus *bus;
    sd_bus_slot *object_slot;
    struct notification_record records[NOTIFICATION_MAX_RECORDS];
    size_t count;
    uint32_t next_id;
    uint64_t revision;
    char *snapshot;
    int available;
    int failed;
    int failure;
    int changed;
};

struct text_buffer {
    char *data;
    size_t length;
    size_t capacity;
};

int tomoe_notifications_abi(void) { return 1; }

static uint64_t monotonic_usec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    if ((uint64_t)ts.tv_sec > UINT64_MAX / 1000000ULL) return UINT64_MAX;
    uint64_t seconds = (uint64_t)ts.tv_sec * 1000000ULL;
    uint64_t nanos = (uint64_t)ts.tv_nsec / 1000ULL;
    return seconds > UINT64_MAX - nanos ? UINT64_MAX : seconds + nanos;
}

static int positive_error(int result) {
    if (result < 0) return -result;
    return result ? result : EIO;
}

static int make_fallback_address(char *destination, size_t capacity,
                                 const char *runtime) {
    static const char hex[] = "0123456789ABCDEF";
    size_t length = strnlen(runtime, NOTIFICATION_MAX_RUNTIME_PATH + 6U);
    if (length > NOTIFICATION_MAX_RUNTIME_PATH + 5U) return -ENAMETOOLONG;
    const char prefix[] = "unix:path=";
    if (sizeof(prefix) - 1U > capacity) return -ENAMETOOLONG;
    memcpy(destination, prefix, sizeof(prefix) - 1U);
    size_t offset = sizeof(prefix) - 1U;
    for (size_t index = 0; index < length; index++) {
        if (offset > capacity - 4U) return -ENAMETOOLONG;
        unsigned char value = (unsigned char)runtime[index];
        destination[offset++] = '%';
        destination[offset++] = hex[value >> 4U];
        destination[offset++] = hex[value & 0x0fU];
    }
    if (offset >= capacity) return -ENAMETOOLONG;
    destination[offset] = '\0';
    return 0;
}

static void revision_bump(struct tomoe_notifications *notifications) {
    notifications->revision++;
    if (!notifications->revision) notifications->revision = 1;
    notifications->changed = 1;
}

static void close_bus(struct tomoe_notifications *notifications) {
    if (!notifications) return;
    if (notifications->object_slot) {
        sd_bus_slot_unref(notifications->object_slot);
        notifications->object_slot = NULL;
    }
    if (notifications->bus) {
        sd_bus_close(notifications->bus);
        sd_bus_unref(notifications->bus);
        notifications->bus = NULL;
    }
}

static void mark_bus_failed(struct tomoe_notifications *notifications,
                            int result) {
    if (notifications->failed) return;
    notifications->failed = 1;
    notifications->failure = positive_error(result);
    notifications->available = 0;
    notifications->count = 0;
    revision_bump(notifications);
}

static int reply_error(struct tomoe_notifications *notifications,
                       sd_bus_message *message, const char *name,
                       const char *format, ...) {
    va_list ap;
    int result;
    va_start(ap, format);
    result = sd_bus_reply_method_errorfv(message, name, format, ap);
    va_end(ap);
    if (result < 0) {
        mark_bus_failed(notifications, result);
        return result;
    }
    return 1;
}

static int reply_invalid(struct tomoe_notifications *notifications,
                         sd_bus_message *message, const char *what) {
    return reply_error(notifications, message,
                       "org.freedesktop.Notifications.Error.InvalidArgs",
                       "%s", what);
}

static int reply_failed(struct tomoe_notifications *notifications,
                        sd_bus_message *message, const char *what) {
    return reply_error(notifications, message,
                       "org.freedesktop.Notifications.Error.Failed", "%s",
                       what);
}

static int reply_empty(struct tomoe_notifications *notifications,
                       sd_bus_message *message) {
    int result = sd_bus_reply_method_return(message, "");
    if (result < 0) {
        mark_bus_failed(notifications, result);
        return result;
    }
    return 1;
}

static int bounded_text(const char *text) {
    return text && strnlen(text, NOTIFICATION_MAX_TEXT + 1U) <=
                         NOTIFICATION_MAX_TEXT;
}

static int text_buffer_reserve(struct text_buffer *buffer, size_t extra) {
    if (extra > SIZE_MAX - buffer->length - 1U) return -E2BIG;
    size_t required = buffer->length + extra + 1U;
    if (required <= buffer->capacity) return 0;
    size_t capacity = buffer->capacity ? buffer->capacity : 256U;
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

static int text_buffer_append(struct text_buffer *buffer, const char *text,
                              size_t length) {
    int result = text_buffer_reserve(buffer, length);
    if (result < 0) return result;
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 0;
}

static int text_buffer_literal(struct text_buffer *buffer, const char *text) {
    return text_buffer_append(buffer, text, strlen(text));
}

static int text_buffer_uint(struct text_buffer *buffer, uint32_t value) {
    char text[16];
    int length = snprintf(text, sizeof(text), "%u", value);
    if (length < 0 || (size_t)length >= sizeof(text)) return -EOVERFLOW;
    return text_buffer_append(buffer, text, (size_t)length);
}

static int text_buffer_string(struct text_buffer *buffer, const char *text) {
    int result = text_buffer_append(buffer, "\"", 1);
    if (result < 0) return result;
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor;
         cursor++) {
        if (*cursor == '\\' || *cursor == '"') {
            result = text_buffer_append(buffer, "\\", 1);
            if (result < 0) return result;
        }
        result = text_buffer_append(buffer, (const char *)cursor, 1);
        if (result < 0) return result;
    }
    return text_buffer_append(buffer, "\"", 1);
}

static int build_snapshot(struct tomoe_notifications *notifications,
                          char **result_out) {
    struct text_buffer buffer = {0};
    int result = text_buffer_literal(&buffer, "(:available ");
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer,
                                 notifications->available ? "t" : "nil");
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, " :notifications (");
    if (result < 0) goto failed;
    for (size_t index = 0; index < notifications->count; index++) {
        const struct notification_record *record =
            &notifications->records[index];
        if (index) {
            result = text_buffer_literal(&buffer, " ");
            if (result < 0) goto failed;
        }
        result = text_buffer_literal(&buffer, "(:id ");
        if (result < 0) goto failed;
        result = text_buffer_uint(&buffer, record->id);
        if (result < 0) goto failed;
        result = text_buffer_literal(&buffer, " :app ");
        if (result < 0) goto failed;
        result = text_buffer_string(&buffer, record->app);
        if (result < 0) goto failed;
        result = text_buffer_literal(&buffer, " :summary ");
        if (result < 0) goto failed;
        result = text_buffer_string(&buffer, record->summary);
        if (result < 0) goto failed;
        result = text_buffer_literal(&buffer, " :body ");
        if (result < 0) goto failed;
        result = text_buffer_string(&buffer, record->body);
        if (result < 0) goto failed;
        result = text_buffer_literal(&buffer, " :urgent ");
        if (result < 0) goto failed;
        result = text_buffer_literal(&buffer, record->urgent ? "t" : "nil");
        if (result < 0) goto failed;
        result = text_buffer_literal(&buffer, ")");
        if (result < 0) goto failed;
    }
    result = text_buffer_literal(&buffer, "))");
    if (result < 0) goto failed;
    *result_out = buffer.data;
    return 0;

failed:
    free(buffer.data);
    return result;
}

const char *tomoe_notifications_snapshot(
    struct tomoe_notifications *notifications) {
    if (!notifications) return NULL;
    char *snapshot = NULL;
    if (build_snapshot(notifications, &snapshot) < 0) return NULL;
    free(notifications->snapshot);
    notifications->snapshot = snapshot;
    return notifications->snapshot;
}

static int find_record(const struct tomoe_notifications *notifications,
                       uint32_t id) {
    for (size_t index = 0; index < notifications->count; index++)
        if (notifications->records[index].id == id) return (int)index;
    return -1;
}

static size_t record_text_bytes(const struct notification_record *record) {
    return strlen(record->app) + strlen(record->summary) + strlen(record->body);
}

static size_t all_text_bytes(const struct tomoe_notifications *notifications,
                             int skip) {
    size_t total = 0;
    for (size_t index = 0; index < notifications->count; index++) {
        if ((int)index == skip) continue;
        size_t bytes = record_text_bytes(&notifications->records[index]);
        if (bytes > SIZE_MAX - total) return SIZE_MAX;
        total += bytes;
    }
    return total;
}

static int allocate_id(struct tomoe_notifications *notifications,
                       uint32_t *id_out) {
    uint32_t first = notifications->next_id;
    uint32_t candidate = first;
    do {
        candidate++;
        if (!candidate) candidate = 1;
        if (find_record(notifications, candidate) < 0) {
            *id_out = candidate;
            notifications->next_id = candidate;
            return 0;
        }
    } while (candidate != first);
    return ENOSPC;
}

static void erase_record(struct tomoe_notifications *notifications,
                         size_t index) {
    if (index + 1U < notifications->count) {
        memmove(&notifications->records[index],
                &notifications->records[index + 1U],
                (notifications->count - index - 1U) *
                    sizeof(notifications->records[0]));
    }
    notifications->count--;
}

static int emit_closed(struct tomoe_notifications *notifications, uint32_t id,
                       uint32_t reason) {
    int result = sd_bus_emit_signal(notifications->bus, NOTIFICATION_PATH,
                                    NOTIFICATION_INTERFACE,
                                    "NotificationClosed", "uu", id, reason);
    if (result < 0) {
        mark_bus_failed(notifications, result);
        return -1;
    }
    return 0;
}

static int expire_due(struct tomoe_notifications *notifications,
                      uint64_t now) {
    int changed = 0;
    size_t index = 0;
    while (index < notifications->count) {
        struct notification_record *record = &notifications->records[index];
        if (!record->deadline_usec || record->deadline_usec > now) {
            index++;
            continue;
        }
        uint32_t id = record->id;
        erase_record(notifications, index);
        revision_bump(notifications);
        changed = 1;
        (void)emit_closed(notifications, id, 1U);
        if (notifications->failed) break;
    }
    return changed;
}

static int read_actions(sd_bus_message *message) {
    int result = sd_bus_message_enter_container(message, 'a', "s");
    if (result < 0) return result;
    size_t count = 0;
    for (;;) {
        const char *action = NULL;
        result = sd_bus_message_read(message, "s", &action);
        if (result < 0) return result;
        if (!result) break;
        if (++count > NOTIFICATION_MAX_ACTIONS || !bounded_text(action))
            return -E2BIG;
    }
    return sd_bus_message_exit_container(message);
}

static int read_hints(sd_bus_message *message, int *urgent) {
    int result = sd_bus_message_enter_container(message, 'a', "{sv}");
    if (result < 0) return result;
    size_t count = 0;
    for (;;) {
        result = sd_bus_message_enter_container(message, 'e', "sv");
        if (result < 0) return result;
        if (!result) break;
        const char *key = NULL;
        result = sd_bus_message_read(message, "s", &key);
        if (result < 0 || !bounded_text(key)) return result < 0 ? result : -E2BIG;

        char variant_type = 0;
        const char *variant_contents = NULL;
        result = sd_bus_message_peek_type(message, &variant_type,
                                           &variant_contents);
        if (result <= 0 || variant_type != 'v' || !variant_contents)
            return result < 0 ? result : -EBADMSG;
        result = sd_bus_message_enter_container(message, 'v',
                                                variant_contents);
        if (result < 0) return result;
        char value_type = 0;
        const char *value_contents = NULL;
        result = sd_bus_message_peek_type(message, &value_type,
                                           &value_contents);
        if (result <= 0) return result < 0 ? result : -EBADMSG;
        if (!strcmp(key, "urgency") && value_type == 'y') {
            uint8_t value = 0;
            result = sd_bus_message_read(message, "y", &value);
            if (result < 0) return result;
            *urgent = value >= 2U;
        } else {
            result = sd_bus_message_skip(message, NULL);
            if (result < 0) return result;
        }
        result = sd_bus_message_exit_container(message);
        if (result < 0) return result;
        result = sd_bus_message_exit_container(message);
        if (result < 0) return result;
        if (++count > NOTIFICATION_MAX_HINTS) return -E2BIG;
    }
    return sd_bus_message_exit_container(message);
}

static int method_get_server_information(sd_bus_message *message,
                                         void *userdata,
                                         sd_bus_error *ret_error) {
    struct tomoe_notifications *notifications = userdata;
    (void)ret_error;
    int result = sd_bus_reply_method_return(message, "ssss", "tomoe", "tomoe",
                                            "0.1.0", "1.2");
    if (result < 0) {
        mark_bus_failed(notifications, result);
        return result;
    }
    return 1;
}

static int method_get_capabilities(sd_bus_message *message, void *userdata,
                                   sd_bus_error *ret_error) {
    struct tomoe_notifications *notifications = userdata;
    (void)ret_error;
    int result = sd_bus_reply_method_return(message, "as", 1U, "body");
    if (result < 0) {
        mark_bus_failed(notifications, result);
        return result;
    }
    return 1;
}

static int method_notify(sd_bus_message *message, void *userdata,
                         sd_bus_error *ret_error) {
    struct tomoe_notifications *notifications = userdata;
    (void)ret_error;
    const char *app = NULL;
    const char *icon = NULL;
    const char *summary = NULL;
    const char *body = NULL;
    uint32_t replaces = 0;
    int32_t timeout_ms = 0;
    int result = sd_bus_message_read(message, "susss", &app, &replaces, &icon,
                                     &summary, &body);
    if (result < 0 || !bounded_text(app) || !bounded_text(summary) ||
            !bounded_text(body) || !bounded_text(icon))
        return reply_invalid(notifications, message,
                             "invalid notification text or arguments");
    result = read_actions(message);
    if (result < 0)
        return reply_invalid(notifications, message,
                             "too many or invalid notification actions");
    int urgent = 0;
    result = read_hints(message, &urgent);
    if (result < 0)
        return reply_invalid(notifications, message,
                             "too many or invalid notification hints");
    result = sd_bus_message_read(message, "i", &timeout_ms);
    if (result < 0 || sd_bus_message_at_end(message, 1) <= 0)
        return reply_invalid(notifications, message,
                             "malformed notification arguments");

    int existing = replaces ? find_record(notifications, replaces) : -1;
    if (existing < 0 && notifications->count >= NOTIFICATION_MAX_RECORDS)
        return reply_failed(notifications, message, "notification limit reached");

    size_t candidate_bytes = strlen(app) + strlen(summary) + strlen(body);
    size_t retained_bytes = all_text_bytes(notifications, existing);
    if (candidate_bytes > NOTIFICATION_MAX_TEXT_BYTES ||
            retained_bytes > NOTIFICATION_MAX_TEXT_BYTES - candidate_bytes)
        return reply_failed(notifications, message,
                            "notification text limit reached");

    struct notification_record *candidate = calloc(1, sizeof(*candidate));
    if (!candidate) return reply_failed(notifications, message, "out of memory");
    if (replaces) {
        candidate->id = replaces;
    } else if (allocate_id(notifications, &candidate->id)) {
        free(candidate);
        return reply_failed(notifications, message, "notification IDs exhausted");
    }
    candidate->urgent = urgent;
    candidate->generation = existing >= 0
        ? notifications->records[existing].generation + 1U : 1U;
    if (!candidate->generation) candidate->generation = 1;
    memcpy(candidate->app, app, strlen(app) + 1U);
    memcpy(candidate->summary, summary, strlen(summary) + 1U);
    memcpy(candidate->body, body, strlen(body) + 1U);
    uint64_t now = monotonic_usec();
    if (timeout_ms < 0) {
        candidate->deadline_usec = now > UINT64_MAX -
                                           NOTIFICATION_DEFAULT_TIMEOUT_USEC
                                       ? UINT64_MAX
                                       : now + NOTIFICATION_DEFAULT_TIMEOUT_USEC;
    } else if (timeout_ms > 0) {
        uint64_t duration = (uint64_t)(uint32_t)timeout_ms * 1000ULL;
        candidate->deadline_usec = now > UINT64_MAX - duration
            ? UINT64_MAX : now + duration;
    }

    if (existing >= 0) erase_record(notifications, (size_t)existing);
    notifications->records[notifications->count++] = *candidate;
    free(candidate);
    revision_bump(notifications);
    result = sd_bus_reply_method_return(message, "u",
                                        notifications->records[
                                            notifications->count - 1U].id);
    if (result < 0) {
        mark_bus_failed(notifications, result);
        return result;
    }
    return 1;
}

static int method_close_notification(sd_bus_message *message, void *userdata,
                                     sd_bus_error *ret_error) {
    struct tomoe_notifications *notifications = userdata;
    (void)ret_error;
    uint32_t id = 0;
    int result = sd_bus_message_read(message, "u", &id);
    if (result < 0 || sd_bus_message_at_end(message, 1) <= 0)
        return reply_invalid(notifications, message,
                             "malformed CloseNotification arguments");
    int index = find_record(notifications, id);
    if (index >= 0) {
        erase_record(notifications, (size_t)index);
        revision_bump(notifications);
        (void)emit_closed(notifications, id, 3U);
    }
    return reply_empty(notifications, message);
}

static const sd_bus_vtable notification_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetServerInformation", "", "ssss",
                  method_get_server_information, 0),
    SD_BUS_METHOD("GetCapabilities", "", "as", method_get_capabilities, 0),
    SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", method_notify, 0),
    SD_BUS_METHOD("CloseNotification", "u", "", method_close_notification, 0),
    SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
    SD_BUS_VTABLE_END,
};

struct tomoe_notifications *tomoe_notifications_open(int *error) {
    if (!error) return NULL;
    *error = EINVAL;
    struct tomoe_notifications *notifications = calloc(1, sizeof(*notifications));
    if (!notifications) {
        *error = ENOMEM;
        return NULL;
    }
    sd_bus *bus = NULL;
    int result = sd_bus_new(&bus);
    if (result < 0) goto failed;
    result = sd_bus_set_bus_client(bus, 1);
    if (result < 0) goto failed;
    result = sd_bus_set_method_call_timeout(bus, NOTIFICATION_SETUP_TIMEOUT_USEC);
    if (result < 0) goto failed;
    const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
    char address_buffer[NOTIFICATION_MAX_RUNTIME_PATH * 3U + 32U];
    if (!address || !*address) {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        if (!runtime || !*runtime) {
            result = -ENOMEDIUM;
            goto failed;
        }
        char runtime_bus[NOTIFICATION_MAX_RUNTIME_PATH + 5U];
        size_t runtime_length = strnlen(runtime,
                                        NOTIFICATION_MAX_RUNTIME_PATH + 1U);
        if (runtime_length > NOTIFICATION_MAX_RUNTIME_PATH ||
                runtime_length > SIZE_MAX - 5U) {
            result = -ENAMETOOLONG;
            goto failed;
        }
        memcpy(runtime_bus, runtime, runtime_length);
        memcpy(runtime_bus + runtime_length, "/bus", 5U);
        result = make_fallback_address(address_buffer, sizeof(address_buffer),
                                       runtime_bus);
        if (result < 0) goto failed;
        address = address_buffer;
    }
    result = sd_bus_set_address(bus, address);
    if (result < 0) goto failed;
    result = sd_bus_start(bus);
    if (result < 0) goto failed;
    uint64_t deadline = monotonic_usec() + NOTIFICATION_SETUP_TIMEOUT_USEC;
    for (;;) {
        if (sd_bus_is_ready(bus) > 0) break;
        uint64_t now = monotonic_usec();
        if (now >= deadline) {
            result = -ETIMEDOUT;
            goto failed;
        }
        result = sd_bus_process(bus, NULL);
        if (result < 0 && result != -EAGAIN && result != -EINTR) goto failed;
        if (sd_bus_is_ready(bus) > 0) break;
        if (!result) {
            now = monotonic_usec();
            if (now >= deadline) {
                result = -ETIMEDOUT;
                goto failed;
            }
            uint64_t remaining = deadline - now;
            result = sd_bus_wait(bus, remaining);
            if (result < 0 && result != -EINTR) goto failed;
        }
    }
    uint64_t now = monotonic_usec();
    if (now >= deadline) {
        result = -ETIMEDOUT;
        goto failed;
    }
    uint64_t remaining = deadline - now;
    result = sd_bus_set_method_call_timeout(bus, remaining);
    if (result < 0) goto failed;
    result = sd_bus_request_name(bus, NOTIFICATION_NAME, 0);
    if (result <= 0) {
        if (!result) result = -EEXIST;
        goto failed;
    }
    result = sd_bus_add_object_vtable(bus, &notifications->object_slot,
                                      NOTIFICATION_PATH,
                                      NOTIFICATION_INTERFACE,
                                      notification_vtable, notifications);
    if (result < 0) goto failed;
    notifications->bus = bus;
    bus = NULL;
    notifications->available = 1;
    notifications->revision = 1;
    *error = 0;
    return notifications;

failed:
    if (notifications->object_slot) sd_bus_slot_unref(notifications->object_slot);
    sd_bus_unref(bus);
    free(notifications);
    *error = positive_error(result);
    return NULL;
}

int tomoe_notifications_poll(struct tomoe_notifications *notifications) {
    if (!notifications) return -EINVAL;
    if (notifications->failed) {
        close_bus(notifications);
        return -notifications->failure;
    }
    notifications->changed = 0;
    uint64_t start = monotonic_usec();
    uint64_t now = start;
    (void)expire_due(notifications, now);
    size_t processed = 0;
    while (notifications->bus && processed < NOTIFICATION_POLL_MESSAGES) {
        now = monotonic_usec();
        if (now >= start && now - start >= NOTIFICATION_POLL_BUDGET_USEC) break;
        int result = sd_bus_process(notifications->bus, NULL);
        if (result < 0) {
            if (result == -EAGAIN || result == -EINTR) break;
            mark_bus_failed(notifications, result);
            break;
        }
        if (!result) break;
        processed++;
        if (notifications->failed) break;
        now = monotonic_usec();
        (void)expire_due(notifications, now);
    }
    now = monotonic_usec();
    (void)expire_due(notifications, now);
    if (notifications->failed) {
        int failure = notifications->failure;
        close_bus(notifications);
        return -failure;
    }
    return notifications->changed ? 1 : 0;
}

uint64_t tomoe_notifications_revision(
    const struct tomoe_notifications *notifications) {
    return notifications ? notifications->revision : 0;
}

int tomoe_notifications_timeout(
    const struct tomoe_notifications *notifications, int max_ms) {
    if (!notifications) return 0;
    if (max_ms < 0) max_ms = 0;
    uint64_t now = monotonic_usec();
    uint64_t best = UINT64_MAX;
    for (size_t index = 0; index < notifications->count; index++) {
        uint64_t deadline = notifications->records[index].deadline_usec;
        if (deadline && deadline < best) best = deadline;
    }
    if (best == UINT64_MAX) return max_ms;
    if (best <= now) return 0;
    uint64_t milliseconds = (best - now + 999ULL) / 1000ULL;
    if (milliseconds >= (uint64_t)max_ms) return max_ms;
    return (int)milliseconds;
}

void tomoe_notifications_close(struct tomoe_notifications *notifications) {
    if (!notifications) return;
    close_bus(notifications);
    free(notifications->snapshot);
    free(notifications);
}
