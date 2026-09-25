#define _GNU_SOURCE

#include "tray.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-bus.h>

#define TRAY_WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define TRAY_WATCHER_PATH "/StatusNotifierWatcher"
#define TRAY_WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define TRAY_ITEM_IFACE "org.kde.StatusNotifierItem"
#define TRAY_PROPERTIES_IFACE "org.freedesktop.DBus.Properties"
#define TRAY_DBUS "org.freedesktop.DBus"
#define TRAY_DBUS_PATH "/org/freedesktop/DBus"
#define TRAY_DEFAULT_ITEM_PATH "/StatusNotifierItem"

#define TRAY_MAX_ITEMS 64U
#define TRAY_MAX_TEXT 4096U
#define TRAY_MAX_NAME 255U
#define TRAY_MAX_DICT 64U
#define TRAY_MAX_INVALIDATED 64U
#define TRAY_MAX_PENDING 128U
#define TRAY_MAX_RETAINED_TEXT (256U * 1024U)
#define TRAY_SETUP_USEC 2000000ULL
#define TRAY_REQUEST_USEC 2000000ULL
#define TRAY_POLL_MESSAGES 64U
#define TRAY_POLL_BUDGET_USEC 4000ULL
#define TRAY_MAX_RUNTIME_PATH 4096U

struct text_buffer {
    char *data;
    size_t length;
    size_t capacity;
};

enum tray_call_kind {
    TRAY_CALL_OWNER,
    TRAY_CALL_GET_ALL,
};

struct tray_item {
    int used;
    uint64_t incarnation;
    char service[TRAY_MAX_NAME + 1U];
    char path[TRAY_MAX_TEXT + 1U];
    char owner[TRAY_MAX_NAME + 1U];
    int owner_known;
    int need_owner;
    int need_get_all;
    uint64_t signal_serial;
    uint64_t id_epoch;
    uint64_t title_epoch;
    uint64_t status_epoch;
    uint64_t icon_epoch;
    uint64_t id_read;
    uint64_t title_read;
    uint64_t status_read;
    uint64_t icon_read;
    int announced;
    char id[TRAY_MAX_TEXT + 1U];
    char title[TRAY_MAX_TEXT + 1U];
    char status[TRAY_MAX_TEXT + 1U];
    char icon_name[TRAY_MAX_TEXT + 1U];
};

struct tray_call {
    enum tray_call_kind kind;
    struct tomoe_tray *tray;
    struct tray_call *next;
    sd_bus_slot *slot;
    struct tray_item *item;
    uint64_t incarnation;
    uint64_t signal_serial;
    uint64_t request_serial;
    uint64_t id_epoch;
    uint64_t title_epoch;
    uint64_t status_epoch;
    uint64_t icon_epoch;
    uint64_t deadline_usec;
    char service[TRAY_MAX_NAME + 1U];
    char owner[TRAY_MAX_NAME + 1U];
};

struct parsed_fields {
    int id;
    int title;
    int status;
    int icon_name;
    char id_value[TRAY_MAX_TEXT + 1U];
    char title_value[TRAY_MAX_TEXT + 1U];
    char status_value[TRAY_MAX_TEXT + 1U];
    char icon_name_value[TRAY_MAX_TEXT + 1U];
};

struct invalidated_fields {
    int id;
    int title;
    int status;
    int icon_name;
};

struct tomoe_tray {
    sd_bus *bus;
    sd_bus_slot *object_slot;
    sd_bus_slot *matches[3];
    struct tray_item items[TRAY_MAX_ITEMS];
    size_t order[TRAY_MAX_ITEMS];
    size_t count;
    struct tray_call *calls;
    size_t pending_count;
    uint64_t next_incarnation;
    uint64_t next_request;
    uint64_t revision;
    int changed;
    int failed;
    int failure;
    char *snapshot;
};

int tomoe_tray_abi(void) { return 1; }

static uint64_t monotonic_usec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    if ((uint64_t)ts.tv_sec > UINT64_MAX / 1000000ULL) return UINT64_MAX;
    uint64_t seconds = (uint64_t)ts.tv_sec * 1000000ULL;
    uint64_t micros = (uint64_t)ts.tv_nsec / 1000ULL;
    return seconds > UINT64_MAX - micros ? UINT64_MAX : seconds + micros;
}

static uint64_t deadline_after(uint64_t now, uint64_t duration) {
    return now > UINT64_MAX - duration ? UINT64_MAX : now + duration;
}

static int positive_error(int result) {
    if (result < 0) return -result;
    return result ? result : EIO;
}

static int bounded_string(const char *value, size_t limit) {
    return value && strnlen(value, limit + 1U) <= limit;
}

static int valid_bus_name(const char *name) {
    return bounded_string(name, TRAY_MAX_NAME) &&
           sd_bus_service_name_is_valid(name) > 0;
}

static int valid_unique_name(const char *name) {
    return valid_bus_name(name) && name[0] == ':';
}

static int valid_object_path(const char *path) {
    return bounded_string(path, TRAY_MAX_TEXT) &&
           sd_bus_object_path_is_valid(path) > 0;
}

static void revision_bump(struct tomoe_tray *tray) {
    tray->revision++;
    if (!tray->revision) tray->revision = 1;
    tray->changed = 1;
}

