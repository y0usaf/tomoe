#define _GNU_SOURCE

#include "network.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#define NETWORK_NM "org.freedesktop.NetworkManager"
#define NETWORK_ROOT_PATH "/org/freedesktop/NetworkManager"
#define NETWORK_ROOT_IFACE "org.freedesktop.NetworkManager"
#define NETWORK_CONN_IFACE "org.freedesktop.NetworkManager.Connection.Active"
#define NETWORK_AP_IFACE "org.freedesktop.NetworkManager.AccessPoint"
#define NETWORK_PROPERTIES "org.freedesktop.DBus.Properties"
#define NETWORK_DBUS "org.freedesktop.DBus"
#define NETWORK_DBUS_PATH "/org/freedesktop/DBus"
#define NETWORK_NO_PATH "/"
#define NETWORK_WIFI_TYPE "802-11-wireless"
#define NETWORK_STATE_CONNECTED 50U
#define NETWORK_DEFAULT_ROOT "/sys/class/net"
#define NETWORK_SYSTEM_ADDRESS "unix:path=/run/dbus/system_bus_socket"

#define NETWORK_SETUP_USEC 2000000ULL
#define NETWORK_REQUEST_USEC 2000000ULL
#define NETWORK_SYSFS_INTERVAL_USEC 30000000ULL
#define NETWORK_POLL_MESSAGES 64U
#define NETWORK_POLL_BUDGET_USEC 4000ULL
#define NETWORK_MAX_TEXT 4096U
#define NETWORK_MAX_PATH 4096U
#define NETWORK_MAX_SSID 4096U
#define NETWORK_MAX_DICT 32U
#define NETWORK_MAX_DIRS 256U
#define NETWORK_MAX_PENDING 64U

enum network_call_kind {
    NETWORK_CALL_OWNER,
    NETWORK_CALL_GET_ALL,
};

enum network_scope {
    NETWORK_SCOPE_ROOT,
    NETWORK_SCOPE_CONN,
    NETWORK_SCOPE_AP,
};

struct network_props {
    int state;
    uint32_t state_value;
    int primary;
    char primary_value[NETWORK_MAX_PATH + 1U];
    int type;
    char type_value[NETWORK_MAX_TEXT + 1U];
    int specific;
    char specific_value[NETWORK_MAX_PATH + 1U];
    int ssid;
    unsigned char ssid_value[NETWORK_MAX_SSID];
    size_t ssid_length;
    int strength;
    uint8_t strength_value;
};

struct network_invalidated {
    int state;
    int primary;
    int type;
    int specific;
    int ssid;
    int strength;
};

struct network_call {
    enum network_call_kind kind;
    enum network_scope scope;
    struct tomoe_network *network;
    struct network_call *next;
    sd_bus_slot *slot;
    uint64_t owner_epoch;
    uint64_t primary_chain_epoch;
    uint64_t primary_signal_epoch;
    uint64_t specific_chain_epoch;
    uint64_t specific_signal_epoch;
    uint64_t state_epoch;
    uint64_t type_epoch;
    uint64_t ssid_epoch;
    uint64_t strength_epoch;
    uint64_t request_serial;
    uint64_t deadline_usec;
    char path[NETWORK_MAX_PATH + 1U];
    char owner[256];
};

struct network_raw {
    int state_known;
    uint32_t state;
    int primary_known;
    char primary[NETWORK_MAX_PATH + 1U];
    int type_known;
    char type[NETWORK_MAX_TEXT + 1U];
    int specific_known;
    char specific[NETWORK_MAX_PATH + 1U];
    int ssid_known;
    unsigned char ssid[NETWORK_MAX_SSID];
    size_t ssid_length;
    int strength_known;
    uint8_t strength;
};

struct text_buffer {
    char *data;
    size_t length;
    size_t capacity;
};

struct tomoe_network {
    sd_bus *bus;
    sd_bus_slot *matches[3];
    struct network_call *calls;
    size_t pending_count;

    char sysfs_root[NETWORK_MAX_PATH + 1U];
    int sysfs_enabled;
    int sysfs_connected;
    uint64_t sysfs_next_usec;

    int bus_disabled;
    int nm_authoritative;
    char owner[256];
    uint64_t owner_epoch;

    uint64_t state_epoch;
    uint64_t primary_chain_epoch;
    uint64_t primary_signal_epoch;
    uint64_t type_epoch;
    uint64_t specific_chain_epoch;
    uint64_t specific_signal_epoch;
    uint64_t ssid_epoch;
    uint64_t strength_epoch;
    uint64_t next_request_serial;
    uint64_t applied_state_serial;
    uint64_t applied_primary_serial;
    uint64_t applied_type_serial;
    uint64_t applied_specific_serial;
    uint64_t applied_ssid_serial;
    uint64_t applied_strength_serial;

    int need_owner;
    int need_root;
    int need_conn;
    int need_ap;
    struct network_raw raw;

    int connected;
    int ssid_present;
    uint8_t strength;
    uint32_t public_ssid[NETWORK_MAX_SSID];
    size_t public_ssid_length;
    uint64_t revision;
    int changed;
    char *snapshot;
};

static void cancel_stale_calls(struct tomoe_network *network);

int tomoe_network_abi(void) { return 1; }

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

static int bounded_string(const char *value, size_t limit) {
    return value && strnlen(value, limit + 1U) <= limit;
}

static int valid_unique_name(const char *value) {
    return bounded_string(value, 255U) && value[0] == ':';
}

static int valid_object_path(const char *value) {
    return bounded_string(value, NETWORK_MAX_PATH) && value[0] == '/';
}

static int copy_bounded(char *destination, size_t capacity, const char *source,
                        size_t limit) {
    if (!source || !bounded_string(source, limit) ||
        strlen(source) + 1U > capacity)
        return -E2BIG;
    memcpy(destination, source, strlen(source) + 1U);
    return 0;
}

