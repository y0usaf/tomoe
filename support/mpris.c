#define _GNU_SOURCE

#include "mpris.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-bus.h>

#define MPRIS_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_DBUS "org.freedesktop.DBus"
#define MPRIS_DBUS_PATH "/org/freedesktop/DBus"
#define MPRIS_PROPERTIES "org.freedesktop.DBus.Properties"
#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER "org.mpris.MediaPlayer2.Player"

#define MPRIS_MAX_PLAYERS 64U
#define MPRIS_MAX_ALIASES 128U
#define MPRIS_MAX_ALIASES_PER_PLAYER 16U
#define MPRIS_MAX_PENDING 256U
#define MPRIS_MAX_NAME 255U
#define MPRIS_MAX_TEXT 4096U
#define MPRIS_MAX_DICT 64U
#define MPRIS_MAX_ARTISTS 32U
#define MPRIS_SETUP_USEC 2000000ULL
#define MPRIS_REQUEST_USEC 2000000ULL
#define MPRIS_POLL_MESSAGES 64U
#define MPRIS_POLL_BUDGET_USEC 4000ULL
#define MPRIS_MAX_RUNTIME_PATH 4096U

enum pending_kind {
    PENDING_LIST_NAMES,
    PENDING_GET_OWNER,
    PENDING_GET_ALL,
    PENDING_GET_POSITION,
};

struct text_buffer {
    char *data;
    size_t length;
    size_t capacity;
};

struct mpris_values {
    int status;
    char status_text[MPRIS_MAX_TEXT + 1U];
    int volume;
    double volume_value;
    int metadata;
    char title[MPRIS_MAX_TEXT + 1U];
    char artist[MPRIS_MAX_TEXT + 1U];
    char album[MPRIS_MAX_TEXT + 1U];
    char art_url[MPRIS_MAX_TEXT + 1U];
    uint64_t length;
    int position;
    int64_t position_value;
};

struct mpris_alias {
    int used;
    int need_owner;
    char name[MPRIS_MAX_NAME + 1U];
    char owner[MPRIS_MAX_NAME + 1U];
    uint64_t epoch;
    uint64_t order;
};

struct mpris_player {
    int used;
    char owner[MPRIS_MAX_NAME + 1U];
    uint64_t generation;
    uint64_t activity;
    uint64_t signal_serial;
    uint64_t status_epoch;
    uint64_t volume_epoch;
    uint64_t metadata_epoch;
    uint64_t position_epoch;
    uint64_t status_read;
    uint64_t volume_read;
    uint64_t metadata_read;
    uint64_t position_request;
    int need_all;
    int need_position;
    char player_name[MPRIS_MAX_NAME + 1U];
    size_t aliases;
    char status[MPRIS_MAX_TEXT + 1U];
    char title[MPRIS_MAX_TEXT + 1U];
    char artist[MPRIS_MAX_TEXT + 1U];
    char album[MPRIS_MAX_TEXT + 1U];
    char art_url[MPRIS_MAX_TEXT + 1U];
    uint64_t length;
    int64_t position;
    double volume;
};

struct mpris_call {
    enum pending_kind kind;
    struct tomoe_mpris *mpris;
    struct mpris_call *next;
    uint64_t request;
    char alias[MPRIS_MAX_NAME + 1U];
    char owner[MPRIS_MAX_NAME + 1U];
    uint64_t alias_epoch;
    uint64_t generation;
    uint64_t signal_serial;
    uint64_t status_epoch;
    uint64_t volume_epoch;
    uint64_t metadata_epoch;
    uint64_t position_epoch;
    uint64_t deadline_usec;
};

struct public_values {
    int available;
    char player_name[MPRIS_MAX_NAME + 1U];
    char status[MPRIS_MAX_TEXT + 1U];
    char title[MPRIS_MAX_TEXT + 1U];
    char artist[MPRIS_MAX_TEXT + 1U];
    char album[MPRIS_MAX_TEXT + 1U];
    char art_url[MPRIS_MAX_TEXT + 1U];
    uint64_t length;
    int64_t position;
    double volume;
};

struct tomoe_mpris {
    sd_bus *bus;
    sd_bus_slot *matches[3];
    struct mpris_alias aliases[MPRIS_MAX_ALIASES];
    struct mpris_player players[MPRIS_MAX_PLAYERS];
    struct mpris_call *calls;
    size_t alias_count;
    size_t player_count;
    size_t pending_count;
    uint64_t alias_order;
    uint64_t alias_epoch;
    uint64_t next_generation;
    uint64_t next_request;
    uint64_t clock;
    uint64_t revision;
    struct public_values last;
    int last_valid;
    int available;
    int failed;
    int failure;
    int changed;
    char *snapshot;
};

int tomoe_mpris_abi(void) { return 1; }

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

static int valid_name(const char *value) {
    return bounded_string(value, MPRIS_MAX_NAME) && *value;
}

static int valid_unique_name(const char *value) {
    return valid_name(value) && value[0] == ':';
}

static int valid_alias_name(const char *value) {
    size_t prefix = sizeof(MPRIS_PREFIX) - 1U;
    return valid_name(value) && !strncmp(value, MPRIS_PREFIX, prefix) &&
           value[prefix] != '\0';
}

static void revision_bump(struct tomoe_mpris *mpris) {
    mpris->revision++;
    if (!mpris->revision) mpris->revision = 1;
    mpris->changed = 1;
}