static int text_buffer_reserve(struct text_buffer *buffer, size_t extra) {
    if (extra > SIZE_MAX - buffer->length - 1U) return -E2BIG;
    size_t needed = buffer->length + extra + 1U;
    if (needed <= buffer->capacity) return 0;
    size_t capacity = buffer->capacity ? buffer->capacity : 256U;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = needed;
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

static int text_buffer_literal(struct text_buffer *buffer, const char *data) {
    return text_buffer_append(buffer, data, strlen(data));
}

static int text_buffer_string(struct text_buffer *buffer, const char *data) {
    int result = text_buffer_append(buffer, "\"", 1U);
    if (result < 0) return result;
    for (const unsigned char *cursor = (const unsigned char *)data; *cursor;
         cursor++) {
        if (*cursor == '\\' || *cursor == '"') {
            result = text_buffer_append(buffer, "\\", 1U);
            if (result < 0) return result;
        }
        result = text_buffer_append(buffer, (const char *)cursor, 1U);
        if (result < 0) return result;
    }
    return text_buffer_append(buffer, "\"", 1U);
}

static size_t item_text_bytes(const struct tray_item *item) {
    return strlen(item->service) + strlen(item->path) + strlen(item->id) +
           strlen(item->title) + strlen(item->status) + strlen(item->icon_name);
}

static size_t retained_text_bytes(const struct tomoe_tray *tray,
                                  const struct tray_item *skip) {
    size_t total = 0;
    for (size_t index = 0; index < TRAY_MAX_ITEMS; index++) {
        const struct tray_item *item = &tray->items[index];
        if (!item->used || item == skip) continue;
        size_t bytes = item_text_bytes(item);
        if (bytes > SIZE_MAX - total) return SIZE_MAX;
        total += bytes;
    }
    return total;
}

static int copy_bus_text(char *destination, size_t capacity, const char *source) {
    if (!source || !bounded_string(source, capacity - 1U)) return -E2BIG;
    memcpy(destination, source, strlen(source) + 1U);
    return 0;
}

static int read_item_dict(sd_bus_message *message, struct parsed_fields *fields) {
    memset(fields, 0, sizeof(*fields));
    int result = sd_bus_message_enter_container(message, 'a', "{sv}");
    if (result <= 0) return result < 0 ? result : -EBADMSG;
    size_t entries = 0;
    for (;;) {
        result = sd_bus_message_enter_container(message, 'e', "sv");
        if (result < 0) return result;
        if (!result) break;
        if (++entries > TRAY_MAX_DICT) return -E2BIG;
        const char *key = NULL;
        result = sd_bus_message_read(message, "s", &key);
        if (result < 0 || !bounded_string(key, TRAY_MAX_TEXT)) return -EBADMSG;
        char variant_type = 0;
        const char *variant_contents = NULL;
        result = sd_bus_message_peek_type(message, &variant_type, &variant_contents);
        if (result <= 0 || variant_type != 'v' || !variant_contents)
            return result < 0 ? result : -EBADMSG;
        result = sd_bus_message_enter_container(message, 'v', variant_contents);
        if (result < 0) return result;
        char value_type = 0;
        const char *value_contents = NULL;
        result = sd_bus_message_peek_type(message, &value_type, &value_contents);
        if (result <= 0) return result < 0 ? result : -EBADMSG;
        if (!strcmp(key, "Id") && value_type == 's') {
            const char *value = NULL;
            result = sd_bus_message_read(message, "s", &value);
            if (result < 0 || copy_bus_text(fields->id_value,
                                           sizeof(fields->id_value), value) < 0)
                return -EBADMSG;
            fields->id = 1;
        } else if (!strcmp(key, "Title") && value_type == 's') {
            const char *value = NULL;
            result = sd_bus_message_read(message, "s", &value);
            if (result < 0 || copy_bus_text(fields->title_value,
                                           sizeof(fields->title_value), value) < 0)
                return -EBADMSG;
            fields->title = 1;
        } else if (!strcmp(key, "Status") && value_type == 's') {
            const char *value = NULL;
            result = sd_bus_message_read(message, "s", &value);
            if (result < 0 || copy_bus_text(fields->status_value,
                                           sizeof(fields->status_value), value) < 0)
                return -EBADMSG;
            fields->status = 1;
        } else if (!strcmp(key, "IconName") && value_type == 's') {
            const char *value = NULL;
            result = sd_bus_message_read(message, "s", &value);
            if (result < 0 || copy_bus_text(fields->icon_name_value,
                                           sizeof(fields->icon_name_value), value) < 0)
                return -EBADMSG;
            fields->icon_name = 1;
        } else {
            result = sd_bus_message_skip(message, NULL);
            if (result < 0) return result;
        }
        if (sd_bus_message_exit_container(message) < 0 ||
            sd_bus_message_exit_container(message) < 0)
            return -EBADMSG;
    }
    return sd_bus_message_exit_container(message);
}

static int read_invalidated(sd_bus_message *message,
                            struct invalidated_fields *fields) {
    memset(fields, 0, sizeof(*fields));
    int result = sd_bus_message_enter_container(message, 'a', "s");
    if (result <= 0) return result < 0 ? result : -EBADMSG;
    size_t count = 0;
    for (;;) {
        const char *key = NULL;
        result = sd_bus_message_read(message, "s", &key);
        if (result < 0) return result;
        if (!result) break;
        if (++count > TRAY_MAX_INVALIDATED || !bounded_string(key, TRAY_MAX_TEXT))
            return -E2BIG;
        if (!strcmp(key, "Id")) fields->id = 1;
        else if (!strcmp(key, "Title")) fields->title = 1;
        else if (!strcmp(key, "Status")) fields->status = 1;
        else if (!strcmp(key, "IconName")) fields->icon_name = 1;
    }
    return sd_bus_message_exit_container(message);
}

static struct tray_item *find_item(const struct tomoe_tray *tray,
                                   const char *service, const char *path) {
    for (size_t index = 0; index < TRAY_MAX_ITEMS; index++) {
        struct tray_item *item = (struct tray_item *)&tray->items[index];
        if (item->used && !strcmp(item->service, service) &&
            !strcmp(item->path, path)) return item;
    }
    return NULL;
}

static int item_matches_signal(const struct tray_item *item,
                               const char *owner, const char *path) {
    return item->used && item->owner_known && !strcmp(item->owner, owner) &&
           !strcmp(item->path, path);
}

static int reply_error(struct tomoe_tray *tray, sd_bus_message *message,
                       const char *name, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int result = sd_bus_reply_method_errorfv(message, name, format, ap);
    va_end(ap);
    if (result < 0) {
        tray->failed = 1;
        tray->failure = positive_error(result);
        return result;
    }
    return 1;
}

static int reply_invalid(struct tomoe_tray *tray, sd_bus_message *message,
                         const char *what) {
    return reply_error(tray, message,
                       "org.kde.StatusNotifierWatcher.Error.InvalidArgs",
                       "%s", what);
}

static int reply_failed(struct tomoe_tray *tray, sd_bus_message *message,
                        const char *what) {
    return reply_error(tray, message,
                       "org.kde.StatusNotifierWatcher.Error.Failed",
                       "%s", what);
}

static int reply_empty(struct tomoe_tray *tray, sd_bus_message *message) {
    int result = sd_bus_reply_method_return(message, "");
    if (result < 0) {
        tray->failed = 1;
        tray->failure = positive_error(result);
        return result;
    }
    return 1;
}

static void pending_remove(struct tomoe_tray *tray, struct tray_call *call) {
    struct tray_call **cursor = &tray->calls;
    while (*cursor && *cursor != call) cursor = &(*cursor)->next;
    if (*cursor) {
        *cursor = call->next;
        if (tray->pending_count) tray->pending_count--;
    }
}

static void cancel_item_calls(struct tomoe_tray *tray,
                              const struct tray_item *item) {
    struct tray_call *call = tray->calls;
    while (call) {
        struct tray_call *next = call->next;
        if (call->item == item) {
            pending_remove(tray, call);
            if (call->slot) {
                sd_bus_slot_unref(call->slot);
                call->slot = NULL;
            }
            free(call);
        }
        call = next;
    }
}

static void clear_item_fields(struct tray_item *item) {
    item->id[0] = '\0';
    item->title[0] = '\0';
    item->status[0] = '\0';
    item->icon_name[0] = '\0';
}

static void bump_item_epochs(struct tray_item *item) {
    item->signal_serial++;
    if (!item->signal_serial) item->signal_serial = 1;
    item->id_epoch++;
    if (!item->id_epoch) item->id_epoch = 1;
    item->title_epoch++;
    if (!item->title_epoch) item->title_epoch = 1;
    item->status_epoch++;
    if (!item->status_epoch) item->status_epoch = 1;
    item->icon_epoch++;
    if (!item->icon_epoch) item->icon_epoch = 1;
}

static void remove_order_index(struct tomoe_tray *tray, size_t index) {
    for (size_t position = 0; position < tray->count; position++) {
        if (tray->order[position] != index) continue;
        if (position + 1U < tray->count)
            memmove(&tray->order[position], &tray->order[position + 1U],
                    (tray->count - position - 1U) * sizeof(tray->order[0]));
        tray->count--;
        return;
    }
}

static int emit_item_signal(struct tomoe_tray *tray, const char *member,
                            const char *service, const char *path) {
    char registration[TRAY_MAX_NAME + TRAY_MAX_TEXT + 1U];
    int length = snprintf(registration, sizeof(registration), "%s%s", service, path);
    if (length < 0 || (size_t)length >= sizeof(registration)) return -E2BIG;
    int result = sd_bus_emit_signal(tray->bus, TRAY_WATCHER_PATH,
                                    TRAY_WATCHER_IFACE, member, "s",
                                    registration);
    if (result >= 0)
        result = sd_bus_emit_properties_changed(tray->bus, TRAY_WATCHER_PATH,
                    TRAY_WATCHER_IFACE, "RegisteredStatusNotifierItems", NULL);
    if (result < 0) {
        tray->failed = 1;
        tray->failure = positive_error(result);
        return result;
    }
    return 0;
}

static void remove_item(struct tomoe_tray *tray, struct tray_item *item,
                        int emit) {
    if (!item || !item->used) return;
    char service[TRAY_MAX_NAME + 1U];
    char path[TRAY_MAX_TEXT + 1U];
    memcpy(service, item->service, sizeof(service));
    memcpy(path, item->path, sizeof(path));
    int announced = item->announced;
    size_t index = (size_t)(item - tray->items);
    cancel_item_calls(tray, item);
    remove_order_index(tray, index);
    memset(item, 0, sizeof(*item));
    if (emit && announced) {
        revision_bump(tray);
        if (emit_item_signal(tray, "StatusNotifierItemUnregistered", service,
                             path) < 0)
            return;
    }
}

static int attach_owner(struct tomoe_tray *tray, struct tray_item *item,
                        const char *owner) {
    if (!item || !item->used || !valid_unique_name(owner)) return -EINVAL;
    int had_owner = item->owner_known;
    if (had_owner && !strcmp(item->owner, owner)) return 0;
    if (had_owner) cancel_item_calls(tray, item);
    memcpy(item->owner, owner, strlen(owner) + 1U);
    item->owner_known = 1;
    item->need_owner = 0;
    item->incarnation = ++tray->next_incarnation;
    if (!item->incarnation) item->incarnation = ++tray->next_incarnation;
    bump_item_epochs(item);
    item->id_read = item->title_read = item->status_read = item->icon_read = 0;
    clear_item_fields(item);
    item->need_get_all = 1;
    revision_bump(tray);
    if (!had_owner) {
        item->announced = 1;
        if (emit_item_signal(tray, "StatusNotifierItemRegistered",
                             item->service, item->path) < 0)
            return -EIO;
    }
    return 0;
}

static void transfer_owner(struct tomoe_tray *tray, struct tray_item *item,
                           const char *owner) {
    if (!item || !item->used || !valid_unique_name(owner)) return;
    (void)attach_owner(tray, item, owner);
}

static int append_order(struct tomoe_tray *tray, size_t index) {
    if (tray->count >= TRAY_MAX_ITEMS) return -ENOSPC;
    tray->order[tray->count++] = index;
    return 0;
}

static int pending_duplicate(const struct tomoe_tray *tray,
                             const struct tray_item *item,
                             enum tray_call_kind kind,
                             uint64_t signal_serial) {
    for (const struct tray_call *call = tray->calls; call; call = call->next)
        if (call->item == item && call->kind == kind &&
            call->incarnation == item->incarnation &&
            (kind == TRAY_CALL_OWNER || call->signal_serial == signal_serial))
            return 1;
    return 0;
}

static int async_reply(sd_bus_message *message, void *userdata,
                       sd_bus_error *ret_error);

static int queue_call(struct tomoe_tray *tray, struct tray_call *call,
                      const char *destination, const char *path,
                      const char *interface, const char *member,
                      const char *types, ...) {
    if (!tray->bus || tray->failed || tray->pending_count >= TRAY_MAX_PENDING) {
        free(call);
        return -ENOSPC;
    }
    call->tray = tray;
    call->request_serial = ++tray->next_request;
    if (!call->request_serial) call->request_serial = ++tray->next_request;
    call->deadline_usec = deadline_after(monotonic_usec(), TRAY_REQUEST_USEC);
    call->next = tray->calls;
    tray->calls = call;
    tray->pending_count++;
    va_list ap;
    va_start(ap, types);
    int result = sd_bus_call_method_asyncv(
        tray->bus, &call->slot, destination, path, interface, member,
        async_reply, call, types, ap);
    va_end(ap);
    if (result < 0) {
        pending_remove(tray, call);
        if (call->slot) sd_bus_slot_unref(call->slot);
        free(call);
        return result;
    }
    return 0;
}

static int queue_owner(struct tomoe_tray *tray, struct tray_item *item) {
    if (!item || !item->used || !item->need_owner ||
        pending_duplicate(tray, item, TRAY_CALL_OWNER, 0) ||
        tray->pending_count >= TRAY_MAX_PENDING)
        return -ENOSPC;
    struct tray_call *call = calloc(1, sizeof(*call));
    if (!call) return -ENOMEM;
    call->kind = TRAY_CALL_OWNER;
    call->item = item;
    call->incarnation = item->incarnation;
    memcpy(call->service, item->service, sizeof(call->service));
    int result = queue_call(tray, call, TRAY_DBUS, TRAY_DBUS_PATH,
                            TRAY_DBUS, "GetNameOwner", "s", item->service);
    if (result >= 0) item->need_owner = 0;
    return result;
}

static int queue_get_all(struct tomoe_tray *tray, struct tray_item *item) {
    if (!item || !item->used || !item->owner_known || !item->need_get_all ||
        pending_duplicate(tray, item, TRAY_CALL_GET_ALL, item->signal_serial) ||
        tray->pending_count >= TRAY_MAX_PENDING)
        return -ENOSPC;
    struct tray_call *call = calloc(1, sizeof(*call));
    if (!call) return -ENOMEM;
    call->kind = TRAY_CALL_GET_ALL;
    call->item = item;
    call->incarnation = item->incarnation;
    call->signal_serial = item->signal_serial;
    call->id_epoch = item->id_epoch;
    call->title_epoch = item->title_epoch;
    call->status_epoch = item->status_epoch;
    call->icon_epoch = item->icon_epoch;
    memcpy(call->owner, item->owner, sizeof(call->owner));
    int result = queue_call(tray, call, item->owner, item->path,
                            TRAY_PROPERTIES_IFACE, "GetAll", "s",
                            TRAY_ITEM_IFACE);
    if (result >= 0) item->need_get_all = 0;
    return result;
}

static void service_needs(struct tomoe_tray *tray) {
    for (size_t position = 0; position < tray->count; position++) {
        struct tray_item *item = &tray->items[tray->order[position]];
        if (!item->used) continue;
        if (item->need_owner) (void)queue_owner(tray, item);
        if (item->owner_known && item->need_get_all) (void)queue_get_all(tray, item);
        if (tray->pending_count >= TRAY_MAX_PENDING) break;
    }
}

static void mark_failed(struct tomoe_tray *tray, int result) {
    if (tray->failed) return;
    tray->failed = 1;
    tray->failure = positive_error(result);
    for (size_t index = 0; index < TRAY_MAX_ITEMS; index++)
        tray->items[index].used = 0;
    tray->count = 0;
    revision_bump(tray);
}

static void close_bus(struct tomoe_tray *tray) {
    if (!tray) return;
    for (size_t index = 0; index < sizeof(tray->matches) / sizeof(tray->matches[0]); index++) {
        if (tray->matches[index]) {
            sd_bus_slot_unref(tray->matches[index]);
            tray->matches[index] = NULL;
        }
    }
    if (tray->object_slot) {
        sd_bus_slot_unref(tray->object_slot);
        tray->object_slot = NULL;
    }
    struct tray_call *call = tray->calls;
    while (call) {
        struct tray_call *next = call->next;
        if (call->slot) sd_bus_slot_unref(call->slot);
        free(call);
        call = next;
    }
    tray->calls = NULL;
    tray->pending_count = 0;
    if (tray->bus) {
        sd_bus_close(tray->bus);
        sd_bus_unref(tray->bus);
        tray->bus = NULL;
    }
}

static int reply_is_error(sd_bus_message *message) {
    return sd_bus_message_is_method_error(message, NULL) > 0;
}

static void apply_fields(struct tomoe_tray *tray, struct tray_item *item,
                         const struct parsed_fields *fields,
                         const struct invalidated_fields *invalidated,
                         const struct tray_call *call) {
    struct tray_item candidate = *item;
    int accept_id = fields && fields->id &&
        (!call || (call->id_epoch == item->id_epoch &&
                   item->id_read < call->request_serial));
    int accept_title = fields && fields->title &&
        (!call || (call->title_epoch == item->title_epoch &&
                   item->title_read < call->request_serial));
    int accept_status = fields && fields->status &&
        (!call || (call->status_epoch == item->status_epoch &&
                   item->status_read < call->request_serial));
    int accept_icon = fields && fields->icon_name &&
        (!call || (call->icon_epoch == item->icon_epoch &&
                   item->icon_read < call->request_serial));
    if (accept_id) memcpy(candidate.id, fields->id_value, sizeof(candidate.id));
    if (accept_title) memcpy(candidate.title, fields->title_value, sizeof(candidate.title));
    if (accept_status) memcpy(candidate.status, fields->status_value, sizeof(candidate.status));
    if (accept_icon) memcpy(candidate.icon_name, fields->icon_name_value,
                            sizeof(candidate.icon_name));
    if (invalidated) {
        if (invalidated->id) candidate.id[0] = '\0';
        if (invalidated->title) candidate.title[0] = '\0';
        if (invalidated->status) candidate.status[0] = '\0';
        if (invalidated->icon_name) candidate.icon_name[0] = '\0';
    }
    size_t other = retained_text_bytes(tray, item);
    if (other > TRAY_MAX_RETAINED_TEXT ||
        item_text_bytes(&candidate) > TRAY_MAX_RETAINED_TEXT - other)
        return;
    int changed = 0;
    if (accept_id) {
        if (strcmp(item->id, fields->id_value)) {
            memcpy(item->id, fields->id_value, sizeof(item->id)); changed = 1;
        }
    }
    if (accept_title) {
        if (strcmp(item->title, fields->title_value)) {
            memcpy(item->title, fields->title_value, sizeof(item->title)); changed = 1;
        }
    }
    if (accept_status) {
        if (strcmp(item->status, fields->status_value)) {
            memcpy(item->status, fields->status_value, sizeof(item->status)); changed = 1;
        }
    }
    if (accept_icon) {
        if (strcmp(item->icon_name, fields->icon_name_value)) {
            memcpy(item->icon_name, fields->icon_name_value, sizeof(item->icon_name)); changed = 1;
        }
    }
    if (invalidated) {
        if (invalidated->id && item->id[0]) { item->id[0] = '\0'; changed = 1; }
        if (invalidated->title && item->title[0]) { item->title[0] = '\0'; changed = 1; }
        if (invalidated->status && item->status[0]) { item->status[0] = '\0'; changed = 1; }
        if (invalidated->icon_name && item->icon_name[0]) { item->icon_name[0] = '\0'; changed = 1; }
    }
    if (call) {
        if (accept_id) item->id_read = call->request_serial;
        if (accept_title) item->title_read = call->request_serial;
        if (accept_status) item->status_read = call->request_serial;
        if (accept_icon) item->icon_read = call->request_serial;
    }
    if (changed) revision_bump(tray);
}

static void handle_owner_reply(struct tray_call *call, sd_bus_message *message) {
    struct tomoe_tray *tray = call->tray;
    struct tray_item *item = call->item;
    if (!item || !item->used || item->incarnation != call->incarnation ||
        strcmp(item->service, call->service)) return;
    if (reply_is_error(message)) {
        remove_item(tray, item, 1);
        return;
    }
    const char *owner = NULL;
    if (sd_bus_message_read(message, "s", &owner) < 0 ||
        !valid_unique_name(owner)) {
        remove_item(tray, item, 1);
        return;
    }
    (void)attach_owner(tray, item, owner);
}

static void handle_get_all_reply(struct tray_call *call,
                                 sd_bus_message *message) {
    struct tomoe_tray *tray = call->tray;
    struct tray_item *item = call->item;
    if (!item || !item->used || item->incarnation != call->incarnation ||
        !item->owner_known || strcmp(item->owner, call->owner))
        return;
    if (reply_is_error(message)) {
        return;
    }
    struct parsed_fields fields;
    if (read_item_dict(message, &fields) < 0) {
        return;
    }
    apply_fields(tray, item, &fields, NULL, call);
}

static int async_reply(sd_bus_message *message, void *userdata,
                       sd_bus_error *ret_error) {
    struct tray_call *call = userdata;
    if (!call || !call->tray) return 1;
    struct tomoe_tray *tray = call->tray;
    pending_remove(tray, call);
    (void)ret_error;
    if (call->kind == TRAY_CALL_OWNER)
        handle_owner_reply(call, message);
    else
        handle_get_all_reply(call, message);
    if (call->slot) {
        sd_bus_slot_unref(call->slot);
        call->slot = NULL;
    }
    free(call);
    return 1;
}

static int watcher_properties(sd_bus *bus, const char *path,
                              const char *interface, const char *property,
                              sd_bus_message *reply, void *userdata,
                              sd_bus_error *ret_error) {
    struct tomoe_tray *tray = userdata;
    (void)bus; (void)path; (void)interface; (void)ret_error;
    if (!strcmp(property, "IsStatusNotifierHostRegistered"))
        return sd_bus_message_append(reply, "b", 1);
    if (!strcmp(property, "ProtocolVersion"))
        return sd_bus_message_append(reply, "i", 0);
    if (!strcmp(property, "RegisteredStatusNotifierItems")) {
        int result = sd_bus_message_open_container(reply, 'a', "s");
        if (result < 0) return result;
        for (size_t position = 0; position < tray->count; position++) {
            const struct tray_item *item = &tray->items[tray->order[position]];
            if (!item->used || !item->owner_known) continue;
            char registration[TRAY_MAX_NAME + TRAY_MAX_TEXT + 1U];
            int length = snprintf(registration, sizeof(registration), "%s%s",
                                  item->service, item->path);
            if (length < 0 || (size_t)length >= sizeof(registration)) return -E2BIG;
            result = sd_bus_message_append(reply, "s", registration);
            if (result < 0) return result;
        }
        return sd_bus_message_close_container(reply);
    }
    return -ENOENT;
}

static int method_register_host(sd_bus_message *message, void *userdata,
                                sd_bus_error *ret_error) {
    struct tomoe_tray *tray = userdata;
    (void)ret_error;
    const char *host = NULL;
    if (sd_bus_message_read(message, "s", &host) < 0 ||
        sd_bus_message_at_end(message, 1) <= 0)
        return reply_invalid(tray, message, "invalid host registration");
    (void)host;
    int result = sd_bus_emit_signal(tray->bus, TRAY_WATCHER_PATH,
                                    TRAY_WATCHER_IFACE,
                                    "StatusNotifierHostRegistered", "");
    if (result < 0) {
        mark_failed(tray, result);
        return result;
    }
    return reply_empty(tray, message);
}

static int parse_registration(const char *argument, const char *sender,
                              char *service, size_t service_capacity,
                              char *path, size_t path_capacity) {
    if (!argument || !bounded_string(argument, TRAY_MAX_NAME + TRAY_MAX_TEXT) || !sender ||
        !valid_unique_name(sender)) return -EINVAL;
    const char *service_start = argument;
    const char *path_start = NULL;
    int default_path = 0;
    int sender_service = 0;
    if (!*argument || argument[0] == '/') {
        service_start = sender;
        path_start = *argument ? argument : TRAY_DEFAULT_ITEM_PATH;
        default_path = !*argument;
        sender_service = 1;
    } else {
        path_start = strchr(argument, '/');
        if (!path_start) {
            path_start = TRAY_DEFAULT_ITEM_PATH;
            default_path = 1;
        }
    }
    size_t service_length = default_path || sender_service
        ? strlen(service_start) : (size_t)(path_start - service_start);
    if (!service_length || service_length + 1U > service_capacity) return -EINVAL;
    memcpy(service, service_start, service_length);
    service[service_length] = '\0';
    if (!valid_bus_name(service)) return -EINVAL;
    if (strlen(path_start) + 1U > path_capacity ||
        !valid_object_path(path_start)) return -EINVAL;
    memcpy(path, path_start, strlen(path_start) + 1U);
    return 0;
}

static int method_register_item(sd_bus_message *message, void *userdata,
                                sd_bus_error *ret_error) {
    struct tomoe_tray *tray = userdata;
    (void)ret_error;
    const char *argument = NULL;
    if (sd_bus_message_read(message, "s", &argument) < 0 ||
        sd_bus_message_at_end(message, 1) <= 0)
        return reply_invalid(tray, message, "invalid item registration");
    const char *sender = sd_bus_message_get_sender(message);
    char service[TRAY_MAX_NAME + 1U];
    char path[TRAY_MAX_TEXT + 1U];
    if (parse_registration(argument, sender, service, sizeof(service), path,
                           sizeof(path)) < 0)
        return reply_invalid(tray, message, "invalid service or object path");
    struct tray_item *existing = find_item(tray, service, path);
    if (existing) return reply_empty(tray, message);
    if (tray->count >= TRAY_MAX_ITEMS) return reply_failed(tray, message, "item limit reached");
    struct tray_item *item = NULL;
    size_t index = 0;
    for (; index < TRAY_MAX_ITEMS; index++) {
        if (!tray->items[index].used) { item = &tray->items[index]; break; }
    }
    if (!item) return reply_failed(tray, message, "item limit reached");
    memset(item, 0, sizeof(*item));
    item->used = 1;
    item->incarnation = ++tray->next_incarnation;
    if (!item->incarnation) item->incarnation = ++tray->next_incarnation;
    memcpy(item->service, service, strlen(service) + 1U);
    memcpy(item->path, path, strlen(path) + 1U);
    if (retained_text_bytes(tray, NULL) > TRAY_MAX_RETAINED_TEXT) {
        memset(item, 0, sizeof(*item));
        return reply_failed(tray, message, "item text limit reached");
    }
    item->need_owner = 1;
    if (append_order(tray, index) < 0) {
        memset(item, 0, sizeof(*item));
        return reply_failed(tray, message, "item limit reached");
    }
    return reply_empty(tray, message);
}

static const sd_bus_vtable tray_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", method_register_item, 0),
    SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", method_register_host, 0),
    SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", watcher_properties,
                    0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ProtocolVersion", "i", watcher_properties,
                    0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", watcher_properties,
                    0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierHostRegistered", "", 0),
    SD_BUS_SIGNAL("StatusNotifierHostUnregistered", "", 0),
    SD_BUS_VTABLE_END,
};