static void revision_bump(struct tomoe_network *network) {
    network->revision++;
    if (!network->revision) network->revision = 1;
    network->changed = 1;
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

static int decode_ssid(const unsigned char *data, size_t length,
                       uint32_t *output, size_t *output_length) {
    size_t index = 0;
    size_t count = 0;
    while (index < length) {
        uint32_t codepoint = 0xfffdU;
        size_t consumed = 1U;
        unsigned char first_byte = data[index];
        if (first_byte < 0x80U) {
            codepoint = first_byte;
        } else if (first_byte >= 0xc2U && first_byte <= 0xdfU) {
            if (index + 1U >= length) {
                consumed = length - index;
            } else if ((data[index + 1U] & 0xc0U) == 0x80U) {
                codepoint = ((uint32_t)first_byte & 0x1fU) << 6U;
                codepoint |= data[index + 1U] & 0x3fU;
                consumed = 2U;
            }
        } else if (first_byte >= 0xe0U && first_byte <= 0xefU) {
            if (index + 1U < length) {
                unsigned char second = data[index + 1U];
                int second_ok = (second & 0xc0U) == 0x80U &&
                    (first_byte != 0xe0U || second >= 0xa0U) &&
                    (first_byte != 0xedu || second <= 0x9fU);
                if (second_ok && index + 2U >= length) {
                    consumed = length - index;
                } else if (second_ok) {
                    unsigned char third = data[index + 2U];
                    if ((third & 0xc0U) == 0x80U) {
                        codepoint = ((uint32_t)first_byte & 0x0fU) << 12U;
                        codepoint |= ((uint32_t)second & 0x3fU) << 6U;
                        codepoint |= third & 0x3fU;
                        consumed = 3U;
                    } else {
                        consumed = 2U;
                    }
                }
            }
        } else if (first_byte >= 0xf0U && first_byte <= 0xf4U) {
            if (index + 1U < length) {
                unsigned char second = data[index + 1U];
                int second_ok = (second & 0xc0U) == 0x80U &&
                    (first_byte != 0xf0U || second >= 0x90U) &&
                    (first_byte != 0xf4U || second <= 0x8fU);
                if (second_ok && index + 2U >= length) {
                    consumed = length - index;
                } else if (second_ok) {
                    unsigned char third = data[index + 2U];
                    if ((third & 0xc0U) == 0x80U && index + 3U >= length) {
                        consumed = length - index;
                    } else if ((third & 0xc0U) == 0x80U) {
                        unsigned char fourth = data[index + 3U];
                        if ((fourth & 0xc0U) == 0x80U) {
                            codepoint = ((uint32_t)first_byte & 0x07U) << 18U;
                            codepoint |= ((uint32_t)second & 0x3fU) << 12U;
                            codepoint |= ((uint32_t)third & 0x3fU) << 6U;
                            codepoint |= fourth & 0x3fU;
                            consumed = 4U;
                        } else {
                            consumed = 3U;
                        }
                    } else {
                        consumed = 2U;
                    }
                }
            }
        }
        if (count >= NETWORK_MAX_SSID) return -E2BIG;
        output[count++] = codepoint;
        index += consumed;
    }
    *output_length = count;
    return 0;
}

static int append_ssid_list(struct text_buffer *buffer,
                            const uint32_t *codepoints, size_t length) {
    int result = text_buffer_literal(buffer, "(");
    if (result < 0) return result;
    for (size_t index = 0; index < length; index++) {
        if (index) {
            result = text_buffer_literal(buffer, " ");
            if (result < 0) return result;
        }
        result = text_buffer_uint(buffer, codepoints[index]);
        if (result < 0) return result;
    }
    return text_buffer_literal(buffer, ")");
}

static void public_default(struct tomoe_network *network) {
    network->connected = 0;
    network->ssid_present = 0;
    network->strength = 0;
}

static int raw_is_connected(const struct network_raw *raw) {
    return raw->state_known && raw->state >= NETWORK_STATE_CONNECTED;
}

static int raw_is_wifi(const struct network_raw *raw) {
    return raw_is_connected(raw) && raw->type_known &&
           !strncmp(raw->type, NETWORK_WIFI_TYPE,
                    sizeof(NETWORK_WIFI_TYPE) - 1U);
}

static void public_from_source(struct tomoe_network *network) {
    int connected = 0;
    int ssid_present = 0;
    uint8_t strength = 0;
    if (network->nm_authoritative) {
        connected = raw_is_connected(&network->raw);
        if (raw_is_wifi(&network->raw)) {
            strength = network->raw.strength_known ? network->raw.strength : 0;
            ssid_present = network->raw.ssid_known &&
                network->raw.ssid_length > 0U;
        }
    } else if (network->sysfs_enabled) {
        connected = network->sysfs_connected;
    }
    uint32_t decoded_ssid[NETWORK_MAX_SSID] = {0};
    const uint32_t *ssid = NULL;
    size_t ssid_length = 0;
    if (network->nm_authoritative && ssid_present) {
        if (decode_ssid(network->raw.ssid, network->raw.ssid_length,
                        decoded_ssid, &ssid_length) < 0) {
            ssid_present = 0;
            ssid_length = 0;
        } else {
            ssid = decoded_ssid;
        }
    }
    int ssid_changed = network->ssid_present != ssid_present ||
        network->public_ssid_length != ssid_length ||
        (ssid_length && memcmp(network->public_ssid, ssid,
                               ssid_length * sizeof(*ssid)));
    if (network->connected != connected || ssid_changed ||
        network->strength != strength) {
        network->connected = connected;
        network->ssid_present = ssid_present;
        network->strength = strength;
        network->public_ssid_length = ssid_length;
        if (ssid_length)
            memcpy(network->public_ssid, ssid,
                   ssid_length * sizeof(*ssid));
        revision_bump(network);
    }
}

static int snapshot_build(struct tomoe_network *network, char **output) {
    struct text_buffer buffer = {0};
    int result = text_buffer_literal(&buffer, "(:connected ");
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, network->connected ? "t" : "nil");
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, " :ssid ");
    if (result < 0) goto failed;
    if (!network->ssid_present) {
        result = text_buffer_literal(&buffer, "nil");
    } else {
        result = append_ssid_list(&buffer, network->public_ssid,
                                  network->public_ssid_length);
    }
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, " :strength ");
    if (result < 0) goto failed;
    result = text_buffer_uint(&buffer, network->strength);
    if (result < 0) goto failed;
    result = text_buffer_literal(&buffer, ")");
    if (result < 0) goto failed;
    *output = buffer.data;
    return 0;