static void mark_failed(struct tomoe_mpris *mpris, int result) {
    if (mpris->failed) return;
    mpris->failed = 1;
    mpris->failure = positive_error(result);
    mpris->available = 0;
    mpris->alias_count = 0;
    mpris->player_count = 0;
    memset(mpris->aliases, 0, sizeof(mpris->aliases));
    memset(mpris->players, 0, sizeof(mpris->players));
    revision_bump(mpris);
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

static int text_buffer_string(struct text_buffer *buffer, const char *text) {
    int result = text_buffer_append(buffer, "\"", 1U);
    if (result < 0) return result;
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor;
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

static int text_buffer_uint64(struct text_buffer *buffer, uint64_t value) {
    char text[32];
    int length = snprintf(text, sizeof(text), "%" PRIu64, value);
    if (length < 0 || (size_t)length >= sizeof(text)) return -EOVERFLOW;
    return text_buffer_append(buffer, text, (size_t)length);
}

static int text_buffer_int64(struct text_buffer *buffer, int64_t value) {
    char text[32];
    int length = snprintf(text, sizeof(text), "%" PRId64, value);
    if (length < 0 || (size_t)length >= sizeof(text)) return -EOVERFLOW;
    return text_buffer_append(buffer, text, (size_t)length);
}

static int text_buffer_double(struct text_buffer *buffer, double value) {
    char text[64];
    int length = snprintf(text, sizeof(text), "%.17g", value);
    if (length < 0 || (size_t)length >= sizeof(text)) return -EOVERFLOW;
    for (int index = 0; index < length; index++) {
        if (text[index] == 'e' || text[index] == 'E') text[index] = 'd';
    }
    if (!strchr(text, 'd')) {
        if ((size_t)length + 2U >= sizeof(text)) return -EOVERFLOW;
        text[length++] = 'd';
        text[length++] = '0';
        text[length] = '\0';
    }
    return text_buffer_append(buffer, text, (size_t)length);
}

static int snapshot_build(struct tomoe_mpris *mpris, char **result_out) {
    struct text_buffer buffer = {0};
    int result = text_buffer_literal(&buffer, "(:available ");
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, mpris->last.available ? "t" : "nil");
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, " :player-name ");
    if (result < 0) goto failed;
    result = text_buffer_string(&buffer, mpris->last.player_name);
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, " :status ");
    if (result < 0) goto failed;
    result = text_buffer_string(&buffer, mpris->last.status);
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, " :title ");
    if (result < 0) goto failed;
    result = text_buffer_string(&buffer, mpris->last.title);
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, " :artist ");
    if (result < 0) goto failed;
    result = text_buffer_string(&buffer, mpris->last.artist);
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, " :album ");
    if (result < 0) goto failed;
    result = text_buffer_string(&buffer, mpris->last.album);
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, " :art-url ");
    if (result < 0) goto failed;
    result = text_buffer_string(&buffer, mpris->last.art_url);
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, " :length ");
    if (result < 0) goto failed;
    result = text_buffer_uint64(&buffer, mpris->last.length);
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, " :position ");
    if (result < 0) goto failed;
    result = text_buffer_int64(&buffer, mpris->last.position);
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, " :volume ");
    if (result < 0) goto failed;
    result = text_buffer_double(&buffer, mpris->last.volume);
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, ")");
    if (result < 0) goto failed;
    *result_out = buffer.data;
    return 0;

failed:
    free(buffer.data);
    return result;
}

const char *tomoe_mpris_snapshot(struct tomoe_mpris *mpris) {
    if (!mpris) return NULL;
    char *snapshot = NULL;
    if (snapshot_build(mpris, &snapshot) < 0) return NULL;
    free(mpris->snapshot);
    mpris->snapshot = snapshot;
    return snapshot;
}

static int make_fallback_address(char *destination, size_t capacity,
                                 const char *runtime) {
    static const char hex[] = "0123456789ABCDEF";
    size_t length = strnlen(runtime, MPRIS_MAX_RUNTIME_PATH + 1U);
    if (length > MPRIS_MAX_RUNTIME_PATH) return -ENAMETOOLONG;
    const char prefix[] = "unix:path=";
    if (sizeof(prefix) - 1U > capacity) return -ENAMETOOLONG;
    memcpy(destination, prefix, sizeof(prefix) - 1U);
    size_t offset = sizeof(prefix) - 1U;
    for (size_t index = 0; index < length; index++) {
        if (offset > capacity - 3U) return -ENAMETOOLONG;
        unsigned char value = (unsigned char)runtime[index];
        destination[offset++] = '%';
        destination[offset++] = hex[value >> 4U];
        destination[offset++] = hex[value & 0x0fU];
    }
    if (offset >= capacity) return -ENAMETOOLONG;
    destination[offset] = '\0';
    return 0;
}

static struct mpris_alias *find_alias(struct tomoe_mpris *mpris,
                                      const char *name) {
    for (size_t index = 0; index < MPRIS_MAX_ALIASES; index++)
        if (mpris->aliases[index].used &&
            !strcmp(mpris->aliases[index].name, name))
            return &mpris->aliases[index];
    return NULL;
}

static struct mpris_player *find_player_owner(struct tomoe_mpris *mpris,
                                              const char *owner) {
    for (size_t index = 0; index < MPRIS_MAX_PLAYERS; index++)
        if (mpris->players[index].used &&
            !strcmp(mpris->players[index].owner, owner))
            return &mpris->players[index];
    return NULL;
}

static struct mpris_player *find_player_sender(struct tomoe_mpris *mpris,
                                               const char *sender) {
    if (!sender) return NULL;
    if (valid_unique_name(sender)) return find_player_owner(mpris, sender);
    if (valid_alias_name(sender)) {
        struct mpris_alias *alias = find_alias(mpris, sender);
        return alias && *alias->owner ? find_player_owner(mpris, alias->owner)
                                      : NULL;
    }
    return NULL;
}

static void player_recompute_name(struct tomoe_mpris *mpris,
                                  struct mpris_player *player) {
    char best[MPRIS_MAX_NAME + 1U] = "";
    uint64_t best_order = UINT64_MAX;
    for (size_t index = 0; index < MPRIS_MAX_ALIASES; index++) {
        struct mpris_alias *alias = &mpris->aliases[index];
        if (!alias->used || strcmp(alias->owner, player->owner)) continue;
        if (alias->order < best_order ||
            (alias->order == best_order && strcmp(alias->name, best) < 0)) {
            best_order = alias->order;
            size_t length = strlen(alias->name + sizeof(MPRIS_PREFIX) - 1U);
            memcpy(best, alias->name + sizeof(MPRIS_PREFIX) - 1U, length + 1U);
        }
    }
    memcpy(player->player_name, best, sizeof(player->player_name));
}

static void remove_player(struct tomoe_mpris *mpris,
                           struct mpris_player *player) {
    if (!player || !player->used) return;
    memset(player, 0, sizeof(*player));
    if (mpris->player_count) mpris->player_count--;
}

static void remove_alias_from_player(struct tomoe_mpris *mpris,
                                     struct mpris_alias *alias) {
    if (!alias || !*alias->owner) return;
    struct mpris_player *player = find_player_owner(mpris, alias->owner);
    alias->owner[0] = '\0';
    if (!player) return;
    if (player->aliases) player->aliases--;
    if (!player->aliases) {
        remove_player(mpris, player);
    } else {
        player_recompute_name(mpris, player);
    }
}