static int name_owner_signal(sd_bus_message *message, void *userdata,
                             sd_bus_error *ret_error) {
    struct tomoe_tray *tray = userdata;
    (void)ret_error;
    const char *sender = sd_bus_message_get_sender(message);
    const char *path = sd_bus_message_get_path(message);
    const char *interface = sd_bus_message_get_interface(message);
    if (!sender || strcmp(sender, TRAY_DBUS) || !path ||
        strcmp(path, TRAY_DBUS_PATH) || !interface ||
        strcmp(interface, TRAY_DBUS)) return 1;
    const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
    if (sd_bus_message_read(message, "sss", &name, &old_owner, &new_owner) < 0 ||
        !valid_bus_name(name) ||
        (old_owner && *old_owner && !valid_unique_name(old_owner)) ||
        (new_owner && *new_owner && !valid_unique_name(new_owner))) return 1;
    for (size_t position = 0; position < tray->count;) {
        struct tray_item *item = &tray->items[tray->order[position]];
        int match_service = item->used && !strcmp(item->service, name);
        int match_owner = item->used && item->owner_known &&
                          !strcmp(item->owner, name);
        if (!match_service && !match_owner) { position++; continue; }
        if (new_owner && *new_owner && match_service) {
            if (!old_owner || !*old_owner || !item->owner_known ||
                !strcmp(item->owner, old_owner)) {
                transfer_owner(tray, item, new_owner);
            }
            position++;
            continue;
        }
        if ((!new_owner || !*new_owner) &&
            ((match_service && (!old_owner || !*old_owner ||
                                !item->owner_known || !strcmp(item->owner, old_owner))) ||
             (match_owner && old_owner && !strcmp(item->owner, old_owner)))) {
            remove_item(tray, item, 1);
            continue;
        }
        position++;
    }
    return 1;
}

