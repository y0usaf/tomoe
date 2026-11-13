import json

start()

listing = json.loads(cli("commands"))
by_name = {command["name"]: command for command in listing}
for name in ("focus-window", "close-window", "switch-workspace", "move-window-to-workspace"):
    assert name in by_name, f"{name} is not a command: {sorted(by_name)}"
    assert by_name[name]["owner"], by_name[name]
    assert by_name[name]["description"], by_name[name]
assert by_name["switch-workspace"]["arguments"] == [
    {"name": "workspace", "type": "workspace", "required": True,
     "description": "Workspace to show.", "maximum": 9}
], by_name["switch-workspace"]
assert by_name["focus-window"]["arguments"][0]["type"] == "window", by_name["focus-window"]

client("first", "--mode xdg --app-id check-first --title first --size 300x200")
client("second", "--mode xdg --app-id check-second --title second --size 300x200")


def windows():
    return {window["app_id"]: window for window in json.loads(cli("msg windows"))}


state = windows()
first, second = state["check-first"]["id"], state["check-second"]["id"]
assert first != second, state
assert state["check-second"]["focused"], state

ran = json.loads(cli(f"command focus-window window={first}"))
assert ran == {"name": "focus-window", "arguments": {"window": first}}, ran
state = windows()
assert state["check-first"]["focused"] and not state["check-second"]["focused"], state

ran = json.loads(cli("""msg command '{"name": "focus-window"}'"""))
assert ran["arguments"] == {"window": first}, ran

ran = json.loads(cli(f"""msg command '{{"name": "move-window-to-workspace", "arguments": {{"window": {first}, "workspace": 3}}}}'"""))
assert ran["arguments"] == {"workspace": 3, "window": first}, ran
state = windows()
assert not state["check-first"]["mapped"] and state["check-second"]["mapped"], state
counts = [workspace["windows"] for workspace in json.loads(cli("msg wm_state"))["workspaces"]]

cli("command switch-workspace workspace=3")
state = windows()
assert state["check-first"]["mapped"] and state["check-first"]["focused"], state
assert not state["check-second"]["mapped"], state
cli("command switch-workspace workspace=1")
state = windows()
assert state["check-second"]["mapped"] and not state["check-first"]["mapped"], state
assert [workspace["windows"] for workspace in json.loads(cli("msg wm_state"))["workspaces"]] == counts, counts

status, output = machine.execute(ENV + "tomoe --socket check command focus-window window=9999")
assert status == 1 and "No window 9999" in output, (status, output)
status, output = machine.execute(ENV + "tomoe --socket check command finish-now")
assert status == 1 and "Unknown command" in output, (status, output)
status, output = machine.execute(ENV + "tomoe --socket check command move-window window=1 x=1")
assert status == 1 and "needs a y argument" in output, (status, output)
status, output = machine.execute(ENV + "tomoe --socket check command switch-workspace workspace=99")
assert status == 1 and output.strip() == '{"error":"No workspace 99."}', (status, output)
status, output = machine.execute(ENV + "tomoe --socket check command move-window-to-workspace workspace=99 window=%d" % first)
assert status == 1 and output.strip() == '{"error":"No workspace 99."}', (status, output)
cli("command switch-workspace workspace=1")
assert windows()["check-second"]["focused"], windows()

ran = json.loads(cli(f"""msg command '{{"name": "close-window", "arguments": {{"window": {first}}}}}'"""))
assert ran == {"name": "close-window", "arguments": {"window": first}}, ran
for _ in range(100):
    if "check-first" not in windows():
        break
    time.sleep(0.1)
assert "check-first" not in windows(), windows()
assert "check-second" in windows(), windows()
quit()