static struct mpris_player *create_player(struct tomoe_mpris *mpris,
                                          const char *owner) {
    if (!valid_unique_name(owner) || mpris->player_count >= MPRIS_MAX_PLAYERS)
        return NULL;
    for (size_t index = 0; index < MPRIS_MAX_PLAYERS; index++) {
        struct mpris_player *player = &mpris->players[index];
        if (player->used) continue;
        memset(player, 0, sizeof(*player));
        player->used = 1;
        memcpy(player->owner, owner, strlen(owner) + 1U);
        player->generation = ++mpris->next_generation;
        if (!player->generation) player->generation = ++mpris->next_generation;
        player->volume = 1.0;
        mpris->player_count++;
        return player;
    }
    return NULL;
}

static struct mpris_player *attach_alias(struct tomoe_mpris *mpris,
                                         struct mpris_alias *alias,
                                         const char *owner) {
    if (!alias || !valid_unique_name(owner)) return NULL;
    alias->need_owner = 0;
    if (alias->owner[0] && !strcmp(alias->owner, owner)) {
        struct mpris_player *existing = find_player_owner(mpris, owner);
        if (existing) player_recompute_name(mpris, existing);
        return existing;
    }
    if (alias->owner[0]) remove_alias_from_player(mpris, alias);
    struct mpris_player *player = find_player_owner(mpris, owner);
    if (!player) player = create_player(mpris, owner);
    if (!player || player->aliases >= MPRIS_MAX_ALIASES_PER_PLAYER) return NULL;
    memcpy(alias->owner, owner, strlen(owner) + 1U);
    alias->order = ++mpris->alias_order;
    if (!alias->order) alias->order = ++mpris->alias_order;
    player->aliases++;
    player_recompute_name(mpris, player);
    if (player->aliases == 1U) {
        player->activity = ++mpris->clock;
        player->need_all = 1;
    }
    return player;
}

static struct mpris_alias *create_alias(struct tomoe_mpris *mpris,
                                        const char *name) {
    struct mpris_alias *alias = find_alias(mpris, name);
    if (alias) return alias;
    if (!valid_alias_name(name) || mpris->alias_count >= MPRIS_MAX_ALIASES)
        return NULL;
    for (size_t index = 0; index < MPRIS_MAX_ALIASES; index++) {
        alias = &mpris->aliases[index];
        if (alias->used) continue;
        memset(alias, 0, sizeof(*alias));
        alias->used = 1;
        memcpy(alias->name, name, strlen(name) + 1U);
        alias->epoch = ++mpris->alias_epoch;
        if (!alias->epoch) alias->epoch = ++mpris->alias_epoch;
        alias->need_owner = 1;
        mpris->alias_count++;
        return alias;
    }
    return NULL;
}

static void pending_remove(struct tomoe_mpris *mpris,
                           struct mpris_call *call) {
    struct mpris_call **cursor = &mpris->calls;
    while (*cursor && *cursor != call) cursor = &(*cursor)->next;
    if (*cursor) {
        *cursor = call->next;
        if (mpris->pending_count) mpris->pending_count--;
    }
}

static void clear_pending(struct tomoe_mpris *mpris) {
    struct mpris_call *call = mpris->calls;
    while (call) {
        struct mpris_call *next = call->next;
        free(call);
        call = next;
    }
    mpris->calls = NULL;
    mpris->pending_count = 0;
}

static int pending_owner_call(const struct tomoe_mpris *mpris,
                              const struct mpris_alias *alias) {
    for (const struct mpris_call *call = mpris->calls; call;
         call = call->next)
        if (call->kind == PENDING_GET_OWNER &&
            call->alias_epoch == alias->epoch &&
            !strcmp(call->alias, alias->name))
            return 1;
    return 0;
}

static int copy_bus_string(char *destination, size_t capacity,
                           const char *value) {
    if (!bounded_string(value, capacity - 1U)) return -E2BIG;
    memcpy(destination, value, strlen(value) + 1U);
    return 0;
}

static int append_artist(char *destination, size_t capacity, size_t *length,
                         size_t *count, const char *artist) {
    if (!bounded_string(artist, MPRIS_MAX_TEXT)) return -E2BIG;
    size_t artist_length = strlen(artist);
    size_t separator = *count ? 2U : 0U;
    if (separator > capacity - 1U - *length ||
        artist_length > capacity - 1U - *length - separator)
        return -E2BIG;
    if (separator) {
        destination[(*length)++] = ',';
        destination[(*length)++] = ' ';
    }
    memcpy(destination + *length, artist, artist_length);
    *length += artist_length;
    destination[*length] = '\0';
    (*count)++;
    return 0;
}

static int parse_length(char type, const void *value, uint64_t *result) {
    long double micros;
    if (type == 'x') micros = (long double)*(const int64_t *)value;
    else if (type == 't') micros = (long double)*(const uint64_t *)value;
    else if (type == 'd') {
        double number = *(const double *)value;
        if (!isfinite(number)) return -EINVAL;
        micros = (long double)number;
    } else return -EINVAL;
    if (micros < 0.0L) micros = 0.0L;
    uint64_t microseconds = micros >= (long double)UINT64_MAX
        ? UINT64_MAX : (uint64_t)micros;
    *result = microseconds / 1000000ULL;
    return 0;
}