static void queue_signal_refresh(struct tomoe_tray *tray, struct tray_item *item,
                                 const char *member) {
    (void)tray;
    if (!item || !item->used || !item->owner_known) return;
    item->signal_serial++;
    if (!item->signal_serial) item->signal_serial = 1;
    if (!strcmp(member, "NewIcon")) {
        item->icon_epoch++;
        if (!item->icon_epoch) item->icon_epoch = 1;
    } else if (!strcmp(member, "NewTitle")) {
        item->title_epoch++;
        if (!item->title_epoch) item->title_epoch = 1;
    } else if (!strcmp(member, "NewStatus")) {
        item->status_epoch++;
        if (!item->status_epoch) item->status_epoch = 1;
    }
    item->need_get_all = 1;
}

static int item_signal(sd_bus_message *message, void *userdata,
                       sd_bus_error *ret_error) {
    struct tomoe_tray *tray = userdata;
    (void)ret_error;
    const char *interface = sd_bus_message_get_interface(message);
    const char *sender = sd_bus_message_get_sender(message);
    const char *path = sd_bus_message_get_path(message);
    const char *member = sd_bus_message_get_member(message);
    if (!interface || strcmp(interface, TRAY_ITEM_IFACE) || !sender ||
        !path || !member) return 1;
    if (!valid_unique_name(sender) || !valid_object_path(path)) return 1;
    if (!strcmp(member, "NewIcon") || !strcmp(member, "NewTitle") ||
        !strcmp(member, "NewToolTip")) {
        if (sd_bus_message_at_end(message, 1) <= 0) return 1;
    } else if (!strcmp(member, "NewStatus")) {
        const char *status = NULL;
        if (sd_bus_message_read(message, "s", &status) < 0 ||
            sd_bus_message_at_end(message, 1) <= 0) return 1;
    } else return 1;
    for (size_t index = 0; index < TRAY_MAX_ITEMS; index++) {
        struct tray_item *item = &tray->items[index];
        if (!item_matches_signal(item, sender, path)) continue;
        if (!strcmp(member, "NewIcon") || !strcmp(member, "NewTitle") ||
            !strcmp(member, "NewToolTip")) {
            queue_signal_refresh(tray, item, member);
            continue;
        }
        if (!strcmp(member, "NewStatus")) {
            queue_signal_refresh(tray, item, member);
        }
    }
    return 1;
}

