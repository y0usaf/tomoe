#include <systemd/sd-bus.h>

int tomoe_backlight_set(const char *name, unsigned value) {
    sd_bus *bus = NULL;
    int result = sd_bus_open_system(&bus);
    if (result >= 0)
        result = sd_bus_call_method(bus, "org.freedesktop.login1",
            "/org/freedesktop/login1/session/auto", "org.freedesktop.login1.Session",
            "SetBrightness", NULL, NULL, "ssu", "backlight", name, value);
    sd_bus_flush_close_unref(bus);
    return result < 0 ? result : 0;
}
