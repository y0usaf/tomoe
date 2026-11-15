import shlex

KEY_A, KEY_D, KEY_F, KEY_G, KEY_H = "30", "32", "33", "34", "35"
EXTENT = "1280x720"
PRESSES = 6
WORK = "/run/oracle"


def labels():
    return {text(plist(entry)[":ID"]): text(plist(entry)[":APP-ID"])
            for entry in items(inspect()[":WINDOWS"])}


def screen():
    fields = inspect()
    label = labels()
    layout = sorted(
        (label.get(text(plist(entry)[":ID"]), "?"),
         text(plist(entry)[":X"]), text(plist(entry)[":Y"]),
         text(plist(entry)[":WIDTH"]), text(plist(entry)[":HEIGHT"]),
         text(plist(entry)[":VISIBLE"]))
        for entry in items(fields[":LAYOUT"]))
    stacking = [label.get(text(value), "?") for value in items(fields[":STACKING"])]
    focus = label.get(text(fields[":FOCUS"]), "?")
    return f"layout={layout} stacking={stacking} focus={focus} data={show(fields[':DATA'])}"


def extensions():
    return {text(plist(entry)[":NAME"]): plist(entry) for entry in items(inspect()[":EXTENSIONS"])}


def dispatches():
    return {name: int(text(fields[":DISPATCHES"])) for name, fields in extensions().items()}


def oracle():
    fields = plist(inspect()[":ORACLE"])
    return {text(key): text(value) for key, value in fields.items()}


def shake(tag):
    machine.execute(f"systemctl stop inject-{tag} 2>/dev/null")
    machine.execute(f"systemctl reset-failed inject-{tag} 2>/dev/null")


def inject(tag, *script):
    shake(tag)
    machine.succeed(
        f"systemd-run --unit=inject-{tag} --property=Type=exec --remain-after-exit "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
        f"{INJECTOR} --extent {EXTENT} " + " ".join(shlex.quote(item) for item in script))
    machine.wait_until_succeeds(
        f"systemctl show -P SubState inject-{tag} | grep -qx 'exited\\\\|failed'", timeout=120)


def burst(tag, code, presses=PRESSES):
    script = []
    for _ in range(presses):
        script.extend([f"key {code} down", f"key {code} up", "sync", "sleep 60"])
    inject(tag, *script)


def open_windows(tag):
    for name in ("one", "two", "three"):
        client(f"{tag}-{name}", f"--app-id {name}", f"--title {name}", "--size 320x200")


def stop_windows(tag):
    for name in ("one", "two", "three"):
        machine.execute(f"systemctl stop {tag}-{name} 2>/dev/null")
        machine.execute(f"systemctl reset-failed {tag}-{name} 2>/dev/null")


def pixels(tag):
    machine.succeed(f"XDG_RUNTIME_DIR={RUNTIME} WAYLAND_DISPLAY=check grim -t ppm {WORK}/{tag}.ppm")
    return machine.succeed(f"sha256sum {WORK}/{tag}.ppm").split()[0]


machine.wait_for_unit("multi-user.target")
machine.execute(f"rm -rf {WORK} && mkdir -p {WORK}")

start("--bare")
cli(f"mount {ORACLE_POLICY}")
open_windows("oracle")
before = dispatches()
burst("burst", KEY_A)
after = dispatches()
counters = oracle()
assert counters[":ENABLED"] == "T", counters
assert int(counters[":ADOPTED"]) >= PRESSES - 2, (
    "keys with a staged transition were not applied by the native side: " + show(counters))
assert counters[":MISSED"] == "0", counters
assert int(counters[":PREPARED"]) >= PRESSES - 2, (
    "the oracle did not stage a transition for the key it had just seen: " + show(counters))
adopted_screen = screen()
adopted_pixels = pixels("adopted")
print(f"adopted: {counters} wm {before['oracle-wm']} -> {after['oracle-wm']}")
quit()

start("--bare", env={"TOMOE_ORACLE": "0"})
cli(f"mount {ORACLE_POLICY}")
open_windows("oracle")
before = dispatches()
burst("burst-off", KEY_A)
after = dispatches()
counters = oracle()
assert counters[":ENABLED"] == "NIL", counters
assert counters[":ADOPTED"] == "0", counters
assert after["oracle-wm"] - before["oracle-wm"] >= PRESSES - 1, (
    "TOMOE_ORACLE=0 did not keep every key on the Lisp path: "
    f"{before['oracle-wm']} -> {after['oracle-wm']}")
live_pixels = pixels("live")
assert screen() == adopted_screen, (
    "an adopted burst and a live burst disagree:\nadopted " + adopted_screen
    + "\nlive    " + screen())
assert live_pixels == adopted_pixels, (
    f"an adopted burst and a live burst drew different pixels: {adopted_pixels} {live_pixels}")
print("equivalence: adopted and live bursts agree")

cli(f"mount {ORACLE_LIVE_POLICY}")
fields = extensions()
assert text(fields["oracle-wm"][":PRECOMPUTE"]) == "T", show(fields["oracle-wm"])
assert text(fields["oracle-clock"][":SHARED"]) == "T", (
    "reading the clock is not a mutation, so that reducer should still share data: "
    + show(fields["oracle-clock"]))

for name, key in (("oracle-live", KEY_D), ("oracle-clock", KEY_F),
                  ("oracle-global", KEY_G), ("oracle-io", KEY_H)):
    assert text(fields[name][":PRECOMPUTE"]) == "NIL", (
        name + " cannot be evaluated ahead of its event but was accepted: " + show(fields[name]))
    before = dispatches()
    counters = oracle()
    burst(name, key, presses=3)
    after = dispatches()
    assert after[name] - before[name] == 3, (
        name + " did not run live for every key: "
        f"{before[name]} -> {after[name]}")
    assert oracle()[":ADOPTED"] == counters[":ADOPTED"], (
        name + " was applied from a staged transition")

before = dispatches()
counters = oracle()
burst("blocked", KEY_A, presses=4)
after = dispatches()
assert after["oracle-wm"] - before["oracle-wm"] == 4, (
    "a command binding was precomputed while a reducer that cannot be precomputed "
    f"reads every command: {before['oracle-wm']} -> {after['oracle-wm']}")
assert oracle()[":PREPARED"] == counters[":PREPARED"], (
    "the oracle staged a transition that a sibling reducer could have changed")
assert oracle()[":MISSED"] == counters[":MISSED"] == "0", oracle()

stop_windows("oracle")
quit()