static int properties_signal(sd_bus_message *message, void *userdata,
                             sd_bus_error *ret_error) {
    struct tomoe_tray *tray = userdata;
    (void)ret_error;
    const char *interface = sd_bus_message_get_interface(message);
    const char *sender = sd_bus_message_get_sender(message);
    const char *path = sd_bus_message_get_path(message);
    if (!interface || strcmp(interface, TRAY_PROPERTIES_IFACE) || !sender ||
        !path) return 1;
    if (!valid_unique_name(sender) || !valid_object_path(path)) return 1;
    const char *changed_interface = NULL;
    if (sd_bus_message_read(message, "s", &changed_interface) < 0 ||
        !changed_interface || strcmp(changed_interface, TRAY_ITEM_IFACE)) return 1;
    struct parsed_fields fields;
    struct invalidated_fields invalidated;
    if (read_item_dict(message, &fields) < 0 ||
        read_invalidated(message, &invalidated) < 0) return 1;
    if (!fields.id && !fields.title && !fields.status && !fields.icon_name &&
        !invalidated.id && !invalidated.title && !invalidated.status &&
        !invalidated.icon_name) return 1;
    for (size_t index = 0; index < TRAY_MAX_ITEMS; index++) {
        struct tray_item *item = &tray->items[index];
        if (!item_matches_signal(item, sender, path)) continue;
        if (fields.id) { item->id_epoch++; if (!item->id_epoch) item->id_epoch = 1; }
        if (fields.title) { item->title_epoch++; if (!item->title_epoch) item->title_epoch = 1; }
        if (fields.status) { item->status_epoch++; if (!item->status_epoch) item->status_epoch = 1; }
        if (fields.icon_name) { item->icon_epoch++; if (!item->icon_epoch) item->icon_epoch = 1; }
        if (invalidated.id) { item->id_epoch++; if (!item->id_epoch) item->id_epoch = 1; }
        if (invalidated.title) { item->title_epoch++; if (!item->title_epoch) item->title_epoch = 1; }
        if (invalidated.status) { item->status_epoch++; if (!item->status_epoch) item->status_epoch = 1; }
        if (invalidated.icon_name) { item->icon_epoch++; if (!item->icon_epoch) item->icon_epoch = 1; }
        item->signal_serial++;
        if (!item->signal_serial) item->signal_serial = 1;
        apply_fields(tray, item, &fields, &invalidated, NULL);
        item->need_get_all = 1;
    }
    return 1;
}

