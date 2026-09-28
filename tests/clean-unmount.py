SKIP = {("commands", "terminal"), ("commands", "launcher"), ("commands", "close"), ("commands", "exit-confirm")}
COUNTERS = {":GENERATION", ":RASTERIZATIONS", ":ASSET-LOADS"}


def files():
    return set(machine.succeed(f"(find {RUNTIME} /tmp/.X11-unix -mindepth 1; ls -d /tmp/.X*-lock) 2>/dev/null || true").split())


def snapshot():
    state = inspect()
    ui = plist(state.pop(":NATIVE-UI"))
    flat = {k: show(v) for k, v in state.items() if k not in COUNTERS}
    flat.update({":NATIVE-UI " + k: v for k, v in ui.items() if k not in COUNTERS})
    return flat, processes(), files()


def differences(before, after):
    (state, pids, names), (state2, pids2, names2) = before, after
    found = [f"{k}: {state.get(k)} -> {state2.get(k)}" for k in sorted(set(state) | set(state2)) if state.get(k) != state2.get(k)]
    found += [f"process still running: {command}" for pid, command in pids2.items() if pid not in pids]
    found += [f"file left behind: {name}" for name in sorted(names2 - names)]
    found += [f"file gone: {name}" for name in sorted(names - names2)]
    return found


def press(owner, command, pointer, state):
    if not pointer:
        return machine.execute(ENV + f"tomoe --socket check command {owner} {command}")
    window = plist(items(inspect()[":WINDOWS"])[0])[":ID"]
    event = (f'(:type :key :owner "{owner}" :command "{command}" :state {state} :button :left '
             f':window {window} :delta 1 :x 200 :y 200 :sx 200 :sy 200)')
    return machine.execute(ENV + f"tomoe --socket check event '{event}'")


def exercise(names):
    for _ in range(2):
        for binding in map(plist, items(inspect()[":BINDINGS"])):
            owner = text(binding.get(":OWNER", "NIL"))
            pointer = any(text(v).startswith(("button-", "scroll-")) for v in binding.values() if isinstance(v, str))
            for key, state in ((":COMMAND", ":pressed"), (":RELEASE", ":released")):
                command = text(binding.get(key, "NIL"))
                if owner in names and command != "NIL" and (owner, command) not in SKIP:
                    status, output = press(owner, command, pointer, state)
                    print(f"{'event' if pointer else 'command'} {owner} {command}: {status} {output.strip()}")
    if "xwayland" in names:
        machine.succeed("DISPLAY=:0 timeout 60 xdpyinfo > /dev/null")
    machine.succeed(ENV + "notify-send check-summary check-body")
    client("transient", "--mode xdg --app-id check-transient --size 200x150")
    machine.sleep(3)
    machine.succeed("systemctl stop transient")


def cycle(path):
    before = snapshot()
    cli(f"mount {path}")
    names = [text(e[":NAME"]) for e in map(plist, items(inspect()[":EXTENSIONS"])) if text(e[":SOURCE"]) == path]
    assert names, f"{path} mounted no extension"
    print(f"cycle {path}: {names}")
    exercise(names)
    errors = [(text(e[":NAME"]), e[":LAST-ERROR"]) for e in map(plist, items(inspect()[":EXTENSIONS"])) if e[":LAST-ERROR"] != "NIL"]
    for name in reversed(names):
        cli(f"unmount {name}")
    deadline = time.monotonic() + 15
    found = differences(before, snapshot())
    while found and time.monotonic() < deadline:
        time.sleep(0.5)
        found = differences(before, snapshot())
    return [f"{path}: {line}" for line in found] + [f"{path}: {name} failed: {error}" for name, error in errors]


start("--bare")
cli(f"mount {WALLPAPER_SETTINGS}")
client("a", "--mode xdg --app-id check-a")
client("b", "--mode xdg --app-id check-b")
client("layer", "--mode layer --namespace check-layer --layer top --anchor top,left,right --size 200x30 --exclusive-zone 30")
failures = []
for path in SOURCES:
    failures += cycle(path)
print("\n".join(failures))
assert not failures, f"{len(failures)} effects outlived their unmount"
quit()
