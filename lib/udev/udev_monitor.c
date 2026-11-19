#include "udev_priv.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/magic.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <unistd.h>

#define UDEV_MONITOR_MAGIC 0xfeedcafeu

struct monitor_header {
    char prefix[8];
    uint32_t magic;
    uint32_t header_size;
    uint32_t properties_off;
    uint32_t properties_len;
    uint32_t filter_subsystem_hash;
    uint32_t filter_devtype_hash;
    uint32_t filter_tag_bloom_hi;
    uint32_t filter_tag_bloom_lo;
};

static uint32_t murmur2(const char *key, uint32_t seed) {
    const uint32_t m = 0x5bd1e995u;
    size_t len = strlen(key);
    uint32_t h = seed ^ (uint32_t)len;
    const unsigned char *data = (const unsigned char *)key;
    while (len >= 4) {
        uint32_t k;
        memcpy(&k, data, sizeof(k));
        k *= m;
        k ^= k >> 24;
        k *= m;
        h *= m;
        h ^= k;
        data += 4;
        len -= 4;
    }
    switch (len) {
    case 3:
        h ^= (uint32_t)data[2] << 16;
        h ^= (uint32_t)data[1] << 8;
        h ^= data[0];
        h *= m;
        break;
    case 2:
        h ^= (uint32_t)data[1] << 8;
        h ^= data[0];
        h *= m;
        break;
    case 1:
        h ^= data[0];
        h *= m;
        break;
    default:
        break;
    }
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return h;
}

static uint32_t monitor_group(const char *name) {
    if (!name) return 0;
    if (strcmp(name, "udev") == 0) return 2;
    if (strcmp(name, "kernel") == 0) return 1;
    return UINT32_MAX;
}

static int devtmpfs(void) {
    struct statfs st;
    if (statfs("/dev", &st) < 0) return 0;
    return st.f_type == TMPFS_MAGIC;
}

struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev, const char *name) {
    unsigned group = monitor_group(name);
    if (group == UINT32_MAX) {
        errno = EINVAL;
        return NULL;
    }
    if (group == 2 && access("/run/udev/control", F_OK) < 0 && !devtmpfs()) group = 0;
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
                    NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return NULL;
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return NULL;
    }
    struct sockaddr_nl address;
    memset(&address, 0, sizeof(address));
    address.nl_family = AF_NETLINK;
    address.nl_groups = group;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return NULL;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, (struct sockaddr *)&address, &length) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return NULL;
    }
    int buffer = 128 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    struct udev_monitor *monitor = tudev_new0(sizeof(*monitor));
    if (!monitor) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    monitor->udev = udev;
    monitor->n_ref = 1;
    monitor->fd = fd;
    monitor->group = group;
    monitor->trusted_pid = address.nl_pid;
    return monitor;
}

struct udev_monitor *udev_monitor_ref(struct udev_monitor *monitor) {
    if (monitor) monitor->n_ref++;
    return monitor;
}

struct udev_monitor *udev_monitor_unref(struct udev_monitor *monitor) {
    if (!monitor) return NULL;
    if (monitor->n_ref > 1) {
        monitor->n_ref--;
        return monitor;
    }
    close(monitor->fd);
    for (size_t i = 0; i < monitor->nfilters; i++) {
        free(monitor->filters[i].subsystem);
        free(monitor->filters[i].devtype);
    }
    free(monitor->filters);
    free(monitor);
    return NULL;
}

struct udev *udev_monitor_get_udev(struct udev_monitor *monitor) {
    if (!monitor) {
        errno = EINVAL;
        return NULL;
    }
    return monitor->udev;
}

int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *monitor,
        const char *subsystem, const char *devtype) {
    if (!monitor) return -EINVAL;
    if (!subsystem) return -EINVAL;
    for (size_t i = 0; i < monitor->nfilters; i++)
        if (strcmp(monitor->filters[i].subsystem, subsystem) == 0) {
            char *copy = tudev_strdup(devtype);
            if (devtype && !copy) return -ENOMEM;
            free(monitor->filters[i].devtype);
            monitor->filters[i].devtype = copy;
            return 0;
        }
    if (monitor->nfilters == monitor->cfilters) {
        size_t next = monitor->cfilters ? monitor->cfilters * 2 : 4;
        struct udev_monitor_filter *grown = realloc(monitor->filters,
                                                    next * sizeof(*grown));
        if (!grown) return -ENOMEM;
        monitor->filters = grown;
        monitor->cfilters = next;
    }
    struct udev_monitor_filter *filter = &monitor->filters[monitor->nfilters];
    filter->subsystem = strdup(subsystem);
    filter->devtype = tudev_strdup(devtype);
    if (!filter->subsystem || (devtype && !filter->devtype)) {
        free(filter->subsystem);
        free(filter->devtype);
        return -ENOMEM;
    }
    monitor->nfilters++;
    return 0;
}