static int make_fallback_address(char *destination, size_t capacity,
                                 const char *runtime) {
    static const char hex[] = "0123456789ABCDEF";
    size_t length = strnlen(runtime, TRAY_MAX_RUNTIME_PATH + 1U);
    if (length > TRAY_MAX_RUNTIME_PATH) return -ENAMETOOLONG;
    const char prefix[] = "unix:path=";
    if (length > (capacity - (sizeof(prefix) - 1U)) / 3U) return -ENAMETOOLONG;
    memcpy(destination, prefix, sizeof(prefix) - 1U);
    size_t offset = sizeof(prefix) - 1U;
    for (size_t index = 0; index < length; index++) {
        unsigned char value = (unsigned char)runtime[index];
        destination[offset++] = '%';
        destination[offset++] = hex[value >> 4U];
        destination[offset++] = hex[value & 0x0fU];
    }
    destination[offset] = '\0';
    return 0;
}

static int setup_ready(sd_bus *bus, uint64_t deadline) {
    for (;;) {
        if (sd_bus_is_ready(bus) > 0) return 0;
        uint64_t now = monotonic_usec();
        if (now >= deadline) return -ETIMEDOUT;
        int result = sd_bus_process(bus, NULL);
        if (result < 0 && result != -EAGAIN && result != -EINTR) return result;
        if (sd_bus_is_ready(bus) > 0) return 0;
        if (!result) {
            now = monotonic_usec();
            if (now >= deadline) return -ETIMEDOUT;
            result = sd_bus_wait(bus, deadline - now);
            if (result < 0 && result != -EINTR) return result;
        }
    }
}

