import os
import sys

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
sender = bus.get_unique_name()[1:].replace(".", "_")
portal = bus.get_object("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop")
screencast = dbus.Interface(portal, "org.freedesktop.portal.ScreenCast")
loop = GLib.MainLoop()
count = [0]


def log(*args):
    print(*args, flush=True)


def request(method, *args):
    count[0] += 1
    token = f"t{count[0]}"
    path = f"/org/freedesktop/portal/desktop/request/{sender}/{token}"
    result = {}

    def on_response(code, results):
        result["r"] = (int(code), results)
        loop.quit()

    match = bus.add_signal_receiver(on_response, "Response", "org.freedesktop.portal.Request", path=path)
    args[-1]["handle_token"] = token
    getattr(screencast, method)(*args)
    loop.run()
    match.remove()
    return result["r"]


code, results = request("CreateSession", {"session_handle_token": dbus.String("s1")})
session = results["session_handle"]
log("create", code, session)
bus.add_signal_receiver(
    lambda *a: (log("CLOSED"), loop.quit()), "Closed", "org.freedesktop.portal.Session", path=session
)
types = int(sys.argv[1]) if len(sys.argv) > 1 else 1
code, results = request("SelectSources", dbus.ObjectPath(session), {"types": dbus.UInt32(types)})
log("select", code)
code, results = request("Start", dbus.ObjectPath(session), "", {})
log("start", code, [(int(s[0]), dict(s[1])) for s in results.get("streams", [])])
if os.environ.get("CLOSE"):
    bus.get_object("org.freedesktop.portal.Desktop", session).Close(
        dbus_interface="org.freedesktop.portal.Session"
    )
    log("closed-by-app")
loop.run()
