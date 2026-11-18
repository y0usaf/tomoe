#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLIENT_OPEN_SEAT 1
#define CLIENT_CLOSE_SEAT 2
#define CLIENT_OPEN_DEVICE 3
#define CLIENT_CLOSE_DEVICE 4
#define CLIENT_DISABLE_SEAT 5
#define CLIENT_SWITCH_SESSION 6
#define CLIENT_PING 7
#define SERVER_SEAT_OPENED 0x8001
#define SERVER_SEAT_CLOSED 0x8002
#define SERVER_DEVICE_OPENED 0x8003
#define SERVER_DEVICE_CLOSED 0x8004
#define SERVER_DISABLE_SEAT 0x8005
#define SERVER_ENABLE_SEAT 0x8006
#define SERVER_PONG 0x8007
#define SERVER_SESSION_SWITCHED 0x8008
#define SERVER_SEAT_DISABLED 0x8009
#define SERVER_ERROR 0xffff
#define DRM_IOCTL_SET_MASTER 0x641e

struct frame {
    uint16_t opcode;
    const unsigned char *payload;
    size_t size;
};

static void send_frames(int connection, const struct frame *frames, int count, int fd) {
    size_t total = 0;
    for (int index = 0; index < count; index++) total += 4 + frames[index].size;
    unsigned char *data = malloc(total ? total : 1);
    if (!data) exit(1);
    size_t at = 0;
    for (int index = 0; index < count; index++) {
        data[at++] = (unsigned char)(frames[index].opcode & 0xff);
        data[at++] = (unsigned char)(frames[index].opcode >> 8);
        data[at++] = (unsigned char)(frames[index].size & 0xff);
        data[at++] = (unsigned char)(frames[index].size >> 8);
        if (frames[index].size) memcpy(data + at, frames[index].payload, frames[index].size);
        at += frames[index].size;
    }
    if (fd >= 0) {
        struct iovec vector = { .iov_base = data, .iov_len = total };
        char control[CMSG_SPACE(sizeof(int))];
        memset(control, 0, sizeof control);
        struct msghdr message;
        memset(&message, 0, sizeof message);
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof control;
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &fd, sizeof(int));
        while (sendmsg(connection, &message, 0) < 0 && errno == EINTR) {}
    } else {
        size_t sent = 0;
        while (sent < total) {
            ssize_t used = send(connection, data + sent, total - sent, 0);
            if (used < 0) {
                if (errno == EINTR) continue;
                break;
            }
            sent += (size_t)used;
        }
    }
    free(data);
}