static int remaining_timeout(sd_bus *bus, uint64_t deadline) {
    uint64_t now = monotonic_usec();
    if (now >= deadline) return -ETIMEDOUT;
    return sd_bus_set_method_call_timeout(bus, deadline - now);
}

struct tomoe_tray *tomoe_tray_open(int *error) {
    if (!error) return NULL;
    *error = EINVAL;
    struct tomoe_tray *tray = calloc(1, sizeof(*tray));
    if (!tray) { *error = ENOMEM; return NULL; }
    sd_bus *bus = NULL;
    int result = sd_bus_new(&bus);
    if (result < 0) goto failed;
    result = sd_bus_set_bus_client(bus, 1);
    if (result < 0) goto failed;
    result = sd_bus_set_method_call_timeout(bus, TRAY_SETUP_USEC);
    if (result < 0) goto failed;
    const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
    char address_buffer[TRAY_MAX_RUNTIME_PATH * 3U + 32U];
    if (!address || !*address) {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        if (!runtime || !*runtime) { result = -ENOMEDIUM; goto failed; }
        size_t length = strnlen(runtime, TRAY_MAX_RUNTIME_PATH + 1U);
        if (length > TRAY_MAX_RUNTIME_PATH || length > SIZE_MAX - 5U) {
            result = -ENAMETOOLONG; goto failed;
        }
        char runtime_bus[TRAY_MAX_RUNTIME_PATH + 5U];
        memcpy(runtime_bus, runtime, length);
        memcpy(runtime_bus + length, "/bus", 5U);
        result = make_fallback_address(address_buffer, sizeof(address_buffer),
                                       runtime_bus);
        if (result < 0) goto failed;
        address = address_buffer;
    }
    result = sd_bus_set_address(bus, address);
    if (result < 0) goto failed;
    result = sd_bus_start(bus);
    if (result < 0) goto failed;
    uint64_t deadline = deadline_after(monotonic_usec(), TRAY_SETUP_USEC);
    result = setup_ready(bus, deadline);
    if (result < 0) goto failed;
    tray->bus = bus;
    bus = NULL;
    result = remaining_timeout(tray->bus, deadline);
    if (result < 0) goto failed_tray;
    result = sd_bus_add_match(tray->bus, &tray->matches[0],
        "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
        name_owner_signal, tray);
    if (result < 0) goto failed_tray;
    result = remaining_timeout(tray->bus, deadline);
    if (result < 0) goto failed_tray;
    result = sd_bus_add_match(tray->bus, &tray->matches[1],
        "type='signal',interface='org.kde.StatusNotifierItem'",
        item_signal, tray);
    if (result < 0) goto failed_tray;
    result = remaining_timeout(tray->bus, deadline);
    if (result < 0) goto failed_tray;
    result = sd_bus_add_match(tray->bus, &tray->matches[2],
        "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
        properties_signal, tray);
    if (result < 0) goto failed_tray;
    result = remaining_timeout(tray->bus, deadline);
    if (result < 0) goto failed_tray;
    result = sd_bus_add_object_vtable(tray->bus, &tray->object_slot,
                                      TRAY_WATCHER_PATH, TRAY_WATCHER_IFACE,
                                      tray_vtable, tray);
    if (result < 0) goto failed_tray;
    result = remaining_timeout(tray->bus, deadline);
    if (result < 0) goto failed_tray;
    result = sd_bus_request_name(tray->bus, TRAY_WATCHER_NAME, 0);
    if (result <= 0) {
        if (!result) result = -EEXIST;
        goto failed_tray;
    }
    result = sd_bus_set_method_call_timeout(tray->bus, TRAY_REQUEST_USEC);
    if (result < 0) goto failed_tray;
    tray->revision = 1;
    *error = 0;
    return tray;

failed_tray:
    close_bus(tray);
    goto failed_allocated;
failed:
    if (bus) { sd_bus_close(bus); sd_bus_unref(bus); }
failed_allocated:
    free(tray);
    *error = positive_error(result);
    return NULL;
}

