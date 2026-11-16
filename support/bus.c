#define _GNU_SOURCE

#include "bus.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define BUS_MAX_DEPTH 32
#define BUS_MAX_SIGNATURE 256
#define BUS_MAX_NAME 256
#define BUS_MAX_PATH 4096
#define BUS_MAX_RULE 1024
#define BUS_MAX_MESSAGE (64U * 1024U * 1024U)
#define BUS_READ_CHUNK 65536U

#define BUS_TYPE_CALL 1
#define BUS_TYPE_RETURN 2
#define BUS_TYPE_ERROR 3
#define BUS_TYPE_SIGNAL 4

#define BUS_FIELD_PATH 1
#define BUS_FIELD_INTERFACE 2
#define BUS_FIELD_MEMBER 3
#define BUS_FIELD_ERROR 4
#define BUS_FIELD_REPLY 5
#define BUS_FIELD_DESTINATION 6
#define BUS_FIELD_SENDER 7
#define BUS_FIELD_SIGNATURE 8

struct bus_buffer {
    uint8_t *data;
    size_t length;
    size_t capacity;
};

struct bus_frame {
    size_t end;
    const char *signature;
    size_t position;
    int repeat;
};

struct bus_out_frame {
    char type;
    size_t length_position;
    size_t data_start;
};

enum bus_slot_kind {
    BUS_SLOT_MATCH,
    BUS_SLOT_OBJECT,
    BUS_SLOT_CALL,
};

struct bus_rule {
    char sender[BUS_MAX_NAME];
    char path[BUS_MAX_PATH];
    char path_namespace[BUS_MAX_PATH];
    char interface[BUS_MAX_NAME];
    char member[BUS_MAX_NAME];
    char arg0[BUS_MAX_NAME];
    char arg0_namespace[BUS_MAX_NAME];
    int has_type;
    int has_sender;
    int has_path;
    int has_path_namespace;
    int has_interface;
    int has_member;
    int has_arg0;
    int has_arg0_namespace;
};

struct tomoe_bus_slot {
    int refs;
    int running;
    int anonymous;
    int kind;
    struct tomoe_bus *bus;
    struct tomoe_bus_slot *next;
    int (*handle)(struct tomoe_bus_message *, void *);
    void *userdata;
    struct bus_rule rule;
    char *path;
    char *interface;
    const struct tomoe_bus_method *methods;
    size_t method_count;
    const struct tomoe_bus_property *properties;
    size_t property_count;
    const struct tomoe_bus_signal *signals;
    size_t signal_count;
    uint32_t serial;
    struct tomoe_bus_message *collected;
    int collect;
    int done;
    int result;
};

struct tomoe_bus {
    int refs;
    int fd;
    int closed;
    int failed;
    uint64_t timeout;
    uint32_t next_serial;
    char unique[BUS_MAX_NAME];
    struct bus_buffer input;
    size_t input_start;
    struct bus_buffer output;
    size_t output_start;
    struct tomoe_bus_slot *slots;
};

struct tomoe_bus_message {
    int refs;
    struct tomoe_bus *bus;
    uint8_t type;
    uint8_t flags;
    uint32_t serial;
    uint32_t reply_serial;
    char *path;
    char *interface;
    char *member;
    char *destination;
    char *error_name;
    char *sender;
    const char *signature;
    char signature_buffer[BUS_MAX_SIGNATURE];
    uint8_t *data;
    size_t size;
    size_t body_start;
    size_t body_end;
    size_t cursor;
    struct bus_frame frames[BUS_MAX_DEPTH];
    int depth;
    char contents[BUS_MAX_SIGNATURE];
    struct bus_buffer body;
    char out_signature[BUS_MAX_SIGNATURE];
    struct bus_out_frame out_frames[BUS_MAX_DEPTH];
    int out_depth;
    int discard;
    int signature_suppress;
};

static void slot_unlink(struct tomoe_bus_slot *slot);
static int dispatch_message(struct tomoe_bus *bus,
                            struct tomoe_bus_message *message);
static int message_append_va(struct tomoe_bus_message *message,
                             const char *signature, va_list *ap);
static int message_seal(struct tomoe_bus *bus,
                        struct tomoe_bus_message *message);

static uint64_t monotonic_usec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    if ((uint64_t)ts.tv_sec > UINT64_MAX / 1000000ULL) return UINT64_MAX;
    uint64_t seconds = (uint64_t)ts.tv_sec * 1000000ULL;
    uint64_t nanos = (uint64_t)ts.tv_nsec / 1000ULL;
    return seconds > UINT64_MAX - nanos ? UINT64_MAX : seconds + nanos;
}

static uint64_t deadline_after(uint64_t now, uint64_t duration) {
    return now > UINT64_MAX - duration ? UINT64_MAX : now + duration;
}

static size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static int buffer_reserve(struct bus_buffer *buffer, size_t extra) {
    if (extra > SIZE_MAX - buffer->length) return -ENOMEM;
    size_t needed = buffer->length + extra;
    if (needed <= buffer->capacity) return 0;
    size_t capacity = buffer->capacity ? buffer->capacity : 256U;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = needed;
            break;
        }
        capacity *= 2U;
    }
    uint8_t *data = realloc(buffer->data, capacity);
    if (!data) return -ENOMEM;
    buffer->data = data;
    buffer->capacity = capacity;
    return 0;
}

static int buffer_add(struct bus_buffer *buffer, const void *data,
                      size_t length) {
    int result = buffer_reserve(buffer, length);
    if (result < 0) return result;
    if (length) memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    return 0;
}

static int buffer_byte(struct bus_buffer *buffer, uint8_t value) {
    return buffer_add(buffer, &value, 1);
}

static int buffer_u32(struct bus_buffer *buffer, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24)
    };
    return buffer_add(buffer, bytes, sizeof(bytes));
}

static int buffer_zero(struct bus_buffer *buffer, size_t count) {
    int result = buffer_reserve(buffer, count);
    if (result < 0) return result;
    memset(buffer->data + buffer->length, 0, count);
    buffer->length += count;
    return 0;
}

static int buffer_align(struct bus_buffer *buffer, size_t alignment) {
    size_t padding = (alignment - (buffer->length % alignment)) % alignment;
    return buffer_zero(buffer, padding);
}

static int buffer_text(struct bus_buffer *buffer, const char *text) {
    return buffer_add(buffer, text, strlen(text));
}

static char *text_copy(const char *text) {
    if (!text) return NULL;
    size_t length = strlen(text);
    char *copy = malloc(length + 1U);
    if (!copy) return NULL;
    memcpy(copy, text, length + 1U);
    return copy;
}