static int receive_exact(int connection, unsigned char *data, size_t count) {
    size_t got = 0;
    while (got < count) {
        ssize_t used = recv(connection, data + got, count - got, 0);
        if (used < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (used == 0) return 0;
        got += (size_t)used;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s SOCKET MODE\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    const char *mode = argv[2];
    unlink(path);
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
        fprintf(stderr, "fake-seatd: socket: %s\n", strerror(errno));
        return 1;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    size_t path_length = strlen(path);
    if (path_length >= sizeof address.sun_path) {
        fprintf(stderr, "socket path is too long\n");
        return 1;
    }
    memcpy(address.sun_path, path, path_length + 1);
    if (bind(server, (struct sockaddr *)&address, sizeof address) != 0) {
        fprintf(stderr, "fake-seatd: bind %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (listen(server, 1) != 0) {
        fprintf(stderr, "fake-seatd: listen: %s\n", strerror(errno));
        return 1;
    }
    int connection = accept(server, NULL, NULL);
    if (connection < 0) {
        fprintf(stderr, "fake-seatd: accept: %s\n", strerror(errno));
        return 1;
    }
    int opened = 0, refused = 0, handed = 0, pinged = 0, acked = 0, closed = 0;
    unsigned char header[4];
    for (;;) {
        if (!receive_exact(connection, header, 4)) break;
        uint16_t opcode = (uint16_t)(header[0] | (header[1] << 8));
        uint16_t size = (uint16_t)(header[2] | (header[3] << 8));
        unsigned char *payload = calloc(size ? size : 1, 1);
        if (!payload) return 1;
        if (size && !receive_exact(connection, payload, size)) {
            free(payload);
            break;
        }
        printf("request %u\n", (unsigned)opcode);
        fflush(stdout);
        if (opcode == CLIENT_OPEN_SEAT) {
            opened = 1;
            static const unsigned char seat[] = { 6, 0, 's', 'e', 'a', 't', '0', 0 };
            struct frame frames[2] = {
                { SERVER_SEAT_OPENED, seat, sizeof seat },
                { SERVER_ENABLE_SEAT, NULL, 0 },
            };
            send_frames(connection, frames, 2, -1);
        } else if (opcode == CLIENT_OPEN_DEVICE) {
            char device[4096];
            size_t at = size > 2 ? 2 : size;
            size_t used = 0;
            while (at < size && payload[at] && used + 1 < sizeof device)
                device[used++] = (char)payload[at++];
            device[used] = '\0';
            printf("open %s\n", device);
            fflush(stdout);
            if (strcmp(mode, "error") == 0) {
                refused = 1;
                static const unsigned char code[] = { 2, 0, 0, 0 };
                struct frame frames[2] = {
                    { SERVER_ERROR, code, sizeof code },
                    { SERVER_DISABLE_SEAT, NULL, 0 },
                };
                send_frames(connection, frames, 2, -1);
                printf("refused with disable\n");
                fflush(stdout);
            } else {
                int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
                if (fd < 0) {
                    fprintf(stderr, "fake-seatd: open %s: %s\n", device, strerror(errno));
                    free(payload);
                    break;
                }
                struct frame frames[2];
                if (strncmp(device, "/dev/dri/card", 13) == 0) {
                    ioctl(fd, DRM_IOCTL_SET_MASTER, 0);
                    handed = 1;
                    static const unsigned char card[] = { 1, 0, 0, 0 };
                    frames[0] = (struct frame){ SERVER_DEVICE_OPENED, card, sizeof card };
                    frames[1] = (struct frame){ SERVER_DISABLE_SEAT, NULL, 0 };
                    printf("opened with disable\n");
                    fflush(stdout);
                } else {
                    static const unsigned char other[] = { 2, 0, 0, 0 };
                    frames[0] = (struct frame){ SERVER_DEVICE_OPENED, other, sizeof other };
                    frames[1] = (struct frame){ 0, NULL, 0 };
                }
                send_frames(connection, frames, strncmp(device, "/dev/dri/card", 13) == 0 ? 2 : 1, fd);
                close(fd);
            }
        } else if (opcode == CLIENT_PING) {
            pinged = 1;
            printf("ping\n");
            fflush(stdout);
            struct frame frames[1] = { { SERVER_PONG, NULL, 0 } };
            send_frames(connection, frames, 1, -1);
        } else if (opcode == CLIENT_DISABLE_SEAT) {
            acked = 1;
            printf("disable acked\n");
            fflush(stdout);
            struct frame frames[1] = { { SERVER_SEAT_DISABLED, NULL, 0 } };
            send_frames(connection, frames, 1, -1);
        } else if (opcode == CLIENT_CLOSE_SEAT) {
            closed = 1;
            printf("seat closed\n");
            fflush(stdout);
            struct frame frames[1] = { { SERVER_SEAT_CLOSED, NULL, 0 } };
            send_frames(connection, frames, 1, -1);
        } else if (opcode == CLIENT_CLOSE_DEVICE) {
            struct frame frames[1] = { { SERVER_DEVICE_CLOSED, NULL, 0 } };
            send_frames(connection, frames, 1, -1);
        } else if (opcode == CLIENT_SWITCH_SESSION) {
            struct frame frames[1] = { { SERVER_SESSION_SWITCHED, NULL, 0 } };
            send_frames(connection, frames, 1, -1);
        } else {
            free(payload);
            break;
        }
        free(payload);
    }
    printf("%s\n", opened ? "seat opened" : "seat not opened");
    printf("%s\n", refused ? "device refused" : "device not refused");
    printf("%s\n", handed ? "device handed" : "device not handed");
    printf("%s\n", pinged ? "ping seen" : "ping not seen");
    printf("%s\n", acked ? "disable acked" : "disable not acked");
    printf("%s\n", closed ? "seat closed" : "seat not closed");
    fflush(stdout);
    close(connection);
    close(server);
    return 0;
}