static int install_filter(struct udev_monitor *monitor) {
    if (monitor->group == 1 || monitor->nfilters == 0) return 0;
    struct sock_filter ins[512];
    memset(ins, 0, sizeof(ins));
    size_t i = 0;
    ins[i++] = (struct sock_filter) {
        .code = BPF_LD | BPF_W | BPF_ABS, .k = offsetof(struct monitor_header, magic)
    };
    ins[i++] = (struct sock_filter) {
        .code = BPF_JMP | BPF_JEQ | BPF_K, .jt = 1, .k = UDEV_MONITOR_MAGIC
    };
    ins[i++] = (struct sock_filter) {
        .code = BPF_RET | BPF_K, .k = 0xffffffffu
    };
    for (size_t f = 0; f < monitor->nfilters; f++) {
        uint32_t hash = murmur2(monitor->filters[f].subsystem, 0);
        ins[i++] = (struct sock_filter) {
            .code = BPF_LD | BPF_W | BPF_ABS,
            .k = offsetof(struct monitor_header, filter_subsystem_hash)
        };
        if (!monitor->filters[f].devtype) {
            ins[i++] = (struct sock_filter) {
                .code = BPF_JMP | BPF_JEQ | BPF_K, .jf = 1, .k = hash
            };
        } else {
            ins[i++] = (struct sock_filter) {
                .code = BPF_JMP | BPF_JEQ | BPF_K, .jf = 3, .k = hash
            };
            ins[i++] = (struct sock_filter) {
                .code = BPF_LD | BPF_W | BPF_ABS,
                .k = offsetof(struct monitor_header, filter_devtype_hash)
            };
            hash = murmur2(monitor->filters[f].devtype, 0);
            ins[i++] = (struct sock_filter) {
                .code = BPF_JMP | BPF_JEQ | BPF_K, .jf = 1, .k = hash
            };
        }
        ins[i++] = (struct sock_filter) {
            .code = BPF_RET | BPF_K, .k = 0xffffffffu
        };
        if (i + 2 >= sizeof(ins) / sizeof(ins[0])) return -E2BIG;
    }
    ins[i++] = (struct sock_filter) { .code = BPF_RET | BPF_K, .k = 0 };
    ins[i++] = (struct sock_filter) { .code = BPF_RET | BPF_K, .k = 0xffffffffu };
    struct sock_fprog program = { .len = (unsigned short)i, .filter = ins };
    if (setsockopt(monitor->fd, SOL_SOCKET, SO_ATTACH_FILTER, &program, sizeof(program)) < 0)
        return -errno;
    return 0;
}

int udev_monitor_enable_receiving(struct udev_monitor *monitor) {
    if (!monitor) return -EINVAL;
    return install_filter(monitor);
}

int udev_monitor_get_fd(struct udev_monitor *monitor) {
    if (!monitor) return -EINVAL;
    return monitor->fd;
}

static int sender_trusted(uid_t uid) {
    if (uid == 0) return 1;
    if (uid == getuid() || uid == geteuid()) return 1;
    return 0;
}

static int passes_filter(struct udev_monitor *monitor, struct udev_device *device) {
    if (monitor->nfilters == 0) return 1;
    const char *subsystem = tudev_device_subsystem(device);
    if (!subsystem) return -ENOENT;
    for (size_t i = 0; i < monitor->nfilters; i++) {
        if (strcmp(monitor->filters[i].subsystem, subsystem) != 0) continue;
        if (!monitor->filters[i].devtype) return 1;
        const char *devtype = udev_device_get_devtype(device);
        if (devtype && strcmp(devtype, monitor->filters[i].devtype) == 0) return 1;
    }
    return 0;
}

