import json

start()
state = inspect()
names = [text(plist(entry)[":NAME"]) for entry in items(state[":EXTENSIONS"])]
assert "wm" in names, f"shipped extensions not mounted: {names}"
assert state[":LAST-ERROR"] == "NIL", state[":LAST-ERROR"]

client("window", "--mode xdg --app-id check-window --title check --size 320x200")
version = json.loads(cli("msg version"))
assert version["version"], version
windows = json.loads(cli("msg windows"))
window = next(w for w in windows if w["app_id"] == "check-window")
usable = json.loads(cli("msg outputs"))[0]["usable"]
box = window["geometry"]
assert window["mapped"] and window["focused"], window
assert usable["x"] <= box["x"] and box["x"] + box["w"] <= usable["x"] + usable["w"], (box, usable)
assert usable["y"] <= box["y"] and box["y"] + box["h"] <= usable["y"] + usable["h"], (box, usable)
assert (box["w"], box["h"]) != (320, 200), f"the window kept its own size: {box}"
machine.succeed(f"journalctl -u window -o cat | grep -qx 'configured {box['w']}x{box['h']}'")

cli(f"mount {ZOOMER}")
status, output = machine.execute(ENV + "tomoe --socket check command zoomer zoom-scroll-in")
assert status == 1 and "tomoe event" in output, (status, output)
zoomer = next(plist(e) for e in items(inspect()[":EXTENSIONS"]) if text(plist(e)[":NAME"]) == "zoomer")
assert zoomer[":LAST-ERROR"] == "NIL", zoomer[":LAST-ERROR"]
cli("unmount zoomer")

quit()