static int data_u32(const uint8_t *data, size_t position, size_t limit,
                    uint32_t *value) {
    if (position > limit || limit - position < 4U) return -EBADMSG;
    const uint8_t *bytes = data + position;
    *value = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
             (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return 0;
}

static int data_u64(const uint8_t *data, size_t position, size_t limit,
                    uint64_t *value) {
    if (position > limit || limit - position < 8U) return -EBADMSG;
    uint64_t result = 0;
    for (size_t index = 0; index < 8U; index++)
        result |= (uint64_t)data[position + index] << (index * 8U);
    *value = result;
    return 0;
}

static int data_text(const uint8_t *data, size_t position, size_t limit,
                     const char **text, size_t *next) {
    uint32_t length;
    int result = data_u32(data, position, limit, &length);
    if (result < 0) return result;
    size_t start = position + 4U;
    if (start > limit || length >= limit - start) return -EBADMSG;
    if (data[start + length] != 0) return -EBADMSG;
    *text = (const char *)(data + start);
    *next = start + length + 1U;
    return 0;
}

static int data_signature(const uint8_t *data, size_t position, size_t limit,
                          const char **signature, size_t *next) {
    if (position >= limit) return -EBADMSG;
    size_t length = data[position];
    size_t start = position + 1U;
    if (start > limit || length >= limit - start) return -EBADMSG;
    if (data[start + length] != 0) return -EBADMSG;
    *signature = (const char *)(data + start);
    *next = start + length + 1U;
    return 0;
}

static size_t signature_alignment(char type) {
    switch (type) {
    case 'y': case 'g': case 'v': return 1U;
    case 'n': case 'q': return 2U;
    case 'b': case 'i': case 'u': case 's': case 'o': case 'a': case 'h':
        return 4U;
    case 'x': case 't': case 'd': case 'r': case 'e': case '(': case '{':
        return 8U;
    default: return 0U;
    }
}

static const char *signature_after(const char *signature) {
    if (!signature || !*signature) return NULL;
    switch (*signature) {
    case 'y': case 'b': case 'n': case 'q': case 'i': case 'u': case 'x':
    case 't': case 'd': case 's': case 'o': case 'g': case 'h': case 'v':
        return signature + 1;
    case 'a':
        return signature_after(signature + 1);
    case '(':
    case '{': {
        char close = *signature == '(' ? ')' : '}';
        const char *cursor = signature + 1;
        while (*cursor && *cursor != close) {
            cursor = signature_after(cursor);
            if (!cursor) return NULL;
        }
        if (*cursor != close) return NULL;
        return cursor + 1;
    }
    default:
        return NULL;
    }
}

static int position_after(const uint8_t *data, size_t position,
                          const char *signature, size_t limit,
                          size_t *next);

static int position_after_type(const uint8_t *data, size_t position,
                               const char **signature, size_t limit,
                               size_t *next) {
    char type = **signature;
    size_t alignment = signature_alignment(type);
    if (!alignment) return -EINVAL;
    position = align_up(position, alignment);
    switch (type) {
    case 'y': case 'b': case 'i': case 'u': case 'h': case 'n': case 'q':
    case 'x': case 't': case 'd': {
        size_t size = type == 'y' ? 1U : type == 'n' || type == 'q' ? 2U :
                      type == 'x' || type == 't' || type == 'd' ? 8U : 4U;
        if (position > limit || size > limit - position) return -EBADMSG;
        position += size;
        break;
    }
    case 's':
    case 'o': {
        const char *text;
        size_t after;
        int result = data_text(data, position, limit, &text, &after);
        if (result < 0) return result;
        position = after;
        break;
    }
    case 'g': {
        const char *text;
        size_t after;
        int result = data_signature(data, position, limit, &text, &after);
        if (result < 0) return result;
        position = after;
        break;
    }
    case 'v': {
        const char *inner;
        size_t start;
        int result = data_signature(data, position, limit, &inner, &start);
        if (result < 0) return result;
        result = position_after(data, start, inner, limit, &position);
        if (result < 0) return result;
        break;
    }
    case 'a': {
        uint32_t length;
        int result = data_u32(data, position, limit, &length);
        if (result < 0) return result;
        (*signature)++;
        size_t element_alignment = signature_alignment(**signature);
        if (!element_alignment) return -EBADMSG;
        const char *element = *signature;
        const char *after_element = signature_after(element);
        if (!after_element) return -EBADMSG;
        size_t start = align_up(position + 4U, element_alignment);
        if (start > limit || length > limit - start) return -EBADMSG;
        position = start + length;
        *signature = after_element;
        *next = position;
        return 0;
    }
    case '(':
    case '{': {
        char close = type == '(' ? ')' : '}';
        (*signature)++;
        while (**signature && **signature != close) {
            int result = position_after_type(data, position, signature, limit,
                                             &position);
            if (result < 0) return result;
        }
        if (**signature != close) return -EBADMSG;
        (*signature)++;
        break;
    }
    default:
        return -EBADMSG;
    }
    (*signature)++;
    *next = position;
    return 0;
}

static int position_after(const uint8_t *data, size_t position,
                          const char *signature, size_t limit,
                          size_t *next) {
    const char *cursor = signature;
    if (!cursor || !*cursor) return -EINVAL;
    while (*cursor) {
        int result = position_after_type(data, position, &cursor, limit,
                                         &position);
        if (result < 0) return result;
    }
    *next = position;
    return 0;
}

static int basic_type(char type) {
    switch (type) {
    case 'y': case 'b': case 'n': case 'q': case 'i': case 'u': case 'x':
    case 't': case 'd': case 's': case 'o': case 'g': case 'h':
        return 1;
    default:
        return 0;
    }
}

static int message_push(struct tomoe_bus_message *message, size_t end,
                        const char *signature, size_t position, int repeat) {
    if (message->depth >= BUS_MAX_DEPTH) return -ENOBUFS;
    struct bus_frame *frame = &message->frames[message->depth++];
    frame->end = end;
    frame->signature = signature;
    frame->position = position;
    frame->repeat = repeat;
    return 0;
}

static size_t message_limit(const struct tomoe_bus_message *message) {
    if (message->depth <= 0) return message->body_end;
    return message->frames[message->depth - 1].end;
}

static int parent_advance(struct tomoe_bus_message *message) {
    if (message->depth <= 0) return -EBADMSG;
    struct bus_frame *frame = &message->frames[message->depth - 1];
    if (frame->repeat) return 0;
    if (!frame->signature) return -EBADMSG;
    const char *next = signature_after(frame->signature + frame->position);
    if (!next) return -EBADMSG;
    frame->position = (size_t)(next - frame->signature);
    return 0;
}

static int message_read_basic(struct tomoe_bus_message *message, char type,
                              va_list *ap) {
    struct bus_frame *frame = &message->frames[message->depth - 1];
    size_t alignment = signature_alignment(type);
    size_t position = align_up(message->cursor, alignment);
    if (position >= frame->end) return 0;
    switch (type) {
    case 'y': {
        if (frame->end - position < 1U) return -EBADMSG;
        uint8_t *out = va_arg(*ap, uint8_t *);
        *out = message->data[position];
        position += 1U;
        break;
    }
    case 'b': {
        uint32_t value;
        int result = data_u32(message->data, position, frame->end, &value);
        if (result < 0) return result;
        int *out = va_arg(*ap, int *);
        *out = value != 0;
        position += 4U;
        break;
    }
    case 'n': {
        if (frame->end - position < 2U) return -EBADMSG;
        int16_t *out = va_arg(*ap, int16_t *);
        *out = (int16_t)((uint16_t)message->data[position] |
                         (uint16_t)message->data[position + 1U] << 8);
        position += 2U;
        break;
    }
    case 'q': {
        if (frame->end - position < 2U) return -EBADMSG;
        uint16_t *out = va_arg(*ap, uint16_t *);
        *out = (uint16_t)((uint16_t)message->data[position] |
                          (uint16_t)message->data[position + 1U] << 8);
        position += 2U;
        break;
    }
    case 'i': {
        uint32_t value;
        int result = data_u32(message->data, position, frame->end, &value);
        if (result < 0) return result;
        int32_t *out = va_arg(*ap, int32_t *);
        *out = (int32_t)value;
        position += 4U;
        break;
    }
    case 'u':
    case 'h': {
        uint32_t value;
        int result = data_u32(message->data, position, frame->end, &value);
        if (result < 0) return result;
        uint32_t *out = va_arg(*ap, uint32_t *);
        *out = value;
        position += 4U;
        break;
    }
    case 'x': {
        uint64_t value;
        int result = data_u64(message->data, position, frame->end, &value);
        if (result < 0) return result;
        int64_t *out = va_arg(*ap, int64_t *);
        *out = (int64_t)value;
        position += 8U;
        break;
    }
    case 't': {
        uint64_t value;
        int result = data_u64(message->data, position, frame->end, &value);
        if (result < 0) return result;
        uint64_t *out = va_arg(*ap, uint64_t *);
        *out = value;
        position += 8U;
        break;
    }
    case 'd': {
        uint64_t value;
        int result = data_u64(message->data, position, frame->end, &value);
        if (result < 0) return result;
        double *out = va_arg(*ap, double *);
        memcpy(out, &value, sizeof(value));
        position += 8U;
        break;
    }
    case 's':
    case 'o': {
        const char *text;
        size_t after;
        int result = data_text(message->data, position, frame->end, &text,
                               &after);
        if (result < 0) return result;
        const char **out = va_arg(*ap, const char **);
        *out = text;
        position = after;
        break;
    }
    case 'g': {
        const char *text;
        size_t after;
        int result = data_signature(message->data, position, frame->end, &text,
                                    &after);
        if (result < 0) return result;
        const char **out = va_arg(*ap, const char **);
        *out = text;
        position = after;
        break;
    }
    default:
        return -EINVAL;
    }
    message->cursor = position;
    return 1;
}

static int message_read_value(struct tomoe_bus_message *message, char type,
                              va_list *ap) {
    struct bus_frame *frame = &message->frames[message->depth - 1];
    size_t alignment = signature_alignment(type);
    if (align_up(message->cursor, alignment) >= frame->end) return 0;
    int result = message_read_basic(message, type, ap);
    if (result <= 0) return result;
    result = parent_advance(message);
    return result < 0 ? result : 1;
}

static int message_read_va(struct tomoe_bus_message *message,
                           const char *signature, va_list *ap) {
    int result = 0;
    for (const char *cursor = signature; *cursor; cursor++) {
        if (!basic_type(*cursor)) return -EINVAL;
        result = message_read_value(message, *cursor, ap);
        if (result <= 0) return result;
    }
    return 1;
}

static int message_enter(struct tomoe_bus_message *message, char type,
                         const char *contents) {
    size_t limit = message_limit(message);
    size_t position = message->cursor;
    if (position >= limit) return 0;
    size_t start = position;
    size_t end = position;
    const char *inner = contents;
    if (type == 'a') {
        if (!contents) return -EINVAL;
        size_t alignment = signature_alignment(*contents);
        if (!alignment) return -EINVAL;
        position = align_up(position, 4U);
        uint32_t length;
        int result = data_u32(message->data, position, limit, &length);
        if (result < 0) return result;
        start = align_up(position + 4U, alignment);
        if (start > limit || length > limit - start) return -EBADMSG;
        end = start + length;
    } else if (type == TOMOE_BUS_STRUCT || type == TOMOE_BUS_DICT_ENTRY) {
        if (!contents) return -EINVAL;
        start = align_up(position, 8U);
        int result = position_after(message->data, start, contents, limit, &end);
        if (result < 0) return result;
    } else if (type == 'v') {
        const char *wire;
        size_t after;
        int result = data_signature(message->data, position, limit, &wire,
                                    &after);
        if (result < 0) return result;
        if (contents && strcmp(contents, wire)) return -EBADMSG;
        result = position_after(message->data, after, wire, limit, &end);
        if (result < 0) return result;
        start = after;
        inner = wire;
    } else {
        return -EINVAL;
    }
    int result = parent_advance(message);
    if (result < 0) return result;
    result = message_push(message, end, inner, 0, type == 'a');
    if (result < 0) return result;
    message->cursor = start;
    return 1;
}

static int signature_record(struct tomoe_bus_message *message,
                            const char *text) {
    size_t length = strlen(message->out_signature);
    size_t extra = strlen(text);
    if (length + extra + 1U > sizeof(message->out_signature)) return -E2BIG;
    memcpy(message->out_signature + length, text, extra + 1U);
    return 0;
}

static int message_write_text(struct tomoe_bus_message *message,
                              const char *text) {
    size_t length = strlen(text);
    if (length > UINT32_MAX) return -E2BIG;
    int result = buffer_u32(&message->body, (uint32_t)length);
    if (result < 0) return result;
    result = buffer_add(&message->body, text, length);
    if (result < 0) return result;
    return buffer_byte(&message->body, 0);
}

static int message_write_signature(struct tomoe_bus_message *message,
                                   const char *signature) {
    size_t length = strlen(signature);
    if (length > UINT8_MAX) return -E2BIG;
    int result = buffer_byte(&message->body, (uint8_t)length);
    if (result < 0) return result;
    result = buffer_add(&message->body, signature, length);
    if (result < 0) return result;
    return buffer_byte(&message->body, 0);
}

static int message_open_container(struct tomoe_bus_message *message, char type,
                                  const char *contents) {
    if (message->out_depth >= BUS_MAX_DEPTH) return -ENOBUFS;
    if (type == 'a') {
        if (!contents) return -EINVAL;
        size_t alignment = signature_alignment(*contents);
        if (!alignment) return -EINVAL;
        int result = buffer_align(&message->body, 4U);
        if (result < 0) return result;
        struct bus_out_frame *frame = &message->out_frames[message->out_depth];
        frame->type = 'a';
        frame->length_position = message->body.length;
        result = buffer_u32(&message->body, 0);
        if (result < 0) return result;
        result = buffer_align(&message->body, alignment);
        if (result < 0) return result;
        frame->data_start = message->body.length;
        message->out_depth++;
        int record = message->signature_suppress == 0;
        message->signature_suppress++;
        if (!record) return 0;
        result = signature_record(message, "a");
        if (result < 0) return result;
        return signature_record(message, contents);
    }
    if (type == TOMOE_BUS_STRUCT || type == TOMOE_BUS_DICT_ENTRY) {
        if (!contents) return -EINVAL;
        int result = buffer_align(&message->body, 8U);
        if (result < 0) return result;
        struct bus_out_frame *frame = &message->out_frames[message->out_depth];
        frame->type = type;
        frame->length_position = 0;
        frame->data_start = 0;
        message->out_depth++;
        int record = message->signature_suppress == 0;
        message->signature_suppress++;
        if (!record) return 0;
        result = signature_record(message,
                                  type == TOMOE_BUS_STRUCT ? "(" : "{");
        if (result < 0) return result;
        result = signature_record(message, contents);
        if (result < 0) return result;
        return signature_record(message,
                                type == TOMOE_BUS_STRUCT ? ")" : "}");
    }
    if (type == 'v') {
        if (!contents) return -EINVAL;
        int result = message_write_signature(message, contents);
        if (result < 0) return result;
        struct bus_out_frame *frame = &message->out_frames[message->out_depth];
        frame->type = 'v';
        frame->length_position = 0;
        frame->data_start = 0;
        message->out_depth++;
        int record = message->signature_suppress == 0;
        message->signature_suppress++;
        if (!record) return 0;
        return signature_record(message, "v");
    }
    return -EINVAL;
}

int tomoe_message_open_container(struct tomoe_bus_message *message, char type,
                                 const char *contents) {
    if (!message) return -EINVAL;
    int result = message_open_container(message, type, contents);
    return result < 0 ? result : 1;
}

int tomoe_message_close_container(struct tomoe_bus_message *message) {
    if (!message) return -EINVAL;
    if (message->out_depth <= 0) return -EINVAL;
    struct bus_out_frame *frame = &message->out_frames[--message->out_depth];
    if (message->signature_suppress > 0) message->signature_suppress--;
    if (frame->type == 'a') {
        size_t length = message->body.length - frame->data_start;
        if (length > UINT32_MAX) return -E2BIG;
        message->body.data[frame->length_position] = (uint8_t)length;
        message->body.data[frame->length_position + 1U] = (uint8_t)(length >> 8);
        message->body.data[frame->length_position + 2U] = (uint8_t)(length >> 16);
        message->body.data[frame->length_position + 3U] = (uint8_t)(length >> 24);
    }
    return 1;
}

static int message_append_basic(struct tomoe_bus_message *message, char type,
                                va_list *ap) {
    size_t alignment = signature_alignment(type);
    if (!alignment) return -EINVAL;
    uint8_t bytes[8] = {0};
    const void *data = bytes;
    size_t length = 0;
    switch (type) {
    case 'y': {
        bytes[0] = (uint8_t)va_arg(*ap, int);
        length = 1;
        break;
    }
    case 'b': {
        int value = va_arg(*ap, int);
        uint32_t encoded = value ? 1U : 0U;
        for (size_t index = 0; index < 4U; index++)
            bytes[index] = (uint8_t)(encoded >> (index * 8U));
        length = 4;
        break;
    }
    case 'n': {
        uint16_t value = (uint16_t)va_arg(*ap, int);
        bytes[0] = (uint8_t)value;
        bytes[1] = (uint8_t)(value >> 8);
        length = 2;
        break;
    }
    case 'q': {
        uint16_t value = (uint16_t)va_arg(*ap, int);
        bytes[0] = (uint8_t)value;
        bytes[1] = (uint8_t)(value >> 8);
        length = 2;
        break;
    }
    case 'i': {
        uint32_t value = (uint32_t)va_arg(*ap, int32_t);
        for (size_t index = 0; index < 4U; index++)
            bytes[index] = (uint8_t)(value >> (index * 8U));
        length = 4;
        break;
    }
    case 'u': {
        uint32_t value = (uint32_t)va_arg(*ap, uint32_t);
        for (size_t index = 0; index < 4U; index++)
            bytes[index] = (uint8_t)(value >> (index * 8U));
        length = 4;
        break;
    }
    case 'x': {
        uint64_t value = (uint64_t)va_arg(*ap, int64_t);
        for (size_t index = 0; index < 8U; index++)
            bytes[index] = (uint8_t)(value >> (index * 8U));
        length = 8;
        break;
    }
    case 't': {
        uint64_t value = va_arg(*ap, uint64_t);
        for (size_t index = 0; index < 8U; index++)
            bytes[index] = (uint8_t)(value >> (index * 8U));
        length = 8;
        break;
    }
    case 'd': {
        double value = va_arg(*ap, double);
        uint64_t bits;
        memcpy(&bits, &value, sizeof(bits));
        for (size_t index = 0; index < 8U; index++)
            bytes[index] = (uint8_t)(bits >> (index * 8U));
        length = 8;
        break;
    }
    default:
        break;
    }
    int result = buffer_align(&message->body, alignment);
    if (result < 0) return result;
    if (length) {
        result = buffer_add(&message->body, data, length);
        if (result < 0) return result;
    } else if (type == 's' || type == 'o') {
        const char *value = va_arg(*ap, const char *);
        if (!value) return -EINVAL;
        result = message_write_text(message, value);
        if (result < 0) return result;
    } else if (type == 'g') {
        const char *value = va_arg(*ap, const char *);
        if (!value) return -EINVAL;
        result = message_write_signature(message, value);
        if (result < 0) return result;
    } else if (type == 'h') {
        int value = va_arg(*ap, int);
        uint32_t encoded = (uint32_t)value;
        for (size_t index = 0; index < 4U; index++)
            bytes[index] = (uint8_t)(encoded >> (index * 8U));
        result = buffer_add(&message->body, bytes, 4U);
        if (result < 0) return result;
    } else {
        return -EINVAL;
    }
    if (message->signature_suppress) return 0;
    char text[2] = { type, 0 };
    return signature_record(message, text);
}

static int message_append_one(struct tomoe_bus_message *message,
                              const char **cursor, va_list *ap);

static int message_append_container_args(struct tomoe_bus_message *message,
                                         char type, const char *contents,
                                         va_list *ap) {
    int result = message_open_container(message, type, contents);
    if (result < 0) return result;
    if (type == 'a') {
        size_t count = va_arg(*ap, size_t);
        for (size_t index = 0; index < count; index++) {
            const char *inner = contents;
            result = message_append_one(message, &inner, ap);
            if (result < 0) return result;
        }
        return tomoe_message_close_container(message);
    }
    const char *inner = contents;
    while (*inner) {
        result = message_append_one(message, &inner, ap);
        if (result < 0) return result;
    }
    return tomoe_message_close_container(message);
}

static int message_append_one(struct tomoe_bus_message *message,
                              const char **cursor, va_list *ap) {
    if (!**cursor) return -EINVAL;
    if (**cursor == 'a') {
        const char *contents = *cursor + 1;
        const char *after = signature_after(contents);
        if (!after) return -EINVAL;
        size_t length = (size_t)(after - contents);
        if (length >= BUS_MAX_SIGNATURE) return -E2BIG;
        char element[BUS_MAX_SIGNATURE];
        memcpy(element, contents, length);
        element[length] = '\0';
        int result = message_append_container_args(message, 'a', element, ap);
        *cursor = after;
        return result;
    }
    if (**cursor == '(' || **cursor == '{') {
        char open = **cursor;
        char close = open == '(' ? ')' : '}';
        const char *contents = *cursor + 1;
        const char *end = contents;
        while (*end && *end != close) {
            end = signature_after(end);
            if (!end) return -EINVAL;
        }
        if (*end != close) return -EINVAL;
        size_t length = (size_t)(end - contents);
        if (length >= BUS_MAX_SIGNATURE) return -E2BIG;
        char inner[BUS_MAX_SIGNATURE];
        memcpy(inner, contents, length);
        inner[length] = '\0';
        int result = message_append_container_args(
            message, open == '(' ? TOMOE_BUS_STRUCT : TOMOE_BUS_DICT_ENTRY,
            inner, ap);
        *cursor = end + 1;
        return result;
    }
    if (**cursor == 'v') {
        const char *inner = va_arg(*ap, const char *);
        if (!inner) return -EINVAL;
        int result = message_append_container_args(message, 'v', inner, ap);
        (*cursor)++;
        return result;
    }
    int result = message_append_basic(message, **cursor, ap);
    if (result < 0) return result;
    (*cursor)++;
    return 0;
}

static int message_append_va(struct tomoe_bus_message *message,
                             const char *signature, va_list *ap) {
    const char *cursor = signature;
    while (*cursor) {
        int result = message_append_one(message, &cursor, ap);
        if (result < 0) return result;
    }
    return 0;
}

static int field_add(struct bus_buffer *fields, uint8_t code, char type,
                     const void *value) {
    int result = buffer_align(fields, 8U);
    if (result < 0) return result;
    result = buffer_byte(fields, code);
    if (result < 0) return result;
    result = buffer_byte(fields, 1);
    if (result < 0) return result;
    result = buffer_byte(fields, (uint8_t)type);
    if (result < 0) return result;
    result = buffer_byte(fields, 0);
    if (result < 0) return result;
    result = buffer_align(fields, signature_alignment(type));
    if (result < 0) return result;
    switch (type) {
    case 'u':
        return buffer_u32(fields, *(const uint32_t *)value);
    case 's':
    case 'o': {
        const char *text = value;
        size_t length = strlen(text);
        if (length > UINT32_MAX) return -E2BIG;
        result = buffer_u32(fields, (uint32_t)length);
        if (result < 0) return result;
        result = buffer_add(fields, text, length);
        if (result < 0) return result;
        return buffer_byte(fields, 0);
    }
    case 'g': {
        const char *text = value;
        size_t length = strlen(text);
        if (length > UINT8_MAX) return -E2BIG;
        result = buffer_byte(fields, (uint8_t)length);
        if (result < 0) return result;
        result = buffer_add(fields, text, length);
        if (result < 0) return result;
        return buffer_byte(fields, 0);
    }
    default:
        return -EINVAL;
    }
}

static void slot_free(struct tomoe_bus_slot *slot) {
    if (slot->kind == BUS_SLOT_OBJECT) {
        free(slot->path);
        free(slot->interface);
    }
    if (slot->kind == BUS_SLOT_CALL && slot->collected)
        tomoe_message_unref(slot->collected);
    free(slot);
}

static void slot_unlink(struct tomoe_bus_slot *slot) {
    if (!slot->bus) return;
    struct tomoe_bus_slot **cursor = &slot->bus->slots;
    while (*cursor) {
        if (*cursor == slot) {
            *cursor = slot->next;
            slot->next = NULL;
            slot->bus = NULL;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

void tomoe_bus_slot_unref(struct tomoe_bus_slot *slot) {
    if (!slot) return;
    if (--slot->refs) return;
    if (slot->running) return;
    slot_unlink(slot);
    slot_free(slot);
}

static int bus_connect_socket(const char *address, int *error) {
    if (!address) {
        *error = EINVAL;
        return -1;
    }
    const char *cursor = address;
    if (!strncmp(cursor, "unix:", 5)) cursor += 5;
    else {
        *error = EOPNOTSUPP;
        return -1;
    }
    const char *path = NULL;
    const char *abstract = NULL;
    size_t path_length = 0, abstract_length = 0;
    while (*cursor && *cursor != ';') {
        const char *field = cursor;
        const char *end = field + strcspn(field, ",;");
        const char *equal = memchr(field, '=', (size_t)(end - field));
        if (!equal) {
            *error = EINVAL;
            return -1;
        }
        size_t key = (size_t)(equal - field);
        const char *value = equal + 1;
        if (key == 4 && !strncmp(field, "path", 4)) {
            path = value;
            path_length = (size_t)(end - value);
        } else if (key == 8 && !strncmp(field, "abstract", 8)) {
            abstract = value;
            abstract_length = (size_t)(end - value);
        }
        if (*end != ',') break;
        cursor = end + 1;
    }
    struct sockaddr_un local = { .sun_family = AF_UNIX };
    size_t length = 0;
    if (abstract) {
        length = abstract_length;
        if (length + 2U > sizeof(local.sun_path)) {
            *error = ENAMETOOLONG;
            return -1;
        }
        local.sun_path[0] = '\0';
        memcpy(local.sun_path + 1, abstract, length);
        length += 1U;
    } else {
        char decoded[sizeof(local.sun_path)];
        size_t written = 0;
        for (const char *source = path; source && source < path + path_length; source++) {
            if (*source == '%' && source + 2 < path + path_length) {
                int value = 0;
                for (int index = 0; index < 2; index++) {
                    int digit = source[1 + index];
                    if (digit >= '0' && digit <= '9') digit -= '0';
                    else if (digit >= 'A' && digit <= 'F') digit -= 'A' - 10;
                    else if (digit >= 'a' && digit <= 'f') digit -= 'a' - 10;
                    else {
                        *error = EINVAL;
                        return -1;
                    }
                    value = value << 4 | digit;
                }
                if (written + 1U >= sizeof(decoded)) {
                    *error = ENAMETOOLONG;
                    return -1;
                }
                decoded[written++] = (char)value;
                source += 2;
            } else {
                if (written + 1U >= sizeof(decoded)) {
                    *error = ENAMETOOLONG;
                    return -1;
                }
                decoded[written++] = *source;
            }
        }
        if (!path || !written) {
            *error = EINVAL;
            return -1;
        }
        decoded[written] = '\0';
        memcpy(local.sun_path, decoded, written + 1U);
        length = written;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = errno;
        return -1;
    }
    socklen_t address_length =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length);
    if (connect(fd, (struct sockaddr *)&local, address_length) < 0) {
        *error = errno;
        close(fd);
        return -1;
    }
    return fd;
}

static int bus_read_line(int fd, char *buffer, size_t capacity) {
    size_t used = 0;
    while (used + 1U < capacity) {
        ssize_t count = read(fd, buffer + used, capacity - used - 1U);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (!count) return -ECONNRESET;
        used += (size_t)count;
        buffer[used] = '\0';
        if (memchr(buffer, '\n', used)) return 0;
    }
    return -ENOBUFS;
}

static int bus_authenticate(int fd) {
    char uid[32];
    int length = snprintf(uid, sizeof(uid), "%u", (unsigned)getuid());
    if (length <= 0 || (size_t)length >= sizeof(uid)) return -EINVAL;
    char header[64];
    int header_length = snprintf(header, sizeof(header), "AUTH EXTERNAL ");
    size_t written = 0;
    char line[160];
    line[written++] = '\0';
    memcpy(line + written, header, (size_t)header_length);
    written += (size_t)header_length;
    for (int index = 0; index < length; index++) {
        static const char digits[] = "0123456789ABCDEF";
        unsigned char byte = (unsigned char)uid[index];
        line[written++] = digits[byte >> 4];
        line[written++] = digits[byte & 0x0fU];
    }
    line[written++] = '\r';
    line[written++] = '\n';
    if (write(fd, line, written) != (ssize_t)written) return -EIO;
    char response[512];
    int result = bus_read_line(fd, response, sizeof(response));
    if (result < 0) return result;
    if (!strncmp(response, "REJECTED", 8) || !strncmp(response, "ERROR", 5))
        return -EACCES;
    if (strncmp(response, "OK ", 3)) return -EACCES;
    static const char begin[] = "BEGIN\r\n";
    if (write(fd, begin, sizeof(begin) - 1U) != (ssize_t)(sizeof(begin) - 1U))
        return -EIO;
    return 0;
}

static int bus_try_flush(struct tomoe_bus *bus) {
    while (bus->output_start < bus->output.length) {
        ssize_t count = send(bus->fd, bus->output.data + bus->output_start,
                             bus->output.length - bus->output_start,
                             MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -EAGAIN;
            return -errno;
        }
        if (!count) return -EIO;
        bus->output_start += (size_t)count;
    }
    bus->output.length = 0;
    bus->output_start = 0;
    return 0;
}

static int bus_queue(struct tomoe_bus *bus, const void *data, size_t length) {
    int result = buffer_add(&bus->output, data, length);
    if (result < 0) return result;
    result = bus_try_flush(bus);
    if (result == -EAGAIN) return 0;
    return result;
}

static int message_seal(struct tomoe_bus *bus,
                        struct tomoe_bus_message *message) {
    if (message->out_depth) return -EBADMSG;
    if (message->discard) return 0;
    struct bus_buffer fields = {0};
    int result = 0;
    message->serial = ++bus->next_serial;
    if (!message->serial) message->serial = ++bus->next_serial;
    if (message->path) {
        result = field_add(&fields, BUS_FIELD_PATH, 'o', message->path);
        if (result < 0) goto failed;
    }
    if (message->interface) {
        result = field_add(&fields, BUS_FIELD_INTERFACE, 's',
                           message->interface);
        if (result < 0) goto failed;
    }
    if (message->member) {
        result = field_add(&fields, BUS_FIELD_MEMBER, 's', message->member);
        if (result < 0) goto failed;
    }
    if (message->error_name) {
        result = field_add(&fields, BUS_FIELD_ERROR, 's', message->error_name);
        if (result < 0) goto failed;
    }
    if (message->reply_serial) {
        uint32_t serial = message->reply_serial;
        result = field_add(&fields, BUS_FIELD_REPLY, 'u', &serial);
        if (result < 0) goto failed;
    }
    if (message->destination) {
        result = field_add(&fields, BUS_FIELD_DESTINATION, 's',
                           message->destination);
        if (result < 0) goto failed;
    }
    if (*message->out_signature) {
        result = field_add(&fields, BUS_FIELD_SIGNATURE, 'g',
                           message->out_signature);
        if (result < 0) goto failed;
    }
    size_t body_length = message->body.length;
    if (body_length > UINT32_MAX) {
        result = -E2BIG;
        goto failed;
    }
    uint8_t fixed[16];
    fixed[0] = 'l';
    fixed[1] = message->type;
    fixed[2] = message->type == BUS_TYPE_CALL ? 0 : 1;
    fixed[3] = 1;
    uint32_t length32 = (uint32_t)body_length;
    uint32_t serial = message->serial;
    uint32_t fields_length = (uint32_t)fields.length;
    for (size_t index = 0; index < 4U; index++) {
        fixed[4 + index] = (uint8_t)(length32 >> (index * 8U));
        fixed[8 + index] = (uint8_t)(serial >> (index * 8U));
        fixed[12 + index] = (uint8_t)(fields_length >> (index * 8U));
    }
    result = bus_queue(bus, fixed, sizeof(fixed));
    if (result < 0) goto failed;
    result = bus_queue(bus, fields.data, fields.length);
    if (result < 0) goto failed;
    size_t padding = (8U - ((16U + fields.length) % 8U)) % 8U;
    if (padding) {
        static const uint8_t zeros[8] = {0};
        result = bus_queue(bus, zeros, padding);
        if (result < 0) goto failed;
    }
    result = bus_queue(bus, message->body.data, message->body.length);
failed:
    free(fields.data);
    return result;
}

static struct tomoe_bus_message *message_new(struct tomoe_bus *bus,
                                             uint8_t type) {
    struct tomoe_bus_message *message = calloc(1, sizeof(*message));
    if (!message) return NULL;
    message->refs = 1;
    message->bus = bus;
    message->type = type;
    message->signature = message->out_signature;
    message->depth = 1;
    message->frames[0].end = 0;
    message->frames[0].signature = message->signature_buffer;
    message->frames[0].position = 0;
    return message;
}

struct tomoe_bus_message *tomoe_message_ref(struct tomoe_bus_message *message) {
    if (!message) return NULL;
    message->refs++;
    return message;
}

void tomoe_message_unref(struct tomoe_bus_message *message) {
    if (!message) return;
    if (--message->refs) return;
    free(message->path);
    free(message->interface);
    free(message->member);
    free(message->destination);
    free(message->error_name);
    free(message->sender);
    free(message->data);
    free(message->body.data);
    free(message);
}

const char *tomoe_message_sender(const struct tomoe_bus_message *message) {
    return message ? message->sender : NULL;
}

const char *tomoe_message_path(const struct tomoe_bus_message *message) {
    return message ? message->path : NULL;
}

const char *tomoe_message_interface(const struct tomoe_bus_message *message) {
    return message ? message->interface : NULL;
}

const char *tomoe_message_member(const struct tomoe_bus_message *message) {
    return message ? message->member : NULL;
}

const char *tomoe_message_error(const struct tomoe_bus_message *message) {
    return message ? message->error_name : NULL;
}

static int message_parse(struct tomoe_bus *bus, const uint8_t *data,
                         size_t size, struct tomoe_bus_message **out) {
    if (size < 16U || data[0] != 'l' || data[3] != 1) return -EBADMSG;
    uint32_t body_length;
    uint32_t serial;
    uint32_t fields_length;
    int result = data_u32(data, 4, size, &body_length);
    if (result < 0) return result;
    result = data_u32(data, 8, size, &serial);
    if (result < 0) return result;
    result = data_u32(data, 12, size, &fields_length);
    if (result < 0) return result;
    size_t fields_start = 16U;
    if (fields_length > size - fields_start) return -EBADMSG;
    size_t fields_end = fields_start + fields_length;
    size_t header_end = align_up(fields_end, 8U);
    if (header_end > size || body_length > size - header_end)
        return -EBADMSG;
    struct tomoe_bus_message *message = calloc(1, sizeof(*message));
    if (!message) return -ENOMEM;
    message->refs = 1;
    message->bus = bus;
    message->type = data[1];
    message->flags = data[2];
    message->serial = serial;
    if (message->type < BUS_TYPE_CALL || message->type > BUS_TYPE_SIGNAL) {
        free(message);
        return -EBADMSG;
    }
    message->size = size;
    message->data = malloc(size);
    if (!message->data) {
        free(message);
        return -ENOMEM;
    }
    memcpy(message->data, data, size);
    size_t position = fields_start;
    int has_signature = 0;
    while (position < fields_end) {
        position = align_up(position, 8U);
        if (position >= fields_end) break;
        uint8_t code = message->data[position++];
        const char *variant;
        size_t after;
        result = data_signature(message->data, position, fields_end, &variant,
                                &after);
        if (result < 0) goto failed;
        size_t value_at = align_up(after, signature_alignment(*variant));
        if (code == BUS_FIELD_PATH || code == BUS_FIELD_INTERFACE ||
            code == BUS_FIELD_MEMBER || code == BUS_FIELD_ERROR ||
            code == BUS_FIELD_DESTINATION || code == BUS_FIELD_SENDER) {
            const char *text;
            size_t next;
            result = data_text(message->data, value_at, fields_end, &text,
                               &next);
            if (result < 0) goto failed;
            char **target = code == BUS_FIELD_PATH ? &message->path :
                            code == BUS_FIELD_INTERFACE ? &message->interface :
                            code == BUS_FIELD_MEMBER ? &message->member :
                            code == BUS_FIELD_ERROR ? &message->error_name :
                            code == BUS_FIELD_DESTINATION ? &message->destination :
                            &message->sender;
            *target = text_copy(text);
            if (!*target) {
                result = -ENOMEM;
                goto failed;
            }
            position = next;
            continue;
        }
        if (code == BUS_FIELD_REPLY || code == BUS_FIELD_SIGNATURE) {
            if (code == BUS_FIELD_REPLY) {
                result = data_u32(message->data, value_at, fields_end,
                                  &message->reply_serial);
                if (result < 0) goto failed;
            } else {
                const char *text;
                size_t next;
                result = data_signature(message->data, value_at, fields_end,
                                        &text, &next);
                if (result < 0) goto failed;
                size_t length = strlen(text);
                if (length >= sizeof(message->signature_buffer)) {
                    result = -EBADMSG;
                    goto failed;
                }
                memcpy(message->signature_buffer, text, length + 1U);
                has_signature = 1;
            }
        }
        const char *cursor = variant;
        size_t end;
        result = position_after(message->data, value_at, cursor, fields_end,
                                &end);
        if (result < 0) goto failed;
        position = end;
    }
    if (!has_signature) message->signature_buffer[0] = '\0';
    message->signature = message->signature_buffer;
    message->body_start = header_end;
    message->body_end = header_end + body_length;
    message->cursor = header_end;
    message->depth = 1;
    message->frames[0].end = message->body_end;
    message->frames[0].signature = message->signature;
    message->frames[0].position = 0;
    *out = message;
    return 0;
failed:
    tomoe_message_unref(message);
    return result;
}

static int bus_read_input(struct tomoe_bus *bus) {
    uint8_t chunk[BUS_READ_CHUNK];
    for (;;) {
        ssize_t count = recv(bus->fd, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -errno;
        }
        if (!count) return -ECONNRESET;
        int result = buffer_add(&bus->input, chunk, (size_t)count);
        if (result < 0) return result;
    }
}

static int bus_dispatch_all(struct tomoe_bus *bus, int *count) {
    int processed = 0;
    for (;;) {
        size_t available = bus->input.length - bus->input_start;
        if (available < 16U) break;
        const uint8_t *data = bus->input.data + bus->input_start;
        uint32_t body_length;
        uint32_t fields_length;
        int result = data_u32(data, 4, available, &body_length);
        if (result < 0) return -EPROTO;
        result = data_u32(data, 12, available, &fields_length);
        if (result < 0) return -EPROTO;
        size_t header_end = align_up(16U + fields_length, 8U);
        if (header_end > BUS_MAX_MESSAGE ||
            body_length > BUS_MAX_MESSAGE - header_end)
            return -EMSGSIZE;
        size_t total = header_end + body_length;
        if (available < total) break;
        struct tomoe_bus_message *message = NULL;
        result = message_parse(bus, data, total, &message);
        if (result < 0) return result;
        bus->input_start += total;
        dispatch_message(bus, message);
        tomoe_message_unref(message);
        processed++;
    }
    if (bus->input_start == bus->input.length) {
        bus->input.length = 0;
        bus->input_start = 0;
    } else if (bus->input_start > 65536U) {
        memmove(bus->input.data, bus->input.data + bus->input_start,
                bus->input.length - bus->input_start);
        bus->input.length -= bus->input_start;
        bus->input_start = 0;
    }
    *count = processed;
    return 0;
}

int tomoe_bus_process(struct tomoe_bus *bus) {
    if (!bus) return -EINVAL;
    if (bus->failed) return -bus->failed;
    if (bus->closed) return -ENOTCONN;
    int result = bus_try_flush(bus);
    if (result < 0 && result != -EAGAIN) {
        bus->failed = -result;
        return result;
    }
    result = bus_read_input(bus);
    if (result < 0) {
        bus->failed = -result;
        return result;
    }
    int count = 0;
    result = bus_dispatch_all(bus, &count);
    if (result < 0) {
        bus->failed = -result;
        return result;
    }
    return count;
}

int tomoe_bus_wait(struct tomoe_bus *bus, uint64_t usec) {
    if (!bus || bus->fd < 0) return -EINVAL;
    int timeout = -1;
    if (usec != UINT64_MAX) {
        uint64_t milliseconds = usec / 1000U + (usec % 1000U ? 1U : 0U);
        timeout = milliseconds > (uint64_t)INT_MAX ? INT_MAX : (int)milliseconds;
    }
    struct pollfd descriptor = {
        .fd = bus->fd,
        .events = POLLIN |
                  (bus->output_start < bus->output.length ? POLLOUT : 0),
        .revents = 0
    };
    int result = poll(&descriptor, 1, timeout);
    if (result < 0) return errno == EINTR ? -EINTR : -errno;
    return result;
}

int tomoe_bus_fd(const struct tomoe_bus *bus) {
    return bus && !bus->closed ? bus->fd : -1;
}

int tomoe_bus_events(const struct tomoe_bus *bus) {
    int events = POLLIN;
    if (bus && bus->output_start < bus->output.length) events |= POLLOUT;
    return events;
}

int tomoe_bus_flush(struct tomoe_bus *bus) {
    if (!bus || bus->closed) return -ENOTCONN;
    int result = bus_try_flush(bus);
    return result == -EAGAIN ? 0 : result;
}

int tomoe_bus_set_timeout(struct tomoe_bus *bus, uint64_t usec) {
    if (!bus) return -EINVAL;
    bus->timeout = usec;
    return 0;
}

const char *tomoe_bus_unique_name(const struct tomoe_bus *bus) {
    return bus ? bus->unique : NULL;
}

static const char *error_name_for(int error) {
    switch (-error) {
    case EINVAL: return "org.freedesktop.DBus.Error.InvalidArgs";
    case ENOMEM: return "org.freedesktop.DBus.Error.NoMemory";
    case EACCES: return "org.freedesktop.DBus.Error.AccessDenied";
    case ENOTSUP: return "org.freedesktop.DBus.Error.NotSupported";
    case ENOSPC: return "org.freedesktop.DBus.Error.LimitsExceeded";
    default: return "org.freedesktop.DBus.Error.Failed";
    }
}

static int object_method(struct tomoe_bus_slot *object,
                         struct tomoe_bus_message *message) {
    for (size_t index = 0; index < object->method_count; index++) {
        const struct tomoe_bus_method *method = &object->methods[index];
        if (strcmp(method->member, message->member)) continue;
        if (message->signature && strcmp(message->signature, method->signature))
            return tomoe_reply_method_error(
                message, "org.freedesktop.DBus.Error.InvalidArgs",
                "signature %s does not match %s", message->signature,
                method->signature);
        object->running = 1;
        int result = method->call(message, object->userdata);
        object->running = 0;
        if (result < 0) {
            const char *name = error_name_for(result);
            return tomoe_reply_method_error(message, name, "%s",
                                            strerror(-result));
        }
        return 1;
    }
    return 0;
}

static int introspection_xml(struct tomoe_bus_slot *object,
                             struct bus_buffer *xml) {
    int result = buffer_text(xml,
        "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
        " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
        "<node>\n");
    if (result < 0) return result;
    result = buffer_text(xml, "  <interface name=\"");
    if (result < 0) return result;
    result = buffer_text(xml, object->interface);
    if (result < 0) return result;
    result = buffer_text(xml, "\">\n");
    if (result < 0) return result;
    for (size_t index = 0; index < object->method_count; index++) {
        const struct tomoe_bus_method *method = &object->methods[index];
        result = buffer_text(xml, "    <method name=\"");
        if (result < 0) return result;
        result = buffer_text(xml, method->member);
        if (result < 0) return result;
        result = buffer_text(xml, "\">\n");
        if (result < 0) return result;
        const char *cursor = method->signature;
        while (*cursor) {
            const char *after = signature_after(cursor);
            if (!after) return -EINVAL;
            result = buffer_text(xml, "      <arg type=\"");
            if (result < 0) return result;
            result = buffer_add(xml, cursor, (size_t)(after - cursor));
            if (result < 0) return result;
            result = buffer_text(xml, "\" direction=\"in\"/>\n");
            if (result < 0) return result;
            cursor = after;
        }
        cursor = method->result;
        while (*cursor) {
            const char *after = signature_after(cursor);
            if (!after) return -EINVAL;
            result = buffer_text(xml, "      <arg type=\"");
            if (result < 0) return result;
            result = buffer_add(xml, cursor, (size_t)(after - cursor));
            if (result < 0) return result;
            result = buffer_text(xml, "\" direction=\"out\"/>\n");
            if (result < 0) return result;
            cursor = after;
        }
        result = buffer_text(xml, "    </method>\n");
        if (result < 0) return result;
    }
    for (size_t index = 0; index < object->property_count; index++) {
        const struct tomoe_bus_property *property = &object->properties[index];
        result = buffer_text(xml, "    <property name=\"");
        if (result < 0) return result;
        result = buffer_text(xml, property->name);
        if (result < 0) return result;
        result = buffer_text(xml, "\" type=\"");
        if (result < 0) return result;
        result = buffer_text(xml, property->signature);
        if (result < 0) return result;
        result = buffer_text(xml, "\" access=\"read\"/>\n");
        if (result < 0) return result;
    }
    for (size_t index = 0; index < object->signal_count; index++) {
        const struct tomoe_bus_signal *signal = &object->signals[index];
        result = buffer_text(xml, "    <signal name=\"");
        if (result < 0) return result;
        result = buffer_text(xml, signal->name);
        if (result < 0) return result;
        if (*signal->signature) {
            result = buffer_text(xml, "\">\n");
            if (result < 0) return result;
            const char *cursor = signal->signature;
            while (*cursor) {
                const char *after = signature_after(cursor);
                if (!after) return -EINVAL;
                result = buffer_text(xml, "      <arg type=\"");
                if (result < 0) return result;
                result = buffer_add(xml, cursor, (size_t)(after - cursor));
                if (result < 0) return result;
                result = buffer_text(xml, "\"/>\n");
                if (result < 0) return result;
                cursor = after;
            }
            result = buffer_text(xml, "    </signal>\n");
            if (result < 0) return result;
        } else {
            result = buffer_text(xml, "\"/>\n");
            if (result < 0) return result;
        }
    }
    return buffer_text(xml,
        "  </interface>\n"
        "  <interface name=\"org.freedesktop.DBus.Peer\">\n"
        "    <method name=\"Ping\"/>\n"
        "    <method name=\"GetMachineId\">\n"
        "      <arg type=\"s\" name=\"machine_uuid\" direction=\"out\"/>\n"
        "    </method>\n"
        "  </interface>\n"
        "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
        "    <method name=\"Get\">\n"
        "      <arg type=\"s\" direction=\"in\"/>\n"
        "      <arg type=\"s\" direction=\"in\"/>\n"
        "      <arg type=\"v\" direction=\"out\"/>\n"
        "    </method>\n"
        "    <method name=\"GetAll\">\n"
        "      <arg type=\"s\" direction=\"in\"/>\n"
        "      <arg type=\"a{sv}\" direction=\"out\"/>\n"
        "    </method>\n"
        "    <method name=\"Set\">\n"
        "      <arg type=\"s\" direction=\"in\"/>\n"
        "      <arg type=\"s\" direction=\"in\"/>\n"
        "      <arg type=\"v\" direction=\"in\"/>\n"
        "    </method>\n"
        "    <signal name=\"PropertiesChanged\">\n"
        "      <arg type=\"s\"/>\n"
        "      <arg type=\"a{sv}\"/>\n"
        "      <arg type=\"as\"/>\n"
        "    </signal>\n"
        "  </interface>\n"
        "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
        "    <method name=\"Introspect\">\n"
        "      <arg type=\"s\" direction=\"out\"/>\n"
        "    </method>\n"
        "  </interface>\n"
        "</node>\n");
}

static const struct tomoe_bus_property *object_property(
    struct tomoe_bus_slot *object, const char *name) {
    for (size_t index = 0; index < object->property_count; index++)
        if (!strcmp(object->properties[index].name, name))
            return &object->properties[index];
    return NULL;
}

static int object_property_append(struct tomoe_bus_slot *object,
                                  struct tomoe_bus_message *reply,
                                  const struct tomoe_bus_property *property) {
    if (property->get)
        return property->get(reply, property->name, object->userdata);
    const void *value = (const char *)object->userdata + property->offset;
    switch (*property->signature) {
    case 'u': return tomoe_message_append(reply, "u", *(const uint32_t *)value);
    case 'i': return tomoe_message_append(reply, "i", *(const int32_t *)value);
    case 'b': return tomoe_message_append(reply, "b", *(const int *)value);
    case 'y': return tomoe_message_append(reply, "y", *(const uint8_t *)value);
    case 't': return tomoe_message_append(reply, "t", *(const uint64_t *)value);
    case 'x': return tomoe_message_append(reply, "x", *(const int64_t *)value);
    case 'd': return tomoe_message_append(reply, "d", *(const double *)value);
    case 's': return tomoe_message_append(reply, "s", *(const char *const *)value);
    case 'o': return tomoe_message_append(reply, "o", *(const char *const *)value);
    default: return -EINVAL;
    }
}

static int object_properties_get(struct tomoe_bus_slot *object,
                                 struct tomoe_bus_message *message,
                                 const char *interface, const char *name) {
    if (strcmp(interface, object->interface))
        return tomoe_reply_method_error(message,
            "org.freedesktop.DBus.Error.UnknownInterface",
            "no such interface %s", interface);
    const struct tomoe_bus_property *property = object_property(object, name);
    if (!property)
        return tomoe_reply_method_error(message,
            "org.freedesktop.DBus.Error.UnknownProperty",
            "no such property %s", name);
    struct tomoe_bus_message *reply = NULL;
    int result = tomoe_message_new_method_return(message, &reply);
    if (result < 0) return result;
    result = tomoe_message_open_container(reply, 'v', property->signature);
    if (result < 0) goto done;
    result = object_property_append(object, reply, property);
    if (result < 0) goto done;
    result = tomoe_message_close_container(reply);
    if (result < 0) goto done;
    result = message_seal(reply->bus, reply);
done:
    tomoe_message_unref(reply);
    return result < 0 ? result : 1;
}

static int object_properties_get_all(struct tomoe_bus_slot *object,
                                     struct tomoe_bus_message *message,
                                     const char *interface) {
    if (strcmp(interface, object->interface))
        return tomoe_reply_method_error(message,
            "org.freedesktop.DBus.Error.UnknownInterface",
            "no such interface %s", interface);
    struct tomoe_bus_message *reply = NULL;
    int result = tomoe_message_new_method_return(message, &reply);
    if (result < 0) return result;
    result = tomoe_message_open_container(reply, 'a', "{sv}");
    if (result < 0) goto done;
    for (size_t index = 0; index < object->property_count; index++) {
        const struct tomoe_bus_property *property = &object->properties[index];
        result = tomoe_message_open_container(reply, TOMOE_BUS_DICT_ENTRY,
                                              "sv");
        if (result < 0) goto done;
        result = tomoe_message_append(reply, "s", property->name);
        if (result < 0) goto done;
        result = tomoe_message_open_container(reply, 'v', property->signature);
        if (result < 0) goto done;
        result = object_property_append(object, reply, property);
        if (result < 0) goto done;
        result = tomoe_message_close_container(reply);
        if (result < 0) goto done;
        result = tomoe_message_close_container(reply);
        if (result < 0) goto done;
    }
    result = tomoe_message_close_container(reply);
    if (result < 0) goto done;
    result = message_seal(reply->bus, reply);
done:
    tomoe_message_unref(reply);
    return result < 0 ? result : 1;
}

static int object_properties(struct tomoe_bus_slot *object,
                             struct tomoe_bus_message *message) {
    if (!message->member) return -EINVAL;
    if (!strcmp(message->member, "Get")) {
        const char *interface = NULL;
        const char *name = NULL;
        if (tomoe_message_read(message, "ss", &interface, &name) <= 0)
            return tomoe_reply_method_error(
                message, "org.freedesktop.DBus.Error.InvalidArgs",
                "Properties.Get takes two strings");
        return object_properties_get(object, message, interface, name);
    }
    if (!strcmp(message->member, "GetAll")) {
        const char *interface = NULL;
        if (tomoe_message_read(message, "s", &interface) <= 0)
            return tomoe_reply_method_error(
                message, "org.freedesktop.DBus.Error.InvalidArgs",
                "Properties.GetAll takes one string");
        return object_properties_get_all(object, message, interface);
    }
    if (!strcmp(message->member, "Set"))
        return tomoe_reply_method_error(
            message, "org.freedesktop.DBus.Error.PropertyReadOnly",
            "properties are read-only");
    return 0;
}

static int object_introspect(struct tomoe_bus_slot *object,
                             struct tomoe_bus_message *message) {
    struct bus_buffer xml = {0};
    int result = introspection_xml(object, &xml);
    if (result < 0) {
        free(xml.data);
        return result;
    }
    tomoe_message_read(message, "");
    struct tomoe_bus_message *reply = NULL;
    result = tomoe_message_new_method_return(message, &reply);
    if (result >= 0) {
        uint8_t terminator = 0;
        result = buffer_add(&xml, &terminator, 1);
        if (result >= 0)
            result = tomoe_message_append(reply, "s", (const char *)xml.data);
        if (result >= 0) result = message_seal(reply->bus, reply);
    }
    tomoe_message_unref(reply);
    free(xml.data);
    return result < 0 ? result : 1;
}

static int peer_dispatch(struct tomoe_bus_message *message) {
    if (!message->member) return 0;
    if (!strcmp(message->member, "Ping"))
        return tomoe_reply_method_return(message, "");
    if (!strcmp(message->member, "GetMachineId")) {
        char machine[64] = {0};
        int fd = open("/etc/machine-id", O_RDONLY | O_CLOEXEC);
        if (fd < 0) fd = open("/var/lib/dbus/machine-id", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -ENOENT;
        ssize_t count = read(fd, machine, sizeof(machine) - 1U);
        close(fd);
        if (count <= 0) return -EIO;
        size_t length = 0;
        while (length < (size_t)count && machine[length] != '\n' &&
               machine[length] != '\r')
            length++;
        machine[length] = '\0';
        return tomoe_reply_method_return(message, "s", machine);
    }
    return 0;
}

static int object_dispatch(struct tomoe_bus_slot *object,
                           struct tomoe_bus_message *message) {
    int result = 0;
    if (message->interface &&
        !strcmp(message->interface, "org.freedesktop.DBus.Peer"))
        result = peer_dispatch(message);
    else if (message->interface &&
        !strcmp(message->interface, "org.freedesktop.DBus.Properties"))
        result = object_properties(object, message);
    else if (message->interface &&
             !strcmp(message->interface,
                     "org.freedesktop.DBus.Introspectable"))
        result = object_introspect(object, message);
    else if (!message->interface ||
             !strcmp(message->interface, object->interface))
        result = object_method(object, message);
    if (result == 0)
        result = tomoe_reply_method_error(
            message, "org.freedesktop.DBus.Error.UnknownMethod",
            "no method %s", message->member ? message->member : "");
    return result;
}

static int rule_matches(const struct bus_rule *rule,
                        struct tomoe_bus_message *message) {
    if (rule->has_type && message->type != BUS_TYPE_SIGNAL) return 0;
    if (rule->has_sender) {
        if (!message->sender) return 0;
        if (rule->sender[0] == ':' && strcmp(rule->sender, message->sender))
            return 0;
    }
    if (rule->has_path &&
        (!message->path || strcmp(rule->path, message->path))) return 0;
    if (rule->has_path_namespace) {
        if (!message->path) return 0;
        size_t length = strlen(rule->path_namespace);
        if (strncmp(rule->path_namespace, message->path, length)) return 0;
        if (message->path[length] && message->path[length] != '/') return 0;
    }
    if (rule->has_interface &&
        (!message->interface || strcmp(rule->interface, message->interface)))
        return 0;
    if (rule->has_member &&
        (!message->member || strcmp(rule->member, message->member))) return 0;
    if (rule->has_arg0 || rule->has_arg0_namespace) {
        struct tomoe_bus_message copy = *message;
        const char *argument = NULL;
        int result = tomoe_message_read(&copy, "s", &argument);
        if (result <= 0 || !argument) return 0;
        if (rule->has_arg0 && strcmp(rule->arg0, argument)) return 0;
        if (rule->has_arg0_namespace) {
            size_t length = strlen(rule->arg0_namespace);
            if (strncmp(rule->arg0_namespace, argument, length)) return 0;
            if (argument[length] && argument[length] != '.') return 0;
        }
    }
    return 1;
}

static int dispatch_message(struct tomoe_bus *bus,
                            struct tomoe_bus_message *message) {
    if (message->type == BUS_TYPE_SIGNAL) {
        for (struct tomoe_bus_slot *slot = bus->slots; slot;) {
            struct tomoe_bus_slot *next = slot->next;
            if (slot->kind == BUS_SLOT_MATCH &&
                rule_matches(&slot->rule, message)) {
                slot->running = 1;
                if (slot->handle) slot->handle(message, slot->userdata);
                slot->running = 0;
                if (!slot->refs) slot_free(slot);
            }
            slot = next;
        }
        return 0;
    }
    if (message->type == BUS_TYPE_RETURN || message->type == BUS_TYPE_ERROR) {
        struct tomoe_bus_slot *slot = NULL;
        for (struct tomoe_bus_slot *cursor = bus->slots; cursor;
             cursor = cursor->next) {
            if (cursor->kind == BUS_SLOT_CALL && !cursor->done &&
                cursor->serial == message->reply_serial) {
                slot = cursor;
                break;
            }
        }
        if (!slot) return 0;
        slot_unlink(slot);
        if (slot->handle) {
            slot->done = 1;
            slot->running = 1;
            slot->handle(message, slot->userdata);
            slot->running = 0;
            if (!slot->refs) slot_free(slot);
            else if (slot->anonymous) {
                slot->refs = 0;
                slot_free(slot);
            }
            return 0;
        }
        if (!slot->collect) {
            slot->refs = 0;
            slot_free(slot);
            return 0;
        }
        slot->result = message->type == BUS_TYPE_ERROR ? -EIO : 0;
        slot->collected = tomoe_message_ref(message);
        slot->done = 1;
        return 0;
    }
    if (message->type != BUS_TYPE_CALL) return 0;
    if (!message->path || !message->member) {
        tomoe_reply_method_error(
            message, "org.freedesktop.DBus.Error.InvalidArgs",
            "incomplete call");
        return 0;
    }
    for (struct tomoe_bus_slot *slot = bus->slots; slot; slot = slot->next) {
        if (slot->kind != BUS_SLOT_OBJECT) continue;
        if (strcmp(slot->path, message->path)) continue;
        object_dispatch(slot, message);
        return 0;
    }
    tomoe_reply_method_error(message,
        "org.freedesktop.DBus.Error.UnknownObject", "no object %s",
        message->path);
    return 0;
}

static int rule_field(struct bus_rule *rule, const char *key, size_t length,
                      const char *value, size_t value_length) {
    char text[BUS_MAX_RULE];
    if (value_length >= sizeof(text) || length >= sizeof(text)) return -EINVAL;
    memcpy(text, value, value_length);
    text[value_length] = '\0';
    char *target = NULL;
    size_t capacity = 0;
    int *present = NULL;
    if (length == 4 && !strncmp(key, "type", 4)) {
        if (strcmp(text, "signal")) return -EINVAL;
        rule->has_type = 1;
        return 0;
    }
    if (length == 6 && !strncmp(key, "sender", 6)) {
        target = rule->sender;
        capacity = sizeof(rule->sender);
        present = &rule->has_sender;
    } else if (length == 4 && !strncmp(key, "path", 4)) {
        target = rule->path;
        capacity = sizeof(rule->path);
        present = &rule->has_path;
    } else if (length == 14 && !strncmp(key, "path_namespace", 14)) {
        target = rule->path_namespace;
        capacity = sizeof(rule->path_namespace);
        present = &rule->has_path_namespace;
    } else if (length == 9 && !strncmp(key, "interface", 9)) {
        target = rule->interface;
        capacity = sizeof(rule->interface);
        present = &rule->has_interface;
    } else if (length == 6 && !strncmp(key, "member", 6)) {
        target = rule->member;
        capacity = sizeof(rule->member);
        present = &rule->has_member;
    } else if (length == 4 && !strncmp(key, "arg0", 4)) {
        target = rule->arg0;
        capacity = sizeof(rule->arg0);
        present = &rule->has_arg0;
    } else if (length == 13 && !strncmp(key, "arg0namespace", 13)) {
        target = rule->arg0_namespace;
        capacity = sizeof(rule->arg0_namespace);
        present = &rule->has_arg0_namespace;
    } else {
        return -EINVAL;
    }
    if (value_length + 1U > capacity) return -EINVAL;
    memcpy(target, text, value_length + 1U);
    *present = 1;
    return 0;
}

static int rule_parse(struct bus_rule *rule, const char *text) {
    memset(rule, 0, sizeof(*rule));
    const char *cursor = text;
    while (*cursor) {
        const char *key = cursor;
        const char *equal = strchr(key, '=');
        if (!equal) return -EINVAL;
        size_t key_length = (size_t)(equal - key);
        const char *value = equal + 1;
        if (*value != '\'') return -EINVAL;
        value++;
        const char *end = strchr(value, '\'');
        if (!end) return -EINVAL;
        int result = rule_field(rule, key, key_length, value,
                                (size_t)(end - value));
        if (result < 0) return result;
        cursor = end + 1;
        if (*cursor == ',') cursor++;
        else if (*cursor) return -EINVAL;
    }
    return 0;
}

static int bus_slot_link(struct tomoe_bus *bus, struct tomoe_bus_slot *slot) {
    slot->bus = bus;
    slot->next = bus->slots;
    bus->slots = slot;
    return 0;
}

static struct tomoe_bus_slot *slot_new(int kind) {
    struct tomoe_bus_slot *slot = calloc(1, sizeof(*slot));
    if (!slot) return NULL;
    slot->refs = 1;
    slot->kind = kind;
    return slot;
}

static int message_call_new(struct tomoe_bus *bus, const char *destination,
                            const char *path, const char *interface,
                            const char *member,
                            struct tomoe_bus_message **out) {
    struct tomoe_bus_message *message = message_new(bus, BUS_TYPE_CALL);
    if (!message) return -ENOMEM;
    message->path = text_copy(path);
    message->interface = text_copy(interface);
    message->member = text_copy(member);
    message->destination = text_copy(destination);
    if ((path && !message->path) || (interface && !message->interface) ||
        (member && !message->member) ||
        (destination && !message->destination)) {
        tomoe_message_unref(message);
        return -ENOMEM;
    }
    *out = message;
    return 0;
}

static int bus_exchange(struct tomoe_bus *bus,
                        struct tomoe_bus_message *message,
                        struct tomoe_bus_message **reply) {
    struct tomoe_bus_slot *slot = slot_new(BUS_SLOT_CALL);
    if (!slot) return -ENOMEM;
    int result = message_seal(bus, message);
    if (result < 0) {
        free(slot);
        return result;
    }
    slot->serial = message->serial;
    slot->anonymous = 1;
    slot->collect = 1;
    bus_slot_link(bus, slot);
    uint64_t deadline = deadline_after(
        monotonic_usec(), bus->timeout ? bus->timeout : 25000000ULL);
    for (;;) {
        if (slot->done) break;
        result = tomoe_bus_process(bus);
        if (result < 0 && result != -EAGAIN && result != -EINTR) {
            slot_unlink(slot);
            slot->refs = 0;
            slot_free(slot);
            return result;
        }
        if (slot->done) break;
        uint64_t now = monotonic_usec();
        if (now >= deadline) {
            slot_unlink(slot);
            slot->refs = 0;
            slot_free(slot);
            return -ETIMEDOUT;
        }
        result = tomoe_bus_wait(bus, deadline - now);
        if (result < 0 && result != -EINTR) {
            slot_unlink(slot);
            slot->refs = 0;
            slot_free(slot);
            return result;
        }
    }
    int outcome = slot->result;
    struct tomoe_bus_message *collected = slot->collected;
    slot->collected = NULL;
    slot_unlink(slot);
    slot->refs = 0;
    slot_free(slot);
    if (outcome < 0) {
        tomoe_message_unref(collected);
        return outcome;
    }
    if (reply) *reply = collected;
    else tomoe_message_unref(collected);
    return 0;
}

static int bus_call_collect(struct tomoe_bus *bus, const char *destination,
                            const char *path, const char *interface,
                            const char *member, const char *signature,
                            va_list *ap, struct tomoe_bus_message **reply) {
    if (!bus || bus->closed) return -ENOTCONN;
    struct tomoe_bus_message *message = NULL;
    int result = message_call_new(bus, destination, path, interface, member,
                                  &message);
    if (result < 0) return result;
    result = message_append_va(message, signature ? signature : "", ap);
    if (result >= 0) result = bus_exchange(bus, message, reply);
    tomoe_message_unref(message);
    return result;
}

int tomoe_bus_call(struct tomoe_bus *bus, const char *destination,
                   const char *path, const char *interface,
                   const char *member, const char *signature, ...) {
    va_list ap;
    va_start(ap, signature);
    int result = bus_call_collect(bus, destination, path, interface, member,
                                  signature, &ap, NULL);
    va_end(ap);
    return result;
}

static int bus_call_async_va(struct tomoe_bus *bus,
                             struct tomoe_bus_slot **slot_out,
                             const char *destination, const char *path,
                             const char *interface, const char *member,
                             int (*reply)(struct tomoe_bus_message *, void *),
                             void *userdata, const char *signature,
                             va_list *ap) {
    if (!bus || bus->closed) return -ENOTCONN;
    struct tomoe_bus_message *message = NULL;
    int result = message_call_new(bus, destination, path, interface, member,
                                  &message);
    if (result < 0) return result;
    result = message_append_va(message, signature ? signature : "", ap);
    if (result < 0) {
        tomoe_message_unref(message);
        return result;
    }
    struct tomoe_bus_slot *slot = slot_new(BUS_SLOT_CALL);
    if (!slot) {
        tomoe_message_unref(message);
        return -ENOMEM;
    }
    result = message_seal(bus, message);
    if (result < 0) {
        tomoe_message_unref(message);
        free(slot);
        return result;
    }
    slot->serial = message->serial;
    slot->handle = reply;
    slot->userdata = userdata;
    slot->anonymous = slot_out == NULL;
    bus_slot_link(bus, slot);
    tomoe_message_unref(message);
    if (slot_out) {
        *slot_out = slot;
        return 1;
    }
    return 0;
}

int tomoe_bus_call_async(struct tomoe_bus *bus, struct tomoe_bus_slot **slot,
                         const char *destination, const char *path,
                         const char *interface, const char *member,
                         int (*reply)(struct tomoe_bus_message *, void *),
                         void *userdata, const char *signature, ...) {
    va_list ap;
    va_start(ap, signature);
    int result = bus_call_async_va(bus, slot, destination, path, interface,
                                   member, reply, userdata, signature, &ap);
    va_end(ap);
    return result;
}

int tomoe_bus_call_asyncv(struct tomoe_bus *bus, struct tomoe_bus_slot **slot,
                          const char *destination, const char *path,
                          const char *interface, const char *member,
                          int (*reply)(struct tomoe_bus_message *, void *),
                          void *userdata, const char *signature,
                          va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int result = bus_call_async_va(bus, slot, destination, path, interface,
                                   member, reply, userdata, signature, &copy);
    va_end(copy);
    return result;
}

int tomoe_bus_add_match(struct tomoe_bus *bus, struct tomoe_bus_slot **slot_out,
                        const char *rule,
                        int (*handle)(struct tomoe_bus_message *, void *),
                        void *userdata) {
    if (!bus || !rule) return -EINVAL;
    struct tomoe_bus_slot *slot = slot_new(BUS_SLOT_MATCH);
    if (!slot) return -ENOMEM;
    int result = rule_parse(&slot->rule, rule);
    if (result < 0) {
        free(slot);
        return result;
    }
    slot->handle = handle;
    slot->userdata = userdata;
    bus_slot_link(bus, slot);
    struct tomoe_bus_message *message = NULL;
    result = message_call_new(bus, "org.freedesktop.DBus",
                              "/org/freedesktop/DBus",
                              "org.freedesktop.DBus", "AddMatch", &message);
    if (result >= 0) {
        result = tomoe_message_append(message, "s", rule);
        if (result >= 0) result = bus_exchange(bus, message, NULL);
    }
    tomoe_message_unref(message);
    if (result < 0) {
        slot_unlink(slot);
        slot->refs = 0;
        slot_free(slot);
        return result;
    }
    if (slot_out) *slot_out = slot;
    return 0;
}

int tomoe_bus_add_object(struct tomoe_bus *bus, struct tomoe_bus_slot **slot_out,
                         const char *path, const char *interface,
                         const struct tomoe_bus_method *methods,
                         size_t method_count,
                         const struct tomoe_bus_property *properties,
                         size_t property_count,
                         const struct tomoe_bus_signal *signals,
                         size_t signal_count, void *userdata) {
    if (!bus || !path || !interface) return -EINVAL;
    struct tomoe_bus_slot *slot = slot_new(BUS_SLOT_OBJECT);
    if (!slot) return -ENOMEM;
    slot->path = text_copy(path);
    slot->interface = text_copy(interface);
    if (!slot->path || !slot->interface) {
        free(slot->path);
        free(slot->interface);
        free(slot);
        return -ENOMEM;
    }
    slot->methods = methods;
    slot->method_count = method_count;
    slot->properties = properties;
    slot->property_count = property_count;
    slot->signals = signals;
    slot->signal_count = signal_count;
    slot->userdata = userdata;
    bus_slot_link(bus, slot);
    if (slot_out) *slot_out = slot;
    return 0;
}

int tomoe_bus_request_name(struct tomoe_bus *bus, const char *name) {
    if (!bus || !name) return -EINVAL;
    if (!tomoe_bus_name_is_valid(name)) return -EINVAL;
    struct tomoe_bus_message *message = NULL;
    int result = message_call_new(bus, "org.freedesktop.DBus",
                                  "/org/freedesktop/DBus",
                                  "org.freedesktop.DBus", "RequestName",
                                  &message);
    if (result < 0) return result;
    result = tomoe_message_append(message, "su", name, (uint32_t)0);
    struct tomoe_bus_message *reply = NULL;
    if (result >= 0) result = bus_exchange(bus, message, &reply);
    tomoe_message_unref(message);
    if (result < 0) return result;
    uint32_t outcome = 0;
    result = tomoe_message_read(reply, "u", &outcome);
    tomoe_message_unref(reply);
    if (result <= 0) return -EPROTO;
    return outcome == 1U || outcome == 4U ? 1 : 0;
}

int tomoe_bus_emit_signal(struct tomoe_bus *bus, const char *path,
                          const char *interface, const char *member,
                          const char *signature, ...) {
    if (!bus || bus->closed) return -ENOTCONN;
    struct tomoe_bus_message *message = message_new(bus, BUS_TYPE_SIGNAL);
    if (!message) return -ENOMEM;
    message->path = text_copy(path);
    message->interface = text_copy(interface);
    message->member = text_copy(member);
    if (!message->path || !message->interface || !message->member) {
        tomoe_message_unref(message);
        return -ENOMEM;
    }
    va_list ap;
    va_start(ap, signature);
    int result = message_append_va(message, signature ? signature : "", &ap);
    va_end(ap);
    if (result >= 0) result = message_seal(bus, message);
    tomoe_message_unref(message);
    return result;
}

static int emit_property_entry(struct tomoe_bus_slot *object,
                               struct tomoe_bus_message *message,
                               const struct tomoe_bus_property *property) {
    int result = tomoe_message_open_container(message, TOMOE_BUS_DICT_ENTRY,
                                              "sv");
    if (result < 0) return result;
    result = tomoe_message_append(message, "s", property->name);
    if (result < 0) return result;
    result = tomoe_message_open_container(message, 'v', property->signature);
    if (result < 0) return result;
    result = object_property_append(object, message, property);
    if (result < 0) return result;
    result = tomoe_message_close_container(message);
    if (result < 0) return result;
    return tomoe_message_close_container(message);
}

int tomoe_bus_emit_properties_changed(struct tomoe_bus *bus, const char *path,
                                      const char *interface, ...) {
    if (!bus || bus->closed) return -ENOTCONN;
    struct tomoe_bus_slot *object = NULL;
    for (struct tomoe_bus_slot *slot = bus->slots; slot; slot = slot->next)
        if (slot->kind == BUS_SLOT_OBJECT && !strcmp(slot->path, path) &&
            !strcmp(slot->interface, interface))
            object = slot;
    if (!object) return -EINVAL;
    struct tomoe_bus_message *message = message_new(bus, BUS_TYPE_SIGNAL);
    if (!message) return -ENOMEM;
    message->path = text_copy(path);
    message->interface = text_copy("org.freedesktop.DBus.Properties");
    message->member = text_copy("PropertiesChanged");
    if (!message->path || !message->interface || !message->member) {
        tomoe_message_unref(message);
        return -ENOMEM;
    }
    int result = tomoe_message_append(message, "s", interface);
    if (result < 0) goto done;
    result = tomoe_message_open_container(message, 'a', "{sv}");
    if (result < 0) goto done;
    va_list ap;
    va_start(ap, interface);
    const char *name = va_arg(ap, const char *);
    if (!name) {
        for (size_t index = 0; index < object->property_count; index++) {
            const struct tomoe_bus_property *property =
                &object->properties[index];
            if (!property->changing) continue;
            result = emit_property_entry(object, message, property);
            if (result < 0) break;
        }
    } else {
        while (name && result >= 0) {
            const struct tomoe_bus_property *property =
                object_property(object, name);
            if (property && property->changing)
                result = emit_property_entry(object, message, property);
            name = va_arg(ap, const char *);
        }
    }
    va_end(ap);
    if (result < 0) goto done;
    result = tomoe_message_close_container(message);
    if (result < 0) goto done;
    result = tomoe_message_open_container(message, 'a', "s");
    if (result < 0) goto done;
    result = tomoe_message_close_container(message);
    if (result < 0) goto done;
    result = message_seal(bus, message);
done:
    tomoe_message_unref(message);
    return result;
}

int tomoe_bus_send(struct tomoe_bus *bus, struct tomoe_bus_message *message) {
    if (!message) return -EINVAL;
    struct tomoe_bus *target = bus ? bus : message->bus;
    if (!target || target->closed) return -ENOTCONN;
    return message_seal(target, message);
}

int tomoe_message_new_method_return(struct tomoe_bus_message *call,
                                    struct tomoe_bus_message **reply) {
    if (!call || !reply) return -EINVAL;
    if (call->type != BUS_TYPE_CALL) return -EINVAL;
    struct tomoe_bus_message *message = message_new(call->bus, BUS_TYPE_RETURN);
    if (!message) return -ENOMEM;
    message->discard = call->flags & 1;
    message->destination = text_copy(call->sender);
    message->reply_serial = call->serial;
    if (call->sender && !message->destination) {
        tomoe_message_unref(message);
        return -ENOMEM;
    }
    *reply = message;
    return 0;
}

static int reply_error_va(struct tomoe_bus_message *call, const char *name,
                          const char *format, va_list ap) {
    if (!call) return -EINVAL;
    struct tomoe_bus_message *message = message_new(call->bus, BUS_TYPE_ERROR);
    if (!message) return -ENOMEM;
    message->discard = call->flags & 1;
    message->destination = text_copy(call->sender);
    message->reply_serial = call->serial;
    message->error_name = text_copy(name);
    if (!message->error_name || (call->sender && !message->destination)) {
        tomoe_message_unref(message);
        return -ENOMEM;
    }
    char text[512];
    int length = 0;
    if (format) {
        length = vsnprintf(text, sizeof(text), format, ap);
        if (length < 0) length = 0;
        if ((size_t)length >= sizeof(text)) {
            text[sizeof(text) - 1U] = '\0';
            length = (int)sizeof(text) - 1;
        }
    } else {
        text[0] = '\0';
    }
    int result = tomoe_message_append(message, "s", text);
    if (result >= 0) result = message_seal(call->bus, message);
    tomoe_message_unref(message);
    return result < 0 ? result : 1;
}

int tomoe_reply_method_errorv(struct tomoe_bus_message *call, const char *name,
                              const char *format, va_list ap) {
    return reply_error_va(call, name, format, ap);
}

int tomoe_reply_method_error(struct tomoe_bus_message *call, const char *name,
                             const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int result = reply_error_va(call, name, format, ap);
    va_end(ap);
    return result;
}

int tomoe_reply_method_return(struct tomoe_bus_message *call,
                              const char *signature, ...) {
    struct tomoe_bus_message *reply = NULL;
    int result = tomoe_message_new_method_return(call, &reply);
    if (result < 0) return result;
    va_list ap;
    va_start(ap, signature);
    result = message_append_va(reply, signature ? signature : "", &ap);
    va_end(ap);
    if (result >= 0) result = message_seal(reply->bus, reply);
    tomoe_message_unref(reply);
    return result < 0 ? result : 1;
}

int tomoe_message_read(struct tomoe_bus_message *message,
                       const char *signature, ...) {
    va_list ap;
    va_start(ap, signature);
    int result = message_read_va(message, signature, &ap);
    va_end(ap);
    return result;
}

int tomoe_message_read_array(struct tomoe_bus_message *message, char type,
                             const void **data, size_t *length) {
    if (!message || !data || !length) return -EINVAL;
    size_t limit = message_limit(message);
    size_t position = align_up(message->cursor, 4U);
    if (position >= limit) return 0;
    uint32_t bytes;
    int result = data_u32(message->data, position, limit, &bytes);
    if (result < 0) return result;
    size_t alignment = signature_alignment(type);
    if (!alignment) return -EINVAL;
    size_t start = align_up(position + 4U, alignment);
    if (start > limit || bytes > limit - start) return -EBADMSG;
    *data = message->data + start;
    *length = bytes;
    message->cursor = start + bytes;
    result = parent_advance(message);
    return result < 0 ? result : 1;
}

int tomoe_message_enter_container(struct tomoe_bus_message *message, char type,
                                  const char *contents) {
    if (!message) return -EINVAL;
    return message_enter(message, type, contents);
}

int tomoe_message_exit_container(struct tomoe_bus_message *message) {
    if (!message || message->depth <= 1) return -EINVAL;
    struct bus_frame *frame = &message->frames[message->depth - 1];
    if (message->cursor > frame->end) return -EBADMSG;
    message->cursor = frame->end;
    message->depth--;
    return 1;
}

int tomoe_message_peek_type(struct tomoe_bus_message *message, char *type,
                            const char **contents) {
    if (!message || !type) return -EINVAL;
    struct bus_frame *frame = &message->frames[message->depth - 1];
    if (message->cursor >= frame->end) return 0;
    if (!frame->signature) return -EBADMSG;
    const char *cursor = frame->signature + frame->position;
    if (!*cursor) return -EBADMSG;
    char value = *cursor;
    const char *inner = NULL;
    if (value == '{' || value == '(') {
        const char *start = cursor + 1;
        const char *end = signature_after(cursor);
        if (!end) return -EBADMSG;
        size_t length = (size_t)(end - start) - 1U;
        if (length >= sizeof(message->contents)) return -EBADMSG;
        memcpy(message->contents, start, length);
        message->contents[length] = '\0';
        inner = message->contents;
        value = value == '{' ? TOMOE_BUS_DICT_ENTRY : TOMOE_BUS_STRUCT;
    } else if (value == 'a') {
        const char *after = signature_after(cursor + 1);
        if (!after) return -EBADMSG;
        size_t length = (size_t)(after - (cursor + 1));
        if (length >= sizeof(message->contents)) return -EBADMSG;
        memcpy(message->contents, cursor + 1, length);
        message->contents[length] = '\0';
        inner = message->contents;
    } else if (value == 'v') {
        const char *text;
        size_t next;
        int result = data_signature(message->data, message->cursor, frame->end,
                                    &text, &next);
        if (result < 0) return result;
        size_t length = strlen(text);
        if (length >= sizeof(message->contents)) return -EBADMSG;
        memcpy(message->contents, text, length + 1U);
        inner = message->contents;
    }
    *type = value;
    if (contents) *contents = inner;
    return 1;
}

int tomoe_message_skip(struct tomoe_bus_message *message,
                       const char *contents) {
    if (!message) return -EINVAL;
    struct bus_frame *frame = &message->frames[message->depth - 1];
    if (!contents) {
        message->cursor = frame->end;
        return 1;
    }
    size_t next;
    int result = position_after(message->data, message->cursor, contents,
                                frame->end, &next);
    if (result < 0) return result;
    message->cursor = next;
    const char *cursor = contents;
    while (*cursor) {
        cursor = signature_after(cursor);
        if (!cursor) return -EBADMSG;
        result = parent_advance(message);
        if (result < 0) return result;
    }
    return 1;
}

int tomoe_message_at_end(struct tomoe_bus_message *message, int complete) {
    if (!message) return -EINVAL;
    if (complete)
        return message->depth == 1 && message->cursor >= message->body_end;
    return message->cursor >= message_limit(message);
}

int tomoe_message_append(struct tomoe_bus_message *message,
                         const char *signature, ...) {
    va_list ap;
    va_start(ap, signature);
    int result = message_append_va(message, signature, &ap);
    va_end(ap);
    return result;
}

int tomoe_bus_name_is_valid(const char *name) {
    if (!name || !*name || strlen(name) > 255U) return 0;
    int unique = name[0] == ':';
    const char *cursor = name + (unique ? 1 : 0);
    if (!*cursor) return 0;
    if (!unique && !strchr(name, '.')) return 0;
    int empty = 1;
    int first = 1;
    for (; *cursor; cursor++) {
        unsigned char value = (unsigned char)*cursor;
        if (value == '.') {
            if (empty) return 0;
            empty = 1;
            first = 0;
            continue;
        }
        int valid = (value >= 'A' && value <= 'Z') ||
                    (value >= 'a' && value <= 'z') ||
                    (value >= '0' && value <= '9') || value == '_' ||
                    value == '-';
        if (!valid) return 0;
        if (!unique && first && value >= '0' && value <= '9') return 0;
        empty = 0;
    }
    return !empty;
}

int tomoe_bus_path_is_valid(const char *path) {
    if (!path || path[0] != '/') return 0;
    if (!path[1]) return 1;
    if (path[strlen(path) - 1U] == '/') return 0;
    int empty = 1;
    for (const char *cursor = path + 1; *cursor; cursor++) {
        unsigned char value = (unsigned char)*cursor;
        if (value == '/') {
            if (empty) return 0;
            empty = 1;
            continue;
        }
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '_')) return 0;
        empty = 0;
    }
    return !empty;
}

static int bus_hello(struct tomoe_bus *bus, uint64_t timeout_usec) {
    uint64_t deadline = deadline_after(monotonic_usec(), timeout_usec);
    int result = bus_authenticate(bus->fd);
    if (result < 0) return result;
    int flags = fcntl(bus->fd, F_GETFL, 0);
    if (flags < 0) return -errno;
    if (fcntl(bus->fd, F_SETFL, flags | O_NONBLOCK) < 0) return -errno;
    struct tomoe_bus_message *message = NULL;
    result = message_call_new(bus, "org.freedesktop.DBus",
                              "/org/freedesktop/DBus", "org.freedesktop.DBus",
                              "Hello", &message);
    if (result < 0) return result;
    struct tomoe_bus_slot *slot = slot_new(BUS_SLOT_CALL);
    if (!slot) {
        tomoe_message_unref(message);
        return -ENOMEM;
    }
    result = message_seal(bus, message);
    if (result < 0) {
        tomoe_message_unref(message);
        free(slot);
        return result;
    }
    slot->serial = message->serial;
    slot->anonymous = 1;
    slot->collect = 1;
    bus_slot_link(bus, slot);
    tomoe_message_unref(message);
    for (;;) {
        if (slot->done) break;
        result = tomoe_bus_process(bus);
        if (result < 0 && result != -EAGAIN && result != -EINTR) {
            slot_unlink(slot);
            slot->refs = 0;
            slot_free(slot);
            return result;
        }
        if (slot->done) break;
        uint64_t now = monotonic_usec();
        if (now >= deadline) {
            slot_unlink(slot);
            slot->refs = 0;
            slot_free(slot);
            return -ETIMEDOUT;
        }
        result = tomoe_bus_wait(bus, deadline - now);
        if (result < 0 && result != -EINTR) {
            slot_unlink(slot);
            slot->refs = 0;
            slot_free(slot);
            return result;
        }
    }
    int outcome = slot->result;
    struct tomoe_bus_message *reply = slot->collected;
    slot->collected = NULL;
    slot_unlink(slot);
    slot->refs = 0;
    slot_free(slot);
    if (outcome < 0) {
        tomoe_message_unref(reply);
        return outcome;
    }
    const char *unique = NULL;
    result = tomoe_message_read(reply, "s", &unique);
    if (result > 0 && unique) {
        size_t length = strlen(unique);
        if (length >= sizeof(bus->unique)) length = sizeof(bus->unique) - 1U;
        memcpy(bus->unique, unique, length);
        bus->unique[length] = '\0';
    }
    tomoe_message_unref(reply);
    if (result <= 0) return -EPROTO;
    bus->timeout = timeout_usec;
    return 0;
}

static struct tomoe_bus *bus_open(const char *address, uint64_t timeout_usec,
                                  int *error) {
    int saved = 0;
    int fd = bus_connect_socket(address, &saved);
    if (fd < 0) {
        if (error) *error = saved;
        return NULL;
    }
    struct tomoe_bus *bus = calloc(1, sizeof(*bus));
    if (!bus) {
        close(fd);
        if (error) *error = ENOMEM;
        return NULL;
    }
    bus->refs = 1;
    bus->fd = fd;
    bus->timeout = timeout_usec;
    int result = bus_hello(bus, timeout_usec);
    if (result < 0) {
        close(fd);
        free(bus->input.data);
        free(bus->output.data);
        free(bus);
        if (error) *error = -result;
        return NULL;
    }
    if (error) *error = 0;
    return bus;
}

int tomoe_bus_open_address(struct tomoe_bus **out, const char *address,
                           uint64_t timeout_usec) {
    if (!out || !address) return -EINVAL;
    int error = 0;
    struct tomoe_bus *bus = bus_open(address, timeout_usec, &error);
    if (!bus) return -error;
    *out = bus;
    return 0;
}

int tomoe_bus_open_session(struct tomoe_bus **out, uint64_t timeout_usec) {
    if (!out) return -EINVAL;
    const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
    char buffer[BUS_MAX_PATH + 32U];
    if (!address || !*address) {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        if (!runtime || !*runtime) return -ENOMEDIUM;
        int length = snprintf(buffer, sizeof(buffer), "unix:path=%s/bus",
                              runtime);
        if (length <= 0 || (size_t)length >= sizeof(buffer))
            return -ENAMETOOLONG;
        address = buffer;
    }
    return tomoe_bus_open_address(out, address, timeout_usec);
}

int tomoe_bus_open_system(struct tomoe_bus **out, uint64_t timeout_usec) {
    if (!out) return -EINVAL;
    const char *address = getenv("DBUS_SYSTEM_BUS_ADDRESS");
    if (!address || !*address)
        address = "unix:path=/run/dbus/system_bus_socket";
    return tomoe_bus_open_address(out, address, timeout_usec);
}

void tomoe_bus_close(struct tomoe_bus *bus) {
    if (!bus || bus->closed) return;
    bus->closed = 1;
    if (bus->fd >= 0) {
        close(bus->fd);
        bus->fd = -1;
    }
}

struct tomoe_bus *tomoe_bus_unref(struct tomoe_bus *bus) {
    if (!bus) return NULL;
    if (--bus->refs) return bus;
    tomoe_bus_close(bus);
    struct tomoe_bus_slot *slot = bus->slots;
    while (slot) {
        struct tomoe_bus_slot *next = slot->next;
        slot->bus = NULL;
        slot->refs = 0;
        slot_free(slot);
        slot = next;
    }
    free(bus->input.data);
    free(bus->output.data);
    free(bus);
    return NULL;
}

void tomoe_bus_flush_close_unref(struct tomoe_bus *bus) {
    if (!bus) return;
    tomoe_bus_flush(bus);
    tomoe_bus_close(bus);
    tomoe_bus_unref(bus);
}