static int read_metadata_value(sd_bus_message *message,
                               struct mpris_values *values) {
    int result = sd_bus_message_enter_container(message, 'a', "{sv}");
    if (result < 0) return result;
    if (!result) return -EBADMSG;
    size_t artist_count = 0;
    size_t artist_length = 0;
    size_t entries = 0;
    values->title[0] = '\0';
    values->artist[0] = '\0';
    values->album[0] = '\0';
    values->art_url[0] = '\0';
    values->length = 0;
    for (;;) {
        result = sd_bus_message_enter_container(message, 'e', "sv");
        if (result < 0) return result;
        if (!result) break;
        if (++entries > MPRIS_MAX_DICT) return -E2BIG;
        const char *key = NULL;
        result = sd_bus_message_read(message, "s", &key);
        if (result < 0 || !bounded_string(key, MPRIS_MAX_TEXT)) return -EBADMSG;
        char value_type = 0;
        const char *value_contents = NULL;
        result = sd_bus_message_peek_type(message, &value_type, &value_contents);
        if (result <= 0 || value_type != 'v' || !value_contents)
            return result < 0 ? result : -EBADMSG;
        result = sd_bus_message_enter_container(message, 'v', value_contents);
        if (result < 0) return result;
        char basic_type = 0;
        const char *basic_contents = NULL;
        result = sd_bus_message_peek_type(message, &basic_type, &basic_contents);
        if (result <= 0) return result < 0 ? result : -EBADMSG;
        if (!strcmp(key, "xesam:title") && basic_type == 's') {
            const char *value = NULL;
            result = sd_bus_message_read(message, "s", &value);
            if (result < 0 || copy_bus_string(values->title,
                                               sizeof(values->title), value) < 0)
                return -EBADMSG;
        } else if (!strcmp(key, "xesam:album") && basic_type == 's') {
            const char *value = NULL;
            result = sd_bus_message_read(message, "s", &value);
            if (result < 0 || copy_bus_string(values->album,
                                               sizeof(values->album), value) < 0)
                return -EBADMSG;
        } else if (!strcmp(key, "mpris:artUrl") && basic_type == 's') {
            const char *value = NULL;
            result = sd_bus_message_read(message, "s", &value);
            if (result < 0 || copy_bus_string(values->art_url,
                                               sizeof(values->art_url), value) < 0)
                return -EBADMSG;
        } else if (!strcmp(key, "xesam:artist") && basic_type == 's') {
            const char *value = NULL;
            result = sd_bus_message_read(message, "s", &value);
            if (result < 0 ||
                append_artist(values->artist, sizeof(values->artist),
                              &artist_length, &artist_count, value) < 0)
                return -EBADMSG;
        } else if (!strcmp(key, "xesam:artist") && basic_type == 'a') {
            result = sd_bus_message_enter_container(message, 'a', "s");
            if (result < 0) return result;
            const char *value = NULL;
            while ((result = sd_bus_message_read(message, "s", &value)) > 0) {
                if (result < 0 ||
                    artist_count >= MPRIS_MAX_ARTISTS ||
                    append_artist(values->artist, sizeof(values->artist),
                                  &artist_length, &artist_count, value) < 0)
                    return -EBADMSG;
            }
            if (result < 0 || sd_bus_message_exit_container(message) < 0)
                return result < 0 ? result : -EBADMSG;
        } else if (!strcmp(key, "mpris:length") &&
                   (basic_type == 'x' || basic_type == 't' ||
                    basic_type == 'd')) {
            int64_t signed_value = 0;
            uint64_t unsigned_value = 0;
            double double_value = 0.0;
            if (basic_type == 'x')
                result = sd_bus_message_read(message, "x", &signed_value);
            else if (basic_type == 't')
                result = sd_bus_message_read(message, "t", &unsigned_value);
            else
                result = sd_bus_message_read(message, "d", &double_value);
            if (result < 0) return result;
            if (basic_type == 'x')
                result = parse_length('x', &signed_value, &values->length);
            else if (basic_type == 't')
                result = parse_length('t', &unsigned_value, &values->length);
            else
                result = parse_length('d', &double_value, &values->length);
            if (result < 0) return -EBADMSG;
        } else {
            result = sd_bus_message_skip(message, NULL);
            if (result < 0) return result;
        }
        result = sd_bus_message_exit_container(message);
        if (result < 0) return result;
        result = sd_bus_message_exit_container(message);
        if (result < 0) return result;
    }
    if (sd_bus_message_exit_container(message) < 0) return -EBADMSG;
    values->metadata = 1;
    return 0;
}

struct invalidated_values {
    int status;
    int volume;
    int metadata;
    int position;
};