static void expire_calls(struct tomoe_tray *tray, uint64_t now) {
    struct tray_call *call = tray->calls;
    while (call) {
        struct tray_call *next = call->next;
        if (call->deadline_usec <= now) {
            struct tray_item *item = call->item;
            pending_remove(tray, call);
            if (call->slot) { sd_bus_slot_unref(call->slot); call->slot = NULL; }
            if (call->kind == TRAY_CALL_OWNER && item && item->used &&
                item->incarnation == call->incarnation && !item->owner_known)
                remove_item(tray, item, 0);
            free(call);
            call = tray->calls;
            continue;
        }
        call = next;
    }
}

int tomoe_tray_poll(struct tomoe_tray *tray) {
    if (!tray) return -EINVAL;
    if (tray->failed) {
        int failure = tray->failure;
        close_bus(tray);
        return -failure;
    }
    tray->changed = 0;
    service_needs(tray);
    uint64_t start = monotonic_usec();
    size_t processed = 0;
    while (tray->bus && processed < TRAY_POLL_MESSAGES) {
        uint64_t now = monotonic_usec();
        if (now >= start && now - start >= TRAY_POLL_BUDGET_USEC) break;
        int result = sd_bus_process(tray->bus, NULL);
        if (result < 0) {
            if (result == -EAGAIN || result == -EINTR) break;
            mark_failed(tray, result);
            break;
        }
        if (!result) break;
        processed++;
        if (tray->failed) break;
        service_needs(tray);
        expire_calls(tray, monotonic_usec());
    }
    expire_calls(tray, monotonic_usec());
    service_needs(tray);
    if (tray->failed) {
        int failure = tray->failure;
        close_bus(tray);
        return -failure;
    }
    return tray->changed ? 1 : 0;
}

static int build_snapshot(struct tomoe_tray *tray, char **result_out) {
    struct text_buffer buffer = {0};
    int result = text_buffer_literal(&buffer, "(:items (");
    if (result < 0) goto failed;
    int emitted = 0;
    for (size_t position = 0; position < tray->count; position++) {
        const struct tray_item *item = &tray->items[tray->order[position]];
        if (!item->used || !item->owner_known) continue;
        if (emitted++ && (result = text_buffer_literal(&buffer, " ")) < 0) goto failed;
        result = text_buffer_literal(&buffer, "(:service "); if (result < 0) goto failed;
        result = text_buffer_string(&buffer, item->service); if (result < 0) goto failed;
        result = text_buffer_literal(&buffer, " :path "); if (result < 0) goto failed;
        result = text_buffer_string(&buffer, item->path); if (result < 0) goto failed;
        result = text_buffer_literal(&buffer, " :id "); if (result < 0) goto failed;
        result = text_buffer_string(&buffer, item->id); if (result < 0) goto failed;
        result = text_buffer_literal(&buffer, " :title "); if (result < 0) goto failed;
        result = text_buffer_string(&buffer, item->title); if (result < 0) goto failed;
        result = text_buffer_literal(&buffer, " :status "); if (result < 0) goto failed;
        result = text_buffer_string(&buffer, item->status); if (result < 0) goto failed;
        result = text_buffer_literal(&buffer, " :icon-name "); if (result < 0) goto failed;
        result = text_buffer_string(&buffer, item->icon_name); if (result < 0) goto failed;
        result = text_buffer_literal(&buffer, ")"); if (result < 0) goto failed;
    }
    result = text_buffer_literal(&buffer, "))");
    if (result < 0) goto failed;
    *result_out = buffer.data;
    return 0;
failed:
    free(buffer.data);
    return result;
}

const char *tomoe_tray_snapshot(struct tomoe_tray *tray) {
    if (!tray) return NULL;
    char *snapshot = NULL;
    if (build_snapshot(tray, &snapshot) < 0) return NULL;
    free(tray->snapshot);
    tray->snapshot = snapshot;
    return snapshot;
}

uint64_t tomoe_tray_revision(const struct tomoe_tray *tray) {
    return tray ? tray->revision : 0;
}

int tomoe_tray_timeout(const struct tomoe_tray *tray, int max_ms) {
    if (!tray) return 0;
    if (max_ms < 0) max_ms = 0;
    uint64_t best = UINT64_MAX;
    for (const struct tray_call *call = tray->calls; call; call = call->next)
        if (call->deadline_usec < best) best = call->deadline_usec;
    if (best == UINT64_MAX) return max_ms;
    uint64_t now = monotonic_usec();
    if (best <= now) return 0;
    uint64_t milliseconds = (best - now + 999ULL) / 1000ULL;
    if (milliseconds >= (uint64_t)max_ms) return max_ms;
    return (int)milliseconds;
}

void tomoe_tray_close(struct tomoe_tray *tray) {
    if (!tray) return;
    close_bus(tray);
    free(tray->snapshot);
    free(tray);
}
