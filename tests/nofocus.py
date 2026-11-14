import json
import time

start("--bare", env={"WAYLAND_DISPLAY": "check"})
cli(f"mount {DECK}")
cli(f"mount {PROBE}")

listing = {command["name"]: command for command in json.loads(cli("commands"))}
assert "spawn" in listing, sorted(listing)
spawn = listing["spawn"]
assert spawn["owner"] == "session", spawn
assert [argument["name"] for argument in spawn["arguments"]] == ["argv", "focus"], spawn
assert spawn["arguments"][1]["type"] == "boolean", spawn
assert spawn["arguments"][1]["default"] is True, spawn


def windows():
    return json.loads(cli("msg windows"))


def find(app_id):
    return next((window for window in windows() if window["app_id"] == app_id), None)


def focused_app():
    return next((window["app_id"] for window in windows() if window["focused"]), None)


def wait_app(app_id):
    for _ in range(300):
        found = find(app_id)
        if found:
            return found
        time.sleep(0.1)
    raise AssertionError(f"{app_id} did not map: {windows()}")


def wait_gone(app_id):
    for _ in range(300):
        if find(app_id) is None:
            return
        time.sleep(0.1)
    raise AssertionError(f"{app_id} stayed mapped: {windows()}")


def wait_focus(app_id):
    for _ in range(100):
        if focused_app() == app_id:
            return
        time.sleep(0.1)
    raise AssertionError(f"{app_id} is not focused: {windows()}")


def pixel(x, y):
    values = machine.succeed(
        ENV + f"WAYLAND_DISPLAY=check grim -t ppm -g '{x},{y} 1x1' - | od -An -v -tu1"
    ).split()
    return tuple(int(value) for value in values[-3:])


def sample(point, color):
    for _ in range(100):
        if pixel(*point) == color:
            return
        time.sleep(0.1)
    raise AssertionError(f"{point} reads {pixel(*point)}, not {color}: {windows()}")


def unpainted(point, color):
    for _ in range(100):
        if pixel(*point) != color:
            return
        time.sleep(0.1)
    raise AssertionError(f"{point} still reads {color}: {windows()}")


def spawn_command(argv, focus=None):
    arguments = "argv='" + json.dumps(argv, separators=(",", ":")) + "'"
    if focus is not None:
        arguments += " focus=" + ("true" if focus else "false")
    return "command spawn " + arguments


def client_argv(app_id, title, color):
    return ["tomoe-test-client", "--mode", "xdg", "--app-id", app_id, "--title", title,
            "--color", color, "--activate", "--seconds", "3600"]


def journal_lines(pattern):
    return machine.succeed("journalctl -u tomoe -o cat | grep -c '" + pattern + "' || true").strip()


client("holder", "--mode xdg", "--app-id check-holder --title holder --color ff0000 --seconds 3600")
wait_focus("check-holder")
screen = json.loads(cli("msg outputs"))[0]["geometry"]
holder = find("check-holder")["id"]
card = (screen["x"] + 100, screen["y"] + screen["h"] // 2)
middle = (screen["x"] + screen["w"] // 2, screen["y"] + screen["h"] // 2)
edge = (screen["x"] + screen["w"] - 40, screen["y"] + screen["h"] // 2)
sample(card, (255, 0, 0))

ran = json.loads(cli(spawn_command(client_argv("check-background", "background", "00ff00"), focus=False)))
assert ran["arguments"]["focus"] is False, ran
wait_app("check-background")
assert focused_app() == "check-holder", windows()
assert find("check-background")["mapped"], windows()
sample(card, (255, 0, 0))

tree = ("tomoe-test-client --mode xdg --app-id check-tree --title tree --color ffff00 "
        "--activate --seconds 3600 & sleep 3600")
ran = json.loads(cli(spawn_command(["sh", "-c", tree], focus=False)))
assert ran["arguments"]["focus"] is False, ran
wait_app("check-tree")
assert focused_app() == "check-holder", windows()
sample(card, (255, 0, 0))

cli("event '(:type :key :owner \"nofocus-probe\" :command \"quiet-spawn\" :state :pressed)'")
wait_app("check-quiet")
assert focused_app() == "check-holder", windows()
sample(card, (255, 0, 0))

for app_id in ("check-background", "check-tree", "check-quiet"):
    cli(f"command close-window window={find(app_id)['id']}")
    wait_gone(app_id)
wait_focus("check-holder")

cli(f"command toggle-fullscreen window={holder}")
sample(card, (255, 0, 0))
sample(middle, (255, 0, 0))
sample(edge, (255, 0, 0))

ran = json.loads(cli(spawn_command(client_argv("check-cli", "cli", "ff00ff"))))
assert ran["arguments"]["focus"] is True, ran
wait_app("check-cli")
assert focused_app() == "check-holder", windows()
sample(edge, (255, 0, 0))

cli("event '(:type :key :owner \"nofocus-probe\" :command \"focused-spawn\" :state :pressed)'")
wait_app("check-input")
wait_focus("check-input")

assert journal_lines("^activation-token none check-background$") != "0"
assert journal_lines("^activation-token none check-tree$") != "0"
assert journal_lines("^activation-token none check-quiet$") != "0"
assert journal_lines("^activation-token none check-cli$") != "0"
assert journal_lines("^activation-token [0-9a-f]* check-input$") != "0"

cli(f"command toggle-fullscreen window={holder}")
unpainted(edge, (255, 0, 0))

cli(spawn_command(client_argv("check-normal", "normal", "336699")))
wait_app("check-normal")
wait_focus("check-normal")
quit()