static int read_property_dict(sd_bus_message *message,
                              struct mpris_values *values) {
    int result = sd_bus_message_enter_container(message, 'a', "{sv}");
    if (result < 0) return result;
    if (!result) return -EBADMSG;
    size_t entries = 0;
    for (;;) {
        result = sd_bus_message_enter_container(message, 'e', "sv");
        if (result < 0) return result;
        if (!result) break;
        if (++entries > MPRIS_MAX_DICT) return -E2BIG;
        const char *key = NULL;
        result = sd_bus_message_read(message, "s", &key);
        if (result < 0 || !bounded_string(key, MPRIS_MAX_TEXT))
            return -EBADMSG;
        char value_type = 0;
        const char *value_contents = NULL;
        result = sd_bus_message_peek_type(message, &value_type, &value_contents);
        if (result <= 0 || value_type != 'v' || !value_contents)
            return result < 0 ? result : -EBADMSG;
        result = sd_bus_message_enter_container(message, 'v', value_contents);
        if (result < 0) return result;
        char basic_type = 0;
        const char *basic_contents = NULL;
        result = sd_bus_message_peek_type(message, &basic_type, &basic_contents);
        if (result <= 0) return result < 0 ? result : -EBADMSG;
        if (!strcmp(key, "PlaybackStatus") && basic_type == 's') {
            const char *value = NULL;
            result = sd_bus_message_read(message, "s", &value);
            if (result < 0 ||
                copy_bus_string(values->status_text,
                                sizeof(values->status_text), value) < 0)
                return -EBADMSG;
            values->status = 1;
        } else if (!strcmp(key, "Volume") && basic_type == 'd') {
            result = sd_bus_message_read(message, "d", &values->volume_value);
            if (result < 0 || !isfinite(values->volume_value))
                return -EBADMSG;
            values->volume = 1;
        } else if (!strcmp(key, "Position") && basic_type == 'x') {
            result = sd_bus_message_read(message, "x", &values->position_value);
            if (result < 0) return result;
            values->position = 1;
        } else if (!strcmp(key, "Metadata") && basic_type == 'a') {
            result = read_metadata_value(message, values);
            if (result < 0) return result;
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
                            struct invalidated_values *invalidated) {
    int result = sd_bus_message_enter_container(message, 'a', "s");
    if (result < 0) return result;
    if (!result) return -EBADMSG;
    size_t count = 0;
    for (;;) {
        const char *property = NULL;
        result = sd_bus_message_read(message, "s", &property);
        if (result < 0) return result;
        if (!result) break;
        if (++count > 16U || !bounded_string(property, MPRIS_MAX_TEXT))
            return -E2BIG;
        if (!strcmp(property, "PlaybackStatus")) invalidated->status = 1;
        else if (!strcmp(property, "Volume")) invalidated->volume = 1;
        else if (!strcmp(property, "Metadata")) invalidated->metadata = 1;
        else if (!strcmp(property, "Position")) invalidated->position = 1;
    }
    return sd_bus_message_exit_container(message);
}

static void clear_metadata(struct mpris_player *player) {
    player->title[0] = '\0';
    player->artist[0] = '\0';
    player->album[0] = '\0';
    player->art_url[0] = '\0';
    player->length = 0;
}

static void apply_metadata(struct mpris_player *player,
                           const struct mpris_values *values) {
    memcpy(player->title, values->title, sizeof(player->title));
    memcpy(player->artist, values->artist, sizeof(player->artist));
    memcpy(player->album, values->album, sizeof(player->album));
    memcpy(player->art_url, values->art_url, sizeof(player->art_url));
    player->length = values->length;
}

static int async_reply(sd_bus_message *message, void *userdata,
                       sd_bus_error *ret_error);

static int queue_call(struct tomoe_mpris *mpris, struct mpris_call *call,
                      const char *destination, const char *interface,
                      const char *member, const char *types, ...) {
    if (!mpris->bus || mpris->failed ||
        mpris->pending_count >= MPRIS_MAX_PENDING) {
        free(call);
        return -ENOSPC;
    }
    call->mpris = mpris;
    call->request = ++mpris->next_request;
    if (!call->request) call->request = ++mpris->next_request;
    call->deadline_usec =
        deadline_after(monotonic_usec(), MPRIS_REQUEST_USEC);
    call->next = mpris->calls;
    mpris->calls = call;
    mpris->pending_count++;
    va_list ap;
    va_start(ap, types);
    int result = sd_bus_call_method_asyncv(
        mpris->bus, NULL, destination,
        !strcmp(interface, MPRIS_DBUS) ? MPRIS_DBUS_PATH : MPRIS_PATH,
        interface, member,
        async_reply, call, types, ap);
    va_end(ap);
    if (result < 0) {
        pending_remove(mpris, call);
        free(call);
        return result;
    }
    return 0;
}

static int queue_list_names(struct tomoe_mpris *mpris) {
    struct mpris_call *call = calloc(1, sizeof(*call));
    if (!call) return -ENOMEM;
    call->kind = PENDING_LIST_NAMES;
    return queue_call(mpris, call, MPRIS_DBUS, MPRIS_DBUS, "ListNames", "");
}

static int queue_get_owner(struct tomoe_mpris *mpris,
                           struct mpris_alias *alias) {
    if (!alias || !alias->used || pending_owner_call(mpris, alias) ||
        mpris->pending_count >= MPRIS_MAX_PENDING)
        return -ENOSPC;
    struct mpris_call *call = calloc(1, sizeof(*call));
    if (!call) return -ENOMEM;
    call->kind = PENDING_GET_OWNER;
    call->alias_epoch = alias->epoch;
    memcpy(call->alias, alias->name, sizeof(call->alias));
    int result = queue_call(mpris, call, MPRIS_DBUS, MPRIS_DBUS,
                            "GetNameOwner", "s", alias->name);
    if (result >= 0) alias->need_owner = 0;
    return result;
}

static int queue_get_all(struct tomoe_mpris *mpris,
                         struct mpris_player *player) {
    if (!player || !player->used) return 0;
    if (mpris->pending_count >= MPRIS_MAX_PENDING) return -ENOSPC;
    struct mpris_call *call = calloc(1, sizeof(*call));
    if (!call) return -ENOMEM;
    call->kind = PENDING_GET_ALL;
    call->generation = player->generation;
    call->signal_serial = player->signal_serial;
    call->status_epoch = player->status_epoch;
    call->volume_epoch = player->volume_epoch;
    call->metadata_epoch = player->metadata_epoch;
    call->position_epoch = player->position_epoch;
    memcpy(call->owner, player->owner, sizeof(call->owner));
    int result = queue_call(mpris, call, player->owner, MPRIS_PROPERTIES,
                            "GetAll", "s", MPRIS_PLAYER);
    if (result >= 0) player->need_all = 0;
    return result;
}

static int queue_position(struct tomoe_mpris *mpris,
                          struct mpris_player *player) {
    if (!player || !player->used) return 0;
    if (mpris->pending_count >= MPRIS_MAX_PENDING) return -ENOSPC;
    struct mpris_call *call = calloc(1, sizeof(*call));
    if (!call) return -ENOMEM;
    call->kind = PENDING_GET_POSITION;
    call->generation = player->generation;
    call->signal_serial = player->signal_serial;
    call->position_epoch = player->position_epoch;
    memcpy(call->owner, player->owner, sizeof(call->owner));
    int result = queue_call(mpris, call, player->owner, MPRIS_PROPERTIES,
                            "Get", "ss", MPRIS_PLAYER, "Position");
    if (result >= 0) {
        player->position_request = call->request;
        player->need_position = 0;
    }
    return result;
}

static void touch_player(struct tomoe_mpris *mpris,
                         struct mpris_player *player) {
    player->activity = ++mpris->clock;
    if (!player->activity) player->activity = ++mpris->clock;
}

static int parse_position_variant(sd_bus_message *message, int64_t *position) {
    char type = 0;
    const char *contents = NULL;
    int result = sd_bus_message_peek_type(message, &type, &contents);
    if (result <= 0 || type != 'v' || !contents)
        return result < 0 ? result : -EBADMSG;
    result = sd_bus_message_enter_container(message, 'v', contents);
    if (result < 0) return result;
    char value_type = 0;
    const char *value_contents = NULL;
    result = sd_bus_message_peek_type(message, &value_type, &value_contents);
    if (result <= 0 || value_type != 'x')
        return result < 0 ? result : -EBADMSG;
    result = sd_bus_message_read(message, "x", position);
    if (result < 0) return result;
    return sd_bus_message_exit_container(message);
}

static int parse_list_names(sd_bus_message *message,
                            struct tomoe_mpris *mpris) {
    int result = sd_bus_message_enter_container(message, 'a', "s");
    if (result < 0) return result;
    if (!result) return -EBADMSG;
    size_t count = 0;
    for (;;) {
        const char *name = NULL;
        result = sd_bus_message_read(message, "s", &name);
        if (result < 0) return result;
        if (!result) break;
        if (++count > 1024U) return -E2BIG;
        if (!valid_alias_name(name)) continue;
        struct mpris_alias *alias = create_alias(mpris, name);
        if (!alias) continue;
        (void)queue_get_owner(mpris, alias);
    }
    return sd_bus_message_exit_container(message);
}

static struct mpris_player *selected_player(struct tomoe_mpris *mpris) {
    struct mpris_player *selected = NULL;
    for (size_t index = 0; index < MPRIS_MAX_PLAYERS; index++) {
        struct mpris_player *candidate = &mpris->players[index];
        if (!candidate->used) continue;
        int candidate_playing = !strcmp(candidate->status, "Playing");
        int selected_playing = selected && !strcmp(selected->status, "Playing");
        if (!selected || candidate_playing > selected_playing ||
            (candidate_playing == selected_playing &&
             (candidate->activity > selected->activity ||
              (candidate->activity == selected->activity &&
               strcmp(candidate->player_name, selected->player_name) < 0))))
            selected = candidate;
    }
    return selected;
}

static void derive_public(struct tomoe_mpris *mpris,
                          struct public_values *values) {
    memset(values, 0, sizeof(*values));
    values->available = mpris->available;
    values->volume = 1.0;
    struct mpris_player *player = selected_player(mpris);
    if (!player) return;
    memcpy(values->player_name, player->player_name,
           sizeof(values->player_name));
    memcpy(values->status, player->status, sizeof(values->status));
    memcpy(values->title, player->title, sizeof(values->title));
    memcpy(values->artist, player->artist, sizeof(values->artist));
    memcpy(values->album, player->album, sizeof(values->album));
    memcpy(values->art_url, player->art_url, sizeof(values->art_url));
    values->length = player->length;
    values->position = player->position < 0 ? 0 : player->position / 1000000;
    values->volume = isfinite(player->volume) ? player->volume : 1.0;
}

static int public_equal(const struct public_values *left,
                        const struct public_values *right) {
    return left->available == right->available &&
           !strcmp(left->player_name, right->player_name) &&
           !strcmp(left->status, right->status) &&
           !strcmp(left->title, right->title) &&
           !strcmp(left->artist, right->artist) &&
           !strcmp(left->album, right->album) &&
           !strcmp(left->art_url, right->art_url) &&
           left->length == right->length && left->position == right->position &&
           left->volume == right->volume;
}

static void refresh_public(struct tomoe_mpris *mpris) {
    struct public_values values;
    derive_public(mpris, &values);
    if (!mpris->last_valid || !public_equal(&values, &mpris->last)) {
        mpris->last = values;
        mpris->last_valid = 1;
        revision_bump(mpris);
    }
}

static int message_path_is(sd_bus_message *message, const char *path) {
    const char *actual = sd_bus_message_get_path(message);
    return actual && !strcmp(actual, path);
}

static int message_sender_is(sd_bus_message *message, const char *sender) {
    const char *actual = sd_bus_message_get_sender(message);
    return actual && !strcmp(actual, sender);
}

static void invalidate_alias_owner(struct tomoe_mpris *mpris,
                                   struct mpris_alias *alias) {
    if (!alias) return;
    remove_alias_from_player(mpris, alias);
}

static void retire_alias(struct tomoe_mpris *mpris,
                         struct mpris_alias *alias) {
    if (!alias || !alias->used) return;
    remove_alias_from_player(mpris, alias);
    memset(alias, 0, sizeof(*alias));
    if (mpris->alias_count) mpris->alias_count--;
}

static int name_owner_signal(sd_bus_message *message, void *userdata,
                             sd_bus_error *ret_error) {
    struct tomoe_mpris *mpris = userdata;
    (void)ret_error;
    if (!message_path_is(message, MPRIS_DBUS_PATH) ||
        !message_sender_is(message, MPRIS_DBUS))
        return 1;
    const char *name = NULL;
    const char *old_owner = NULL;
    const char *new_owner = NULL;
    int result = sd_bus_message_read(message, "sss", &name, &old_owner,
                                     &new_owner);
    if (result < 0 || !valid_alias_name(name) ||
        (old_owner && *old_owner && !valid_unique_name(old_owner)) ||
        (new_owner && *new_owner && !valid_unique_name(new_owner)))
        return 1;
    struct mpris_alias *alias = create_alias(mpris, name);
    if (!alias) return 1;
    alias->epoch = ++mpris->alias_epoch;
    if (!alias->epoch) alias->epoch = ++mpris->alias_epoch;
    if (alias->owner[0] &&
        (!old_owner || strcmp(alias->owner, old_owner)))
        return 1;
    invalidate_alias_owner(mpris, alias);
    if (new_owner && *new_owner) {
        alias->need_owner = 0;
        struct mpris_player *player = find_player_owner(mpris, new_owner);
        int was_new = !player;
        player = attach_alias(mpris, alias, new_owner);
        if (player && (was_new || player->aliases == 1U))
            (void)queue_get_all(mpris, player);
    } else retire_alias(mpris, alias);
    return 1;
}

static void apply_signal_values(struct tomoe_mpris *mpris,
                                struct mpris_player *player,
                                const struct mpris_values *values,
                                const struct invalidated_values *invalidated) {
    int need_all = invalidated->status || invalidated->volume ||
                   invalidated->metadata;
    int need_position = values->position || values->metadata ||
                        values->status || invalidated->position ||
                        invalidated->metadata || invalidated->status;
    if (values->status || invalidated->status) player->status_epoch++;
    if (values->volume || invalidated->volume) player->volume_epoch++;
    if (values->metadata || invalidated->metadata) player->metadata_epoch++;
    if (need_position) player->position_epoch++;
    player->signal_serial++;
    if (values->status) memcpy(player->status, values->status_text,
                                sizeof(player->status));
    if (values->volume) player->volume = values->volume_value;
    if (values->metadata) {
        clear_metadata(player);
        apply_metadata(player, values);
        player->position = 0;
    }
    if (values->position) {
        player->position = values->position_value < 0 ? 0 :
                           values->position_value;
        player->need_position = 0;
    } else if (need_position) {
        player->need_position = 1;
    }
    if (need_all) player->need_all = 1;
    touch_player(mpris, player);
    if (player->need_all) (void)queue_get_all(mpris, player);
    if (player->need_position) (void)queue_position(mpris, player);
}

static int properties_signal(sd_bus_message *message, void *userdata,
                             sd_bus_error *ret_error) {
    struct tomoe_mpris *mpris = userdata;
    (void)ret_error;
    if (!message_path_is(message, MPRIS_PATH)) return 1;
    struct mpris_player *player =
        find_player_sender(mpris, sd_bus_message_get_sender(message));
    if (!player) return 1;
    const char *interface = NULL;
    int result = sd_bus_message_read(message, "s", &interface);
    if (result < 0 || !interface || strcmp(interface, MPRIS_PLAYER)) return 1;
    struct mpris_values values = {0};
    struct invalidated_values invalidated = {0};
    result = read_property_dict(message, &values);
    if (result < 0) return 1;
    result = read_invalidated(message, &invalidated);
    if (result < 0) return 1;
    if (values.status || values.volume || values.metadata || values.position ||
        invalidated.status || invalidated.volume || invalidated.metadata ||
        invalidated.position)
        apply_signal_values(mpris, player, &values, &invalidated);
    else {
        player->signal_serial++;
        touch_player(mpris, player);
    }
    return 1;
}

static int seeked_signal(sd_bus_message *message, void *userdata,
                         sd_bus_error *ret_error) {
    struct tomoe_mpris *mpris = userdata;
    (void)ret_error;
    if (!message_path_is(message, MPRIS_PATH)) return 1;
    struct mpris_player *player =
        find_player_sender(mpris, sd_bus_message_get_sender(message));
    if (!player) return 1;
    int64_t position = 0;
    int result = sd_bus_message_read(message, "x", &position);
    if (result < 0) return 1;
    player->position_epoch++;
    player->signal_serial++;
    player->position = position < 0 ? 0 : position;
    player->need_position = 0;
    touch_player(mpris, player);
    return 1;
}

static int reply_is_error(sd_bus_message *message) {
    return !message || sd_bus_message_get_error(message) != NULL;
}

static void handle_get_owner(struct mpris_call *call,
                             sd_bus_message *message) {
    struct tomoe_mpris *mpris = call->mpris;
    struct mpris_alias *alias = find_alias(mpris, call->alias);
    if (!alias || alias->epoch != call->alias_epoch) return;
    const sd_bus_error *error = message ? sd_bus_message_get_error(message) : NULL;
    if (error) {
        if (error->name &&
            !strcmp(error->name, "org.freedesktop.DBus.Error.NameHasNoOwner"))
            retire_alias(mpris, alias);
        return;
    }
    const char *owner = NULL;
    if (sd_bus_message_read(message, "s", &owner) < 0 ||
        !valid_unique_name(owner))
        return;
    struct mpris_player *player = find_player_owner(mpris, owner);
    int was_new = !player;
    player = attach_alias(mpris, alias, owner);
    if (player && (was_new || player->aliases == 1U))
        (void)queue_get_all(mpris, player);
}

static void handle_get_all(struct mpris_call *call,
                           sd_bus_message *message) {
    struct tomoe_mpris *mpris = call->mpris;
    if (reply_is_error(message)) return;
    struct mpris_player *player = find_player_owner(mpris, call->owner);
    if (!player || player->generation != call->generation) return;
    struct mpris_values values = {0};
    if (read_property_dict(message, &values) < 0) return;
    int need_position = 0;
    if (values.status && player->status_epoch == call->status_epoch &&
        player->status_read < call->request) {
        player->status_read = call->request;
        memcpy(player->status, values.status_text, sizeof(player->status));
        if (player->position_epoch == call->position_epoch)
            need_position = 1;
    }
    if (values.volume && player->volume_epoch == call->volume_epoch &&
        player->volume_read < call->request) {
        player->volume_read = call->request;
        player->volume = values.volume_value;
    }
    if (values.metadata && player->metadata_epoch == call->metadata_epoch &&
        player->metadata_read < call->request) {
        player->metadata_read = call->request;
        apply_metadata(player, &values);
        if (player->position_epoch == call->position_epoch)
            need_position = 1;
    }
    if (values.position && player->position_epoch == call->position_epoch) {
        player->position = values.position_value < 0 ? 0 :
                           values.position_value;
    }
    if (need_position) player->need_position = 1;
    if (player->need_position) (void)queue_position(mpris, player);
    if (player->need_all) (void)queue_get_all(mpris, player);
}

static void handle_get_position(struct mpris_call *call,
                                sd_bus_message *message) {
    struct tomoe_mpris *mpris = call->mpris;
    if (reply_is_error(message)) return;
    struct mpris_player *player = find_player_owner(mpris, call->owner);
    if (!player || player->generation != call->generation)
        return;
    int64_t position = 0;
    if (parse_position_variant(message, &position) < 0) return;
    if (!player->need_position &&
        player->position_epoch == call->position_epoch &&
        player->position_request == call->request) {
        player->position = position < 0 ? 0 : position;
        player->need_position = 0;
    }
    if (player->need_position) (void)queue_position(mpris, player);
}

static void handle_list_names(struct mpris_call *call,
                              sd_bus_message *message) {
    if (reply_is_error(message)) return;
    (void)parse_list_names(message, call->mpris);
}

static int async_reply(sd_bus_message *message, void *userdata,
                       sd_bus_error *ret_error) {
    struct mpris_call *call = userdata;
    struct tomoe_mpris *mpris = call ? call->mpris : NULL;
    (void)ret_error;
    if (!call || !mpris) return 1;
    pending_remove(mpris, call);
    switch (call->kind) {
    case PENDING_LIST_NAMES:
        handle_list_names(call, message);
        break;
    case PENDING_GET_OWNER:
        handle_get_owner(call, message);
        break;
    case PENDING_GET_ALL:
        handle_get_all(call, message);
        break;
    case PENDING_GET_POSITION:
        handle_get_position(call, message);
        break;
    }
    free(call);
    return 1;
}

static void service_pending_needs(struct tomoe_mpris *mpris) {
    for (size_t index = 0; index < MPRIS_MAX_ALIASES; index++) {
        struct mpris_alias *alias = &mpris->aliases[index];
        if (!alias->used || !alias->need_owner) continue;
        (void)queue_get_owner(mpris, alias);
        if (mpris->pending_count >= MPRIS_MAX_PENDING) return;
    }
    for (size_t index = 0; index < MPRIS_MAX_PLAYERS; index++) {
        struct mpris_player *player = &mpris->players[index];
        if (!player->used) continue;
        if (player->need_all) (void)queue_get_all(mpris, player);
        if (player->need_position) (void)queue_position(mpris, player);
        if (mpris->pending_count >= MPRIS_MAX_PENDING) break;
    }
}

static void close_bus(struct tomoe_mpris *mpris) {
    if (!mpris) return;
    for (size_t index = 0; index < sizeof(mpris->matches) /
                                      sizeof(mpris->matches[0]); index++) {
        if (mpris->matches[index]) {
            sd_bus_slot_unref(mpris->matches[index]);
            mpris->matches[index] = NULL;
        }
    }
    if (mpris->bus) {
        sd_bus_close(mpris->bus);
        sd_bus_unref(mpris->bus);
        mpris->bus = NULL;
    }
    clear_pending(mpris);
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

static int remaining_timeout(sd_bus *bus, uint64_t deadline) {
    uint64_t now = monotonic_usec();
    if (now >= deadline) return -ETIMEDOUT;
    int result = sd_bus_set_method_call_timeout(bus, deadline - now);
    return result < 0 ? result : 0;
}

struct tomoe_mpris *tomoe_mpris_open(int *error) {
    if (!error) return NULL;
    *error = EINVAL;
    struct tomoe_mpris *mpris = calloc(1, sizeof(*mpris));
    if (!mpris) {
        *error = ENOMEM;
        return NULL;
    }
    mpris->available = 1;
    mpris->revision = 1;
    mpris->last.available = 1;
    mpris->last.volume = 1.0;
    mpris->last_valid = 1;

    sd_bus *bus = NULL;
    int result = sd_bus_new(&bus);
    if (result < 0) goto failed;
    result = sd_bus_set_bus_client(bus, 1);
    if (result < 0) goto failed;
    const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
    char address_buffer[MPRIS_MAX_RUNTIME_PATH * 3U + 32U];
    if (!address || !*address) {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        if (!runtime || !*runtime) {
            result = -ENOMEDIUM;
            goto failed;
        }
        size_t length = strnlen(runtime, MPRIS_MAX_RUNTIME_PATH + 1U);
        if (length > MPRIS_MAX_RUNTIME_PATH ||
            length > SIZE_MAX - 5U) {
            result = -ENAMETOOLONG;
            goto failed;
        }
        char runtime_bus[MPRIS_MAX_RUNTIME_PATH + 5U];
        memcpy(runtime_bus, runtime, length);
        memcpy(runtime_bus + length, "/bus", 5U);
        result = make_fallback_address(address_buffer, sizeof(address_buffer),
                                       runtime_bus);
        if (result < 0) goto failed;
        address = address_buffer;
    }
    result = sd_bus_set_address(bus, address);
    if (result < 0) goto failed;
    result = sd_bus_set_method_call_timeout(bus, MPRIS_SETUP_USEC);
    if (result < 0) goto failed;
    result = sd_bus_start(bus);
    if (result < 0) goto failed;
    uint64_t deadline = deadline_after(monotonic_usec(), MPRIS_SETUP_USEC);
    result = setup_ready(bus, deadline);
    if (result < 0) goto failed;

    mpris->bus = bus;
    bus = NULL;
    result = remaining_timeout(mpris->bus, deadline);
    if (result < 0) goto failed_mpris;
    result = sd_bus_add_match(mpris->bus, &mpris->matches[0],
        "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0namespace='org.mpris.MediaPlayer2'",
        name_owner_signal, mpris);
    if (result < 0) goto failed_mpris;
    result = remaining_timeout(mpris->bus, deadline);
    if (result < 0) goto failed_mpris;
    result = sd_bus_add_match(mpris->bus, &mpris->matches[1],
        "type='signal',path='/org/mpris/MediaPlayer2',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
        properties_signal, mpris);
    if (result < 0) goto failed_mpris;
    result = remaining_timeout(mpris->bus, deadline);
    if (result < 0) goto failed_mpris;
    result = sd_bus_add_match(mpris->bus, &mpris->matches[2],
        "type='signal',path='/org/mpris/MediaPlayer2',interface='org.mpris.MediaPlayer2.Player',member='Seeked'",
        seeked_signal, mpris);
    if (result < 0) goto failed_mpris;
    result = remaining_timeout(mpris->bus, deadline);
    if (result < 0) goto failed_mpris;
    result = queue_list_names(mpris);
    if (result < 0) goto failed_mpris;
    result = sd_bus_set_method_call_timeout(mpris->bus, MPRIS_REQUEST_USEC);
    if (result < 0) goto failed_mpris;
    *error = 0;
    return mpris;

failed_mpris:
    close_bus(mpris);
    goto failed_allocated;
failed:
    if (bus) {
        sd_bus_close(bus);
        sd_bus_unref(bus);
    }
failed_allocated:
    free(mpris);
    *error = positive_error(result);
    return NULL;
}

int tomoe_mpris_poll(struct tomoe_mpris *mpris) {
    if (!mpris) return -EINVAL;
    if (mpris->failed) {
        int failure = mpris->failure;
        close_bus(mpris);
        return -failure;
    }
    mpris->changed = 0;
    uint64_t start = monotonic_usec();
    size_t processed = 0;
    service_pending_needs(mpris);
    while (mpris->bus && processed < MPRIS_POLL_MESSAGES) {
        uint64_t now = monotonic_usec();
        if (now >= start && now - start >= MPRIS_POLL_BUDGET_USEC) break;
        int result = sd_bus_process(mpris->bus, NULL);
        if (result < 0) {
            if (result == -EAGAIN || result == -EINTR) break;
            mark_failed(mpris, result);
            break;
        }
        if (!result) break;
        processed++;
        if (mpris->failed) break;
        service_pending_needs(mpris);
    }
    if (mpris->failed) {
        refresh_public(mpris);
        int failure = mpris->failure;
        close_bus(mpris);
        return -failure;
    }
    service_pending_needs(mpris);
    refresh_public(mpris);
    return mpris->changed ? 1 : 0;
}

uint64_t tomoe_mpris_revision(const struct tomoe_mpris *mpris) {
    return mpris ? mpris->revision : 0;
}

int tomoe_mpris_timeout(const struct tomoe_mpris *mpris, int max_ms) {
    if (!mpris) return 0;
    if (max_ms < 0) max_ms = 0;
    uint64_t best = UINT64_MAX;
    for (const struct mpris_call *call = mpris->calls; call;
         call = call->next)
        if (call->deadline_usec < best) best = call->deadline_usec;
    if (best == UINT64_MAX) return max_ms;
    uint64_t now = monotonic_usec();
    if (best <= now) return 0;
    uint64_t milliseconds = (best - now + 999ULL) / 1000ULL;
    if (milliseconds >= (uint64_t)max_ms) return max_ms;
    return (int)milliseconds;
}

void tomoe_mpris_close(struct tomoe_mpris *mpris) {
    if (!mpris) return;
    close_bus(mpris);
    free(mpris->snapshot);
    free(mpris);
}
