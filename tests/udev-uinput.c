#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum kind { KIND_KEYBOARD, KIND_MOUSE, KIND_TOUCHPAD, KIND_TOUCHSCREEN, KIND_JOYSTICK, KIND_TABLET };

static int create(int kind, int index) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "udev-uinput: open: %s\n", strerror(errno));
        return -1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    switch (kind) {
    case KIND_KEYBOARD:
        for (int key = KEY_1; key <= KEY_0; key++) ioctl(fd, UI_SET_KEYBIT, key);
        for (int key = KEY_A; key <= KEY_Z; key++) ioctl(fd, UI_SET_KEYBIT, key);
        ioctl(fd, UI_SET_KEYBIT, KEY_SPACE);
        ioctl(fd, UI_SET_KEYBIT, KEY_ENTER);
        ioctl(fd, UI_SET_KEYBIT, KEY_LEFTSHIFT);
        ioctl(fd, UI_SET_EVBIT, EV_LED);
        ioctl(fd, UI_SET_LEDBIT, LED_CAPSL);
        break;
    case KIND_MOUSE:
        ioctl(fd, UI_SET_EVBIT, EV_REL);
        ioctl(fd, UI_SET_RELBIT, REL_X);
        ioctl(fd, UI_SET_RELBIT, REL_Y);
        ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
        ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
        ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
        ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
        break;
    case KIND_TOUCHPAD:
    case KIND_TOUCHSCREEN:
    case KIND_TABLET: {
        ioctl(fd, UI_SET_EVBIT, EV_ABS);
        ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
        ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
        ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_PEN);
        ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
        struct uinput_abs_setup axes[6] = {
            { .code = ABS_X, .absinfo = { .minimum = 0, .maximum = 4095 } },
            { .code = ABS_Y, .absinfo = { .minimum = 0, .maximum = 4095 } },
            { .code = ABS_PRESSURE, .absinfo = { .minimum = 0, .maximum = 255 } },
            { .code = ABS_MT_SLOT, .absinfo = { .minimum = 0, .maximum = 4 } },
            { .code = ABS_MT_POSITION_X, .absinfo = { .minimum = 0, .maximum = 4095 } },
            { .code = ABS_MT_POSITION_Y, .absinfo = { .minimum = 0, .maximum = 4095 } },
        };
        for (size_t i = 0; i < 6; i++) {
            ioctl(fd, UI_SET_ABSBIT, axes[i].code);
            ioctl(fd, UI_ABS_SETUP, &axes[i]);
        }
        ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
        struct uinput_abs_setup tracking = {
            .code = ABS_MT_TRACKING_ID, .absinfo = { .minimum = -1, .maximum = 65535 }
        };
        ioctl(fd, UI_ABS_SETUP, &tracking);
        if (kind == KIND_TOUCHPAD) {
            ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
            ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD);
        } else if (kind == KIND_TOUCHSCREEN)
            ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
        break;
    }
    case KIND_JOYSTICK: {
        ioctl(fd, UI_SET_KEYBIT, BTN_A);
        ioctl(fd, UI_SET_KEYBIT, BTN_B);
        ioctl(fd, UI_SET_EVBIT, EV_ABS);
        struct uinput_abs_setup axes[4] = {
            { .code = ABS_X, .absinfo = { .minimum = -32768, .maximum = 32767 } },
            { .code = ABS_Y, .absinfo = { .minimum = -32768, .maximum = 32767 } },
            { .code = ABS_HAT0X, .absinfo = { .minimum = -1, .maximum = 1 } },
            { .code = ABS_HAT0Y, .absinfo = { .minimum = -1, .maximum = 1 } },
        };
        for (size_t i = 0; i < 4; i++) {
            ioctl(fd, UI_SET_ABSBIT, axes[i].code);
            ioctl(fd, UI_ABS_SETUP, &axes[i]);
        }
        break;
    }
    }
    static const char *const names[] = {
        "tomoe-test-keyboard", "tomoe-test-mouse", "tomoe-test-touchpad",
        "tomoe-test-touchscreen", "tomoe-test-joystick", "tomoe-test-tablet",
    };
    char name[64];
    snprintf(name, sizeof(name), "%s-%d", names[kind], index);
    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1209;
    setup.id.product = (unsigned short)(0x2000 + kind);
    setup.id.version = 1;
    snprintf(setup.name, sizeof(setup.name), "%s", name);
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "udev-uinput: create %s: %s\n", name, strerror(errno));
        close(fd);
        return -1;
    }
    struct input_event events[] = {
        { .type = EV_KEY, .code = BTN_TOUCH, .value = 1 },
        { .type = EV_KEY, .code = KEY_A, .value = 1 },
        { .type = EV_SYN, .code = SYN_REPORT, .value = 0 },
        { .type = EV_KEY, .code = KEY_A, .value = 0 },
        { .type = EV_SYN, .code = SYN_REPORT, .value = 0 },
    };
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); i++)
        (void)!write(fd, &events[i], sizeof(events[i]));
    return fd;
}

int main(int argc, char **argv) {
    const char *hold = getenv("UDEV_UINPUT_HOLD");
    if (argc < 2) {
        fprintf(stderr, "usage: udev-uinput KIND...\n");
        return 2;
    }
    int fds[16];
    int count = 0;
    for (int i = 1; i < argc && count < 16; i++) {
        int kind;
        if (strcmp(argv[i], "keyboard") == 0) kind = KIND_KEYBOARD;
        else if (strcmp(argv[i], "mouse") == 0) kind = KIND_MOUSE;
        else if (strcmp(argv[i], "touchpad") == 0) kind = KIND_TOUCHPAD;
        else if (strcmp(argv[i], "touchscreen") == 0) kind = KIND_TOUCHSCREEN;
        else if (strcmp(argv[i], "joystick") == 0) kind = KIND_JOYSTICK;
        else if (strcmp(argv[i], "tablet") == 0) kind = KIND_TABLET;
        else {
            fprintf(stderr, "udev-uinput: unknown kind %s\n", argv[i]);
            return 2;
        }
        int fd = create(kind, count);
        if (fd < 0) return 1;
        fds[count++] = fd;
    }
    printf("ready\n");
    fflush(stdout);
    if (hold) {
        for (;;) {
            if (access(hold, F_OK) == 0) break;
            usleep(50000);
        }
    } else
        sleep(2);
    for (int i = 0; i < count; i++) {
        ioctl(fds[i], UI_DEV_DESTROY);
        close(fds[i]);
    }
    printf("removed\n");
    fflush(stdout);
    return 0;
}