static int next_datagram_size(int fd) {
    ssize_t size = recv(fd, NULL, 0, MSG_PEEK | MSG_TRUNC);
    if (size < 0) {
        if (errno != EOPNOTSUPP && errno != EFAULT) return -errno;
        int fallback = 0;
        if (ioctl(fd, FIONREAD, &fallback) < 0) return -errno;
        size = fallback;
    }
    if (size <= 0) return -EAGAIN;
    return (int)size;
}

static int datagram_properties(const char *buffer, size_t length, size_t *offset) {
    if (length < 32 || !memchr(buffer, 0, length)) return -1;
    if (strcmp(buffer, "libudev") == 0) {
        struct monitor_header header;
        if (length < sizeof(header)) return -1;
        memcpy(&header, buffer, sizeof(header));
        if (header.magic != htobe32(UDEV_MONITOR_MAGIC)) return -1;
        if ((uint64_t)header.properties_off + 32 > (uint64_t)length) return -1;
        *offset = header.properties_off;
        return 1;
    }
    if (!strstr(buffer, "@/")) return -1;
    *offset = strlen(buffer) + 1;
    if (*offset >= length) return -1;
    return 0;
}

static int receive_one(struct udev_monitor *monitor, struct udev_device **ret) {
    int available = next_datagram_size(monitor->fd);
    if (available < 0) return available;
    size_t size = (size_t)available;
    char *buffer = malloc(size + 1);
    if (!buffer) return -ENOMEM;
    struct iovec iov = { .iov_base = buffer, .iov_len = size };
    union {
        char bytes[CMSG_SPACE(sizeof(struct ucred))];
        struct cmsghdr align;
    } control;
    struct sockaddr_nl address;
    memset(&address, 0, sizeof(address));
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_name = &address;
    message.msg_namelen = sizeof(address);
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    ssize_t received = recvmsg(monitor->fd, &message, 0);
    if (received < 0) {
        int error = errno;
        free(buffer);
        return -error;
    }
    ssize_t length = received;
    if (message.msg_flags & MSG_TRUNC) {
        free(buffer);
        return -EAGAIN;
    }
    if (length < 32) {
        free(buffer);
        return -EINVAL;
    }
    buffer[length] = 0;
    if (address.nl_groups == 0) {
        if (monitor->trusted_pid == 0 || address.nl_pid != monitor->trusted_pid) {
            free(buffer);
            return -EAGAIN;
        }
    } else if (address.nl_groups == 1 && address.nl_pid > 0) {
        free(buffer);
        return -EAGAIN;
    }
    struct ucred *credentials = NULL;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message); cmsg;
         cmsg = CMSG_NXTHDR(&message, cmsg))
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS) {
            credentials = (struct ucred *)CMSG_DATA(cmsg);
            break;
        }
    if (!credentials || !sender_trusted(credentials->uid)) {
        free(buffer);
        return -EAGAIN;
    }
    size_t offset;
    int initialized = datagram_properties(buffer, (size_t)length, &offset);
    if (initialized < 0) {
        free(buffer);
        return -EAGAIN;
    }
    struct udev_device *device = tudev_device_new(monitor->udev);
    if (!device) {
        free(buffer);
        return -ENOMEM;
    }
    int r = tudev_device_from_nulstr(device, buffer + offset, (size_t)length - offset);
    free(buffer);
    if (r < 0) {
        udev_device_unref(device);
        return r;
    }
    if (initialized) device->is_initialized = 1;
    r = passes_filter(monitor, device);
    if (r < 0) {
        udev_device_unref(device);
        return r;
    }
    if (r == 0) {
        udev_device_unref(device);
        *ret = NULL;
        return 0;
    }
    *ret = device;
    return 1;
}

struct udev_device *udev_monitor_receive_device(struct udev_monitor *monitor) {
    if (!monitor) {
        errno = EINVAL;
        return NULL;
    }
    for (;;) {
        struct udev_device *device = NULL;
        int r = receive_one(monitor, &device);
        if (r < 0) {
            errno = -r;
            return NULL;
        }
        if (r == 1) return device;
        struct pollfd descriptor = { .fd = monitor->fd, .events = POLLIN };
        int ready = poll(&descriptor, 1, 0);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }
        if (ready == 0) {
            errno = EAGAIN;
            return NULL;
        }
    }
}
