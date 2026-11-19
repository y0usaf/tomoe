#include "../lib/udev/udev_monitor.c"

#include <stdio.h>

static int failures;

static void expect(const char *name, const char *buffer, size_t length, int want, size_t want_offset) {
    char *copy = malloc(length + 1);
    memcpy(copy, buffer, length);
    copy[length] = 0;
    size_t offset = 0;
    int got = datagram_properties(copy, length, &offset);
    free(copy);
    int same = got == want && (got < 0 || offset == want_offset);
    printf("%s %s %d %zu\n", same ? "ok" : "FAIL", name, got, offset);
    if (!same) failures++;
}

static size_t header(char *buffer, uint32_t magic, uint32_t off) {
    struct monitor_header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.prefix, "libudev", 8);
    h.magic = htobe32(magic);
    h.header_size = sizeof(h);
    h.properties_off = off;
    memcpy(buffer, &h, sizeof(h));
    return sizeof(h);
}

int main(void) {
    char buffer[256];
    memset(buffer, 0, sizeof(buffer));
    size_t n = header(buffer, UDEV_MONITOR_MAGIC, sizeof(struct monitor_header));
    memcpy(buffer + n, "ACTION=add\0DEVPATH=/devices/x\0SUBSYSTEM=input\0", 45);
    expect("libudev", buffer, n + 45, 1, n);
    expect("libudev-short-header", buffer, 36, -1, 0);
    expect("libudev-under-32", buffer, 20, -1, 0);
    header(buffer, 0xdeadbeefu, sizeof(struct monitor_header));
    expect("libudev-bad-magic", buffer, n + 45, -1, 0);
    header(buffer, UDEV_MONITOR_MAGIC, n + 45 - 31);
    expect("libudev-offset-past-end", buffer, n + 45, -1, 0);
    header(buffer, UDEV_MONITOR_MAGIC, n + 45 - 32);
    expect("libudev-offset-at-limit", buffer, n + 45, 1, n + 45 - 32);
    header(buffer, UDEV_MONITOR_MAGIC, 0xffffffe8u);
    expect("libudev-offset-wraps", buffer, n + 45, -1, 0);
    header(buffer, UDEV_MONITOR_MAGIC, 0xffffffffu);
    expect("libudev-offset-max", buffer, n + 45, -1, 0);

    static const char kernel[] = "add@/devices/virtual/input/input9\0ACTION=add\0DEVPATH=/devices/virtual/input/input9\0SUBSYSTEM=input\0";
    expect("kernel", kernel, sizeof(kernel) - 1, 0, 34);
    expect("kernel-no-properties", kernel, 34, -1, 0);
    static const char nopath[] = "add-devices-virtual-input-input9-padding-padding\0ACTION=add\0";
    expect("kernel-no-devpath", nopath, sizeof(nopath) - 1, -1, 0);
    char unterminated[40];
    memset(unterminated, 'a', sizeof(unterminated));
    expect("unterminated", unterminated, sizeof(unterminated), -1, 0);
    printf("udev-datagram: %s\n", failures ? "failed" : "every header checked");
    return failures != 0;
}