failed:
    free(buffer.data);
    return result;
}

const char *tomoe_network_snapshot(struct tomoe_network *network) {
    if (!network) return NULL;
    char *snapshot = NULL;
    if (snapshot_build(network, &snapshot) < 0) return NULL;
    free(network->snapshot);
    network->snapshot = snapshot;
    return snapshot;
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

static int sysfs_scan(struct tomoe_network *network, int *connected) {
    int root = open(network->sysfs_root,
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK);
    if (root < 0) return -errno;
    int scan_fd = dup(root);
    DIR *directory = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!directory) {
        if (scan_fd >= 0) close(scan_fd);
        close(root);
        return -errno;
    }
    size_t visited = 0;
    struct dirent *entry;
    int found = 0;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (++visited > NETWORK_MAX_DIRS) break;
        if (!strcmp(entry->d_name, "lo") ||
            !bounded_string(entry->d_name, NETWORK_MAX_TEXT))
            continue;
        int device = openat(root, entry->d_name,
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK);
        if (device < 0) continue;
        struct stat status;
        char operstate[NETWORK_MAX_TEXT + 1U] = {0};
        int valid = fstat(device, &status) == 0 && S_ISDIR(status.st_mode) &&
            read_attribute(device, "operstate", operstate,
                           sizeof(operstate)) >= 0;
        if (valid) {
            struct stat current;
            if (fstatat(root, entry->d_name, &current, 0) < 0 ||
                current.st_dev != status.st_dev ||
                current.st_ino != status.st_ino)
                valid = 0;
        }
        if (valid && text_equals(operstate, "up")) found = 1;
        close(device);
        if (found) break;
    }
    closedir(directory);
    close(root);
    *connected = found;
    return 0;
}

static int read_sysfs(struct tomoe_network *network) {
    int connected = 0;
    int result = sysfs_scan(network, &connected);
    if (result < 0) {
        network->sysfs_connected = 0;
    } else {
        network->sysfs_connected = connected;
    }
    if (!network->nm_authoritative) public_from_source(network);
    return result;
}

static void start_sysfs(struct tomoe_network *network) {
    network->nm_authoritative = 0;
    network->sysfs_enabled = 1;
    if (read_sysfs(network) >= 0) {
        network->sysfs_next_usec =
            deadline_after(monotonic_usec(), NETWORK_SYSFS_INTERVAL_USEC);
    } else {
        network->sysfs_enabled = 0;
        network->sysfs_connected = 0;
        public_from_source(network);
    }
}

static void clear_ap(struct tomoe_network *network) {
    network->raw.ssid_known = 0;
    network->raw.ssid_length = 0;
    network->raw.strength_known = 0;
    network->raw.strength = 0;
    network->ssid_epoch++;
    network->strength_epoch++;
}

static void clear_conn(struct tomoe_network *network) {
    network->raw.type_known = 0;
    network->raw.type[0] = '\0';
    network->raw.specific_known = 0;
    network->raw.specific[0] = '\0';
    network->type_epoch++;
    network->specific_signal_epoch++;
    network->specific_chain_epoch++;
    clear_ap(network);
}

static void clear_primary(struct tomoe_network *network) {
    network->raw.primary_known = 0;
    network->raw.primary[0] = '\0';
    network->primary_chain_epoch++;
    network->primary_signal_epoch++;
    clear_conn(network);
}

static void reset_nm_values(struct tomoe_network *network) {
    network->raw.state_known = 0;
    network->raw.state = 0;
    network->state_epoch++;
    clear_primary(network);
}

static void clear_nm(struct tomoe_network *network) {
    network->owner[0] = '\0';
    network->owner_epoch++;
    if (!network->owner_epoch) network->owner_epoch++;
    cancel_stale_calls(network);
    reset_nm_values(network);
    network->nm_authoritative = 0;
    public_from_source(network);
}

static int read_string_value(sd_bus_message *message, char *destination,
                             size_t capacity, size_t limit) {
    const char *value = NULL;
    int result = sd_bus_message_read(message, "s", &value);
    if (result < 0) return result;
    return copy_bounded(destination, capacity, value, limit);
}

static int read_path_value(sd_bus_message *message, char *destination,
                           size_t capacity) {
    const char *value = NULL;
    int result = sd_bus_message_read(message, "o", &value);
    if (result < 0) return result;
    if (!valid_object_path(value)) return -EBADMSG;
    return copy_bounded(destination, capacity, value, NETWORK_MAX_PATH);
}

