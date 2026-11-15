#include <stdarg.h>
#include <syslog.h>

void tomoe_syslog_line(int level, const char *fmt, va_list args) {
    static const int severity[] = { LOG_ERR, LOG_WARNING, LOG_INFO, LOG_DEBUG };
    static int opened;
    if (!opened) {
        openlog("tomoe", LOG_PID, LOG_DAEMON);
        opened = 1;
    }
    vsyslog(severity[level - 1], fmt, args);
}
