#include "bus.h"

int tomoe_backlight_set(const char *name, unsigned value) {
    struct tomoe_bus *bus = NULL;
    int result = tomoe_bus_open_system(&bus, 2000000ULL);
    if (result >= 0)
        result = tomoe_bus_call(bus, "org.freedesktop.login1",
            "/org/freedesktop/login1/session/auto", "org.freedesktop.login1.Session",
            "SetBrightness", "ssu", "backlight", name, value);
    tomoe_bus_flush_close_unref(bus);
    return result < 0 ? result : 0;
}