static int read_property_dict(sd_bus_message *message,
                              struct network_props *props) {
    int result = sd_bus_message_enter_container(message, 'a', "{sv}");
    if (result < 0) return result;
    if (!result) return -EBADMSG;
    size_t entries = 0;
    for (;;) {
        result = sd_bus_message_enter_container(message, 'e', "sv");
        if (result < 0) return result;
        if (!result) break;
        if (++entries > NETWORK_MAX_DICT) return -E2BIG;
        const char *key = NULL;
        result = sd_bus_message_read(message, "s", &key);
        if (result < 0 || !bounded_string(key, NETWORK_MAX_TEXT))
            return result < 0 ? result : -EBADMSG;
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
        if (!strcmp(key, "State")) {
            if (value_type != 'u') return -EBADMSG;
            result = sd_bus_message_read(message, "u", &props->state_value);
            if (result < 0) return result;
            props->state = 1;
        } else if (!strcmp(key, "PrimaryConnection")) {
            if (value_type != 'o') return -EBADMSG;
            result = read_path_value(message, props->primary_value,
                                     sizeof(props->primary_value));
            if (result < 0) return result;
            props->primary = 1;
        } else if (!strcmp(key, "Type")) {
            if (value_type != 's') return -EBADMSG;
            result = read_string_value(message, props->type_value,
                                       sizeof(props->type_value),
                                       NETWORK_MAX_TEXT);
            if (result < 0) return result;
            props->type = 1;
        } else if (!strcmp(key, "SpecificObject")) {
            if (value_type != 'o') return -EBADMSG;
            result = read_path_value(message, props->specific_value,
                                     sizeof(props->specific_value));
            if (result < 0) return result;
            props->specific = 1;
        } else if (!strcmp(key, "Ssid")) {
            if (value_type != 'a' || !value_contents ||
                strcmp(value_contents, "y")) return -EBADMSG;
            const void *data = NULL;
            size_t length = 0;
            result = sd_bus_message_read_array(message, 'y', &data, &length);
            if (result < 0) return result;
            if (length > NETWORK_MAX_SSID) return -E2BIG;
            if (length) memcpy(props->ssid_value, data, length);
            props->ssid_length = length;
            props->ssid = 1;
        } else if (!strcmp(key, "Strength")) {
            if (value_type != 'y') return -EBADMSG;
            result = sd_bus_message_read(message, "y", &props->strength_value);
            if (result < 0) return result;
            props->strength = 1;
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
                            struct network_invalidated *invalidated) {
    int result = sd_bus_message_enter_container(message, 'a', "s");
    if (result < 0) return result;
    if (!result) return -EBADMSG;
    size_t count = 0;
    for (;;) {
        const char *key = NULL;
        result = sd_bus_message_read(message, "s", &key);
        if (result < 0) return result;
        if (!result) break;
        if (++count > NETWORK_MAX_DICT ||
            !bounded_string(key, NETWORK_MAX_TEXT))
            return -E2BIG;
        if (!strcmp(key, "State")) invalidated->state = 1;
        else if (!strcmp(key, "PrimaryConnection")) invalidated->primary = 1;
        else if (!strcmp(key, "Type")) invalidated->type = 1;
        else if (!strcmp(key, "SpecificObject")) invalidated->specific = 1;
        else if (!strcmp(key, "Ssid")) invalidated->ssid = 1;
        else if (!strcmp(key, "Strength")) invalidated->strength = 1;
    }
    return sd_bus_message_exit_container(message);
}

static int reply_is_error(sd_bus_message *message) {
    return !message || sd_bus_message_get_error(message) != NULL;
}

static int message_path_is(sd_bus_message *message, const char *path) {
    const char *actual = sd_bus_message_get_path(message);
    return actual && !strcmp(actual, path);
}

static int sender_matches(const struct tomoe_network *network,
                          sd_bus_message *message, const char *well_known) {
    const char *sender = sd_bus_message_get_sender(message);
    (void)well_known;
    if (!sender || !valid_unique_name(sender)) return 0;
    return network->owner[0] &&
           !strcmp(sender, network->owner);
}

static int serial_epoch_current(uint64_t epoch, uint64_t current,
                                uint64_t serial, uint64_t applied) {
    return epoch == current && serial >= applied;
}

static int path_is_none(const char *path) {
    return !path[0] || !strcmp(path, NETWORK_NO_PATH);
}

static void mark_nm_observed(struct tomoe_network *network) {
    network->nm_authoritative = 1;
    network->sysfs_enabled = 0;
    public_from_source(network);
}

static void accept_primary(struct tomoe_network *network, const char *path,
                           int signal) {
    int changed = !network->raw.primary_known ||
        strcmp(network->raw.primary, path);
    if (changed) {
        clear_conn(network);
        network->primary_chain_epoch++;
    }
    if (signal) network->primary_signal_epoch++;
    memcpy(network->raw.primary, path, strlen(path) + 1U);
    network->raw.primary_known = 1;
    if (changed) {
        if (!path_is_none(path)) network->need_conn = 1;
        else network->need_conn = 0;
        cancel_stale_calls(network);
    } else if (!network->raw.type_known || !network->raw.specific_known) {
        network->need_conn = !path_is_none(path);
    }
}

static void accept_type(struct tomoe_network *network, const char *type,
                        int signal) {
    memcpy(network->raw.type, type, strlen(type) + 1U);
    network->raw.type_known = 1;
    if (signal) network->type_epoch++;
}

static void accept_specific(struct tomoe_network *network, const char *path,
                            int signal) {
    int changed = !network->raw.specific_known ||
        strcmp(network->raw.specific, path);
    if (changed) {
        clear_ap(network);
        network->specific_chain_epoch++;
    }
    if (signal) network->specific_signal_epoch++;
    memcpy(network->raw.specific, path, strlen(path) + 1U);
    network->raw.specific_known = 1;
    if (changed) {
        if (!path_is_none(path)) network->need_ap = 1;
        else network->need_ap = 0;
        cancel_stale_calls(network);
    } else if (!network->raw.ssid_known || !network->raw.strength_known) {
        network->need_ap = !path_is_none(path);
    }
}

static void accept_ssid(struct tomoe_network *network,
                        const unsigned char *ssid, size_t length, int signal) {
    if (length) memcpy(network->raw.ssid, ssid, length);
    network->raw.ssid_length = length;
    network->raw.ssid_known = 1;
    if (signal) network->ssid_epoch++;
}

static void accept_strength(struct tomoe_network *network, uint8_t strength,
                            int signal) {
    network->raw.strength = strength;
    network->raw.strength_known = 1;
    if (signal) network->strength_epoch++;
}

static void apply_values(struct tomoe_network *network,
                         enum network_scope scope,
                         const struct network_props *props,
                         const struct network_invalidated *invalidated,
                         const struct network_call *call) {
    int signal = !call;
    int accepted = 0;
    uint64_t serial = call ? call->request_serial : UINT64_MAX;
    if (scope == NETWORK_SCOPE_ROOT) {
        if (props->state &&
            (signal || serial_epoch_current(call->state_epoch,
                                            network->state_epoch, serial,
                                            network->applied_state_serial))) {
            network->raw.state = props->state_value;
            network->raw.state_known = 1;
            if (signal) network->state_epoch++;
            if (call) network->applied_state_serial = serial;
            accepted = 1;
        }
        if (props->primary &&
            (signal || serial_epoch_current(call->primary_signal_epoch,
                                            network->primary_signal_epoch, serial,
                                            network->applied_primary_serial))) {
            accept_primary(network, props->primary_value, signal);
            if (call) network->applied_primary_serial = serial;
            accepted = 1;
        }
        if (invalidated->state) {
            network->raw.state_known = 0;
            network->state_epoch++;
            network->need_root = 1;
        }
        if (invalidated->primary) {
            clear_primary(network);
            network->need_root = 1;
            cancel_stale_calls(network);
        }
    } else if (scope == NETWORK_SCOPE_CONN) {
        if (props->type &&
            (signal || serial_epoch_current(call->type_epoch,
                                            network->type_epoch, serial,
                                            network->applied_type_serial))) {
            accept_type(network, props->type_value, signal);
            if (call) network->applied_type_serial = serial;
            accepted = 1;
        }
        if (props->specific &&
            (signal || serial_epoch_current(call->specific_signal_epoch,
                                            network->specific_signal_epoch, serial,
                                            network->applied_specific_serial))) {
            accept_specific(network, props->specific_value, signal);
            if (call) network->applied_specific_serial = serial;
            accepted = 1;
        }
        if (invalidated->type) {
            network->raw.type_known = 0;
            network->type_epoch++;
            network->need_conn = 1;
        }
        if (invalidated->specific) {
            network->raw.specific_known = 0;
            network->raw.specific[0] = '\0';
            network->specific_signal_epoch++;
            clear_ap(network);
            network->need_conn = 1;
            network->need_ap = 0;
            cancel_stale_calls(network);
        }
    } else {
        if (props->ssid &&
            (signal || serial_epoch_current(call->ssid_epoch,
                                            network->ssid_epoch, serial,
                                            network->applied_ssid_serial))) {
            accept_ssid(network, props->ssid_value, props->ssid_length, signal);
            if (call) network->applied_ssid_serial = serial;
            accepted = 1;
        }
        if (props->strength &&
            (signal || serial_epoch_current(call->strength_epoch,
                                            network->strength_epoch, serial,
                                            network->applied_strength_serial))) {
            accept_strength(network, props->strength_value, signal);
            if (call) network->applied_strength_serial = serial;
            accepted = 1;
        }
        if (invalidated->ssid) {
            network->raw.ssid_known = 0;
            network->raw.ssid_length = 0;
            network->ssid_epoch++;
            network->need_ap = 1;
        }
        if (invalidated->strength) {
            network->raw.strength_known = 0;
            network->raw.strength = 0;
            network->strength_epoch++;
            network->need_ap = 1;
        }
    }
    if (accepted)
        mark_nm_observed(network);
    else
        public_from_source(network);
}

static void pending_remove(struct tomoe_network *network,
                           struct network_call *call) {
    struct network_call **cursor = &network->calls;
    while (*cursor && *cursor != call) cursor = &(*cursor)->next;
    if (*cursor) {
        *cursor = call->next;
        if (network->pending_count) network->pending_count--;
    }
}

static void clear_pending(struct tomoe_network *network) {
    struct network_call *call = network->calls;
    while (call) {
        struct network_call *next = call->next;
        if (call->slot) sd_bus_slot_unref(call->slot);
        free(call);
        call = next;
    }
    network->calls = NULL;
    network->pending_count = 0;
}

static int call_is_current(const struct tomoe_network *network,
                           const struct network_call *call) {
    if (call->owner_epoch != network->owner_epoch) return 0;
    if (call->scope == NETWORK_SCOPE_CONN)
        return network->raw.primary_known &&
            call->primary_chain_epoch == network->primary_chain_epoch &&
            !strcmp(call->path, network->raw.primary);
    if (call->scope == NETWORK_SCOPE_AP)
        return network->raw.specific_known &&
            call->specific_chain_epoch == network->specific_chain_epoch &&
            !strcmp(call->path, network->raw.specific);
    return 1;
}

static void cancel_stale_calls(struct tomoe_network *network) {
    struct network_call **cursor = &network->calls;
    while (*cursor) {
        struct network_call *call = *cursor;
        if (call_is_current(network, call)) {
            cursor = &call->next;
            continue;
        }
        *cursor = call->next;
        if (network->pending_count) network->pending_count--;
        if (call->slot) sd_bus_slot_unref(call->slot);
        free(call);
    }
}

static int pending_owner(const struct tomoe_network *network) {
    for (const struct network_call *call = network->calls; call;
         call = call->next)
        if (call->kind == NETWORK_CALL_OWNER) return 1;
    return 0;
}

static int pending_scope(const struct tomoe_network *network,
                         enum network_scope scope, const char *path) {
    for (const struct network_call *call = network->calls; call;
         call = call->next)
        if (call->kind == NETWORK_CALL_GET_ALL && call->scope == scope &&
            (!path || !strcmp(call->path, path)) && call_is_current(network, call) &&
            ((scope == NETWORK_SCOPE_ROOT &&
              call->state_epoch == network->state_epoch &&
              call->primary_signal_epoch == network->primary_signal_epoch) ||
             (scope == NETWORK_SCOPE_CONN &&
              call->type_epoch == network->type_epoch &&
              call->specific_signal_epoch == network->specific_signal_epoch) ||
             (scope == NETWORK_SCOPE_AP &&
              call->ssid_epoch == network->ssid_epoch &&
              call->strength_epoch == network->strength_epoch)))
            return 1;
    return 0;
}

static int async_reply(sd_bus_message *message, void *userdata,
                       sd_bus_error *ret_error);

static int queue_call(struct tomoe_network *network, struct network_call *call,
                      const char *destination, const char *path,
                      const char *interface, const char *member,
                      const char *types, ...) {
    if (!network->bus || network->bus_disabled ||
        network->pending_count >= NETWORK_MAX_PENDING) {
        free(call);
        return -ENOSPC;
    }
    call->network = network;
    call->deadline_usec =
        deadline_after(monotonic_usec(), NETWORK_REQUEST_USEC);
    call->next = network->calls;
    network->calls = call;
    network->pending_count++;
    va_list ap;
    va_start(ap, types);
    int result = sd_bus_call_method_asyncv(
        network->bus, &call->slot, destination, path, interface, member,
        async_reply, call, types, ap);
    va_end(ap);
    if (result < 0) {
        pending_remove(network, call);
        if (call->slot) sd_bus_slot_unref(call->slot);
        free(call);
        return result;
    }
    return 0;
}

static void capture_epochs(struct tomoe_network *network,
                           struct network_call *call) {
    call->owner_epoch = network->owner_epoch;
    call->primary_chain_epoch = network->primary_chain_epoch;
    call->primary_signal_epoch = network->primary_signal_epoch;
    call->specific_chain_epoch = network->specific_chain_epoch;
    call->specific_signal_epoch = network->specific_signal_epoch;
    call->state_epoch = network->state_epoch;
    call->type_epoch = network->type_epoch;
    call->ssid_epoch = network->ssid_epoch;
    call->strength_epoch = network->strength_epoch;
    memcpy(call->owner, network->owner, sizeof(call->owner));
}

static int queue_owner(struct tomoe_network *network) {
    if (!network->bus || pending_owner(network)) return 0;
    struct network_call *call = calloc(1, sizeof(*call));
    if (!call) return -ENOMEM;
    call->kind = NETWORK_CALL_OWNER;
    call->scope = NETWORK_SCOPE_ROOT;
    capture_epochs(network, call);
    int result = queue_call(network, call, NETWORK_DBUS, NETWORK_DBUS_PATH,
                            NETWORK_DBUS, "GetNameOwner", "s", NETWORK_NM);
    if (result == -EAGAIN || result == -ENOSPC) {
        network->need_owner = 1;
        return 0;
    }
    if (result >= 0) network->need_owner = 0;
    return result;
}

static int queue_get_all(struct tomoe_network *network,
                         enum network_scope scope, const char *path,
                         const char *interface) {
    if (!network->bus || network->pending_count >= NETWORK_MAX_PENDING ||
        pending_scope(network, scope, path))
        return -ENOSPC;
    struct network_call *call = calloc(1, sizeof(*call));
    if (!call) return -ENOMEM;
    call->kind = NETWORK_CALL_GET_ALL;
    call->scope = scope;
    capture_epochs(network, call);
    if (copy_bounded(call->path, sizeof(call->path), path, NETWORK_MAX_PATH) < 0) {
        free(call);
        return -E2BIG;
    }
    call->request_serial = ++network->next_request_serial;
    if (!call->request_serial) call->request_serial = ++network->next_request_serial;
    int result = queue_call(network, call, NETWORK_NM, path,
                            NETWORK_PROPERTIES,
                            "GetAll", "s", interface);
    if (result >= 0) {
        if (scope == NETWORK_SCOPE_ROOT) network->need_root = 0;
        else if (scope == NETWORK_SCOPE_CONN) network->need_conn = 0;
        else network->need_ap = 0;
    }
    return result;
}

static void service_needs(struct tomoe_network *network) {
    if (network->need_owner && !network->owner[0])
        (void)queue_owner(network);
    if (!network->owner[0]) {
        if (network->need_root &&
            !pending_scope(network, NETWORK_SCOPE_ROOT, NULL))
            (void)queue_get_all(network, NETWORK_SCOPE_ROOT,
                                NETWORK_ROOT_PATH, NETWORK_ROOT_IFACE);
        return;
    }
    if (network->need_root && !pending_scope(network, NETWORK_SCOPE_ROOT, NULL))
        (void)queue_get_all(network, NETWORK_SCOPE_ROOT, NETWORK_ROOT_PATH,
                            NETWORK_ROOT_IFACE);
    if (network->need_conn && network->raw.primary_known &&
        !path_is_none(network->raw.primary) &&
        !pending_scope(network, NETWORK_SCOPE_CONN, network->raw.primary))
        (void)queue_get_all(network, NETWORK_SCOPE_CONN, network->raw.primary,
                            NETWORK_CONN_IFACE);
    if (network->need_ap && network->raw.specific_known &&
        !path_is_none(network->raw.specific) &&
        !pending_scope(network, NETWORK_SCOPE_AP, network->raw.specific))
        (void)queue_get_all(network, NETWORK_SCOPE_AP, network->raw.specific,
                            NETWORK_AP_IFACE);
}

static void fallback_to_sysfs(struct tomoe_network *network) {
    clear_nm(network);
    start_sysfs(network);
}

static void fallback_to_sysfs_keep_owner(struct tomoe_network *network) {
    start_sysfs(network);
}

static void handle_owner_reply(struct network_call *call,
                               sd_bus_message *message) {
    struct tomoe_network *network = call->network;
    if (call->owner_epoch != network->owner_epoch) return;
    if (reply_is_error(message)) {
        network->need_root = 1;
        return;
    }
    const char *owner = NULL;
    if (sd_bus_message_read(message, "s", &owner) < 0 ||
        !valid_unique_name(owner)) {
        fallback_to_sysfs(network);
        return;
    }
    if (network->owner[0] && strcmp(network->owner, owner)) return;
    if (!network->owner[0]) reset_nm_values(network);
    memcpy(network->owner, owner, strlen(owner) + 1U);
    network->owner_epoch++;
    if (!network->owner_epoch) network->owner_epoch++;
    cancel_stale_calls(network);
    network->need_owner = 0;
    network->need_root = 1;
}

static void handle_get_all_reply(struct network_call *call,
                                 sd_bus_message *message) {
    struct tomoe_network *network = call->network;
    if (!call_is_current(network, call)) return;
    if (reply_is_error(message)) {
        if (!network->owner[0] || !call->owner[0])
            fallback_to_sysfs(network);
        else if (!network->nm_authoritative)
            fallback_to_sysfs_keep_owner(network);
        return;
    }
    const char *sender = sd_bus_message_get_sender(message);
    if (!sender || !valid_unique_name(sender)) return;
    if (network->owner[0] && strcmp(network->owner, sender)) return;
    if (!network->owner[0]) {
        memcpy(network->owner, sender, strlen(sender) + 1U);
        network->owner_epoch++;
        if (!network->owner_epoch) network->owner_epoch++;
        cancel_stale_calls(network);
    }
    struct network_props props = {0};
    if (read_property_dict(message, &props) < 0) {
        if (!network->nm_authoritative)
            fallback_to_sysfs_keep_owner(network);
        return;
    }
    struct network_invalidated none = {0};
    apply_values(network, call->scope, &props, &none, call);
}

static int scope_for(const struct tomoe_network *network, const char *iface,
                     const char *path, enum network_scope *scope) {
    if (!strcmp(iface, NETWORK_ROOT_IFACE) &&
        !strcmp(path, NETWORK_ROOT_PATH)) {
        *scope = NETWORK_SCOPE_ROOT;
        return 1;
    }
    if (!strcmp(iface, NETWORK_CONN_IFACE) && network->raw.primary_known &&
        !strcmp(path, network->raw.primary)) {
        *scope = NETWORK_SCOPE_CONN;
        return 1;
    }
    if (!strcmp(iface, NETWORK_AP_IFACE) && network->raw.specific_known &&
        !strcmp(path, network->raw.specific)) {
        *scope = NETWORK_SCOPE_AP;
        return 1;
    }
    return 0;
}

static int owner_changed_signal(sd_bus_message *message, void *userdata,
                                sd_bus_error *ret_error) {
    struct tomoe_network *network = userdata;
    (void)ret_error;
    if (!message_path_is(message, NETWORK_DBUS_PATH)) return 1;
    const char *sender = sd_bus_message_get_sender(message);
    if (!sender || strcmp(sender, NETWORK_DBUS)) return 1;
    const char *name = NULL;
    const char *old_owner = NULL;
    const char *new_owner = NULL;
    int result = sd_bus_message_read(message, "sss", &name, &old_owner,
                                     &new_owner);
    if (result < 0 || !name || strcmp(name, NETWORK_NM) ||
        (old_owner && *old_owner && !valid_unique_name(old_owner)) ||
        (new_owner && *new_owner && !valid_unique_name(new_owner)))
        return 1;
    if (network->owner[0] && old_owner && *old_owner &&
        strcmp(network->owner, old_owner))
        return 1;
    if (new_owner && *new_owner) {
        network->owner_epoch++;
        if (!network->owner_epoch) network->owner_epoch++;
        cancel_stale_calls(network);
        reset_nm_values(network);
        memcpy(network->owner, new_owner, strlen(new_owner) + 1U);
        network->need_owner = 0;
        network->need_root = 1;
        start_sysfs(network);
    } else {
        fallback_to_sysfs(network);
    }
    return 1;
}

static int properties_signal(sd_bus_message *message, void *userdata,
                             sd_bus_error *ret_error) {
    struct tomoe_network *network = userdata;
    (void)ret_error;
    if (!sender_matches(network, message, NETWORK_NM)) return 1;
    const char *path = sd_bus_message_get_path(message);
    const char *interface = NULL;
    if (!path || sd_bus_message_read(message, "s", &interface) < 0 ||
        !interface)
        return 1;
    enum network_scope scope;
    if (!scope_for(network, interface, path, &scope)) return 1;
    struct network_props props = {0};
    struct network_invalidated invalidated = {0};
    if (read_property_dict(message, &props) < 0 ||
        read_invalidated(message, &invalidated) < 0)
        return 1;
    apply_values(network, scope, &props, &invalidated, NULL);
    return 1;
}

static int legacy_state_signal(sd_bus_message *message, void *userdata,
                               sd_bus_error *ret_error) {
    struct tomoe_network *network = userdata;
    (void)ret_error;
    if (!message_path_is(message, NETWORK_ROOT_PATH) ||
        !sender_matches(network, message, NETWORK_NM)) return 1;
    uint32_t state = 0;
    if (sd_bus_message_read(message, "u", &state) < 0) return 1;
    network->raw.state = state;
    network->raw.state_known = 1;
    network->state_epoch++;
    mark_nm_observed(network);
    return 1;
}

static int async_reply(sd_bus_message *message, void *userdata,
                       sd_bus_error *ret_error) {
    struct network_call *call = userdata;
    (void)ret_error;
    if (!call || !call->network) return 1;
    struct tomoe_network *network = call->network;
    sd_bus_slot *slot = call->slot;
    call->slot = NULL;
    pending_remove(network, call);
    if (call->kind == NETWORK_CALL_OWNER)
        handle_owner_reply(call, message);
    else
        handle_get_all_reply(call, message);
    if (slot) sd_bus_slot_unref(slot);
    free(call);
    return 1;
}

static void close_bus(struct tomoe_network *network) {
    if (!network) return;
    for (size_t index = 0; index < sizeof(network->matches) /
                                      sizeof(network->matches[0]); index++) {
        if (network->matches[index]) {
            sd_bus_slot_unref(network->matches[index]);
            network->matches[index] = NULL;
        }
    }
    clear_pending(network);
    if (network->bus) {
        sd_bus_close(network->bus);
        sd_bus_unref(network->bus);
        network->bus = NULL;
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

static void disconnect_bus(struct tomoe_network *network) {
    network->bus_disabled = 1;
    close_bus(network);
    fallback_to_sysfs(network);
}

struct tomoe_network *tomoe_network_open(const char *sysfs_root, int *error) {
    if (!error) return NULL;
    *error = EINVAL;
    struct tomoe_network *network = calloc(1, sizeof(*network));
    if (!network) {
        *error = ENOMEM;
        return NULL;
    }
    const char *root = sysfs_root && *sysfs_root
        ? sysfs_root : NETWORK_DEFAULT_ROOT;
    if (!bounded_string(root, NETWORK_MAX_PATH)) {
        free(network);
        *error = ENAMETOOLONG;
        return NULL;
    }
    memcpy(network->sysfs_root, root, strlen(root) + 1U);
    public_default(network);
    network->revision = 1;

    sd_bus *bus = NULL;
    int result = sd_bus_new(&bus);
    if (result < 0) goto fallback;
    result = sd_bus_set_bus_client(bus, 1);
    if (result < 0) goto fallback_bus;
    const char *address = getenv("DBUS_SYSTEM_BUS_ADDRESS");
    if (!address || !*address) address = NETWORK_SYSTEM_ADDRESS;
    result = sd_bus_set_address(bus, address);
    if (result < 0) goto fallback_bus;
    result = sd_bus_set_method_call_timeout(bus, NETWORK_SETUP_USEC);
    if (result < 0) goto fallback_bus;
    result = sd_bus_start(bus);
    if (result < 0) goto fallback_bus;
    uint64_t deadline = deadline_after(monotonic_usec(), NETWORK_SETUP_USEC);
    result = setup_ready(bus, deadline);
    if (result < 0) goto fallback_bus;
    network->bus = bus;
    bus = NULL;
    result = set_remaining_timeout(network->bus, deadline);
    if (result < 0) goto fallback_bus_handle;
    result = sd_bus_add_match(
        network->bus, &network->matches[0],
        "type='signal',sender='org.freedesktop.NetworkManager',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',path_namespace='/org/freedesktop/NetworkManager'",
        properties_signal, network);
    if (result < 0) goto fallback_bus_handle;
    result = set_remaining_timeout(network->bus, deadline);
    if (result < 0) goto fallback_bus_handle;
    result = sd_bus_add_match(
        network->bus, &network->matches[1],
        "type='signal',sender='org.freedesktop.NetworkManager',path='/org/freedesktop/NetworkManager',interface='org.freedesktop.NetworkManager',member='StateChanged'",
        legacy_state_signal, network);
    if (result < 0) goto fallback_bus_handle;
    result = set_remaining_timeout(network->bus, deadline);
    if (result < 0) goto fallback_bus_handle;
    result = sd_bus_add_match(
        network->bus, &network->matches[2],
        "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.freedesktop.NetworkManager'",
        owner_changed_signal, network);
    if (result < 0) goto fallback_bus_handle;
    network->need_owner = 1;
    result = queue_owner(network);
    if (result < 0) goto fallback_bus_handle;
    result = sd_bus_set_method_call_timeout(network->bus, NETWORK_REQUEST_USEC);
    if (result < 0) goto fallback_bus_handle;
    *error = 0;
    return network;

fallback_bus_handle:
    close_bus(network);
    network->bus_disabled = 1;
    goto fallback;
fallback_bus:
    if (bus) {
        sd_bus_close(bus);
        sd_bus_unref(bus);
        bus = NULL;
    }
fallback:
    network->bus_disabled = 1;
    start_sysfs(network);
    *error = 0;
    return network;
}

int tomoe_network_poll(struct tomoe_network *network) {
    if (!network) return -EINVAL;
    network->changed = 0;
    service_needs(network);
    uint64_t start = monotonic_usec();
    size_t processed = 0;
    while (network->bus && processed < NETWORK_POLL_MESSAGES) {
        uint64_t now = monotonic_usec();
        if (now >= start && now - start >= NETWORK_POLL_BUDGET_USEC) break;
        int result = sd_bus_process(network->bus, NULL);
        if (result < 0) {
            if (result == -EAGAIN || result == -EINTR) break;
            disconnect_bus(network);
            break;
        }
        if (!result) break;
        processed++;
        service_needs(network);
    }
    if (network->sysfs_enabled) {
        uint64_t now = monotonic_usec();
        if (now >= network->sysfs_next_usec) {
            (void)read_sysfs(network);
            network->sysfs_next_usec =
                deadline_after(now, NETWORK_SYSFS_INTERVAL_USEC);
        }
    }
    service_needs(network);
    return network->changed ? 1 : 0;
}

uint64_t tomoe_network_revision(const struct tomoe_network *network) {
    return network ? network->revision : 0;
}

int tomoe_network_timeout(const struct tomoe_network *network, int max_ms) {
    if (!network) return 0;
    if (max_ms < 0) max_ms = 0;
    uint64_t best = UINT64_MAX;
    for (const struct network_call *call = network->calls; call;
         call = call->next)
        if (call->deadline_usec < best) best = call->deadline_usec;
    if (network->sysfs_enabled && network->sysfs_next_usec < best)
        best = network->sysfs_next_usec;
    if (best == UINT64_MAX) return max_ms;
    uint64_t now = monotonic_usec();
    if (best <= now) return 0;
    uint64_t milliseconds = (best - now + 999ULL) / 1000ULL;
    if (milliseconds >= (uint64_t)max_ms) return max_ms;
    return (int)milliseconds;
}

void tomoe_network_close(struct tomoe_network *network) {
    if (!network) return;
    close_bus(network);
    free(network->snapshot);
    free(network);
}
