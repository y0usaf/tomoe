import difflib
import shlex

WORK = "/run/scenario"
EXTENT = "1280x720"
LEFT, RIGHT, SUPER = "0x110", "0x111", "125"
KEY_A, KEY_J, KEY_F, KEY_F2 = "30", "36", "33", "60"
MID_DRAG = ("drag-mid", "resize-mid", "drag-before-open", "drag-open", "drag-reload",
            "drag-close")


def quote(arguments):
    return " ".join(shlex.quote(argument) for argument in arguments)


def control(binary, arguments):
    return machine.succeed(
        f"XDG_RUNTIME_DIR={RUNTIME} DBUS_SESSION_BUS_ADDRESS={BUS} "
        f"{binary} --socket check {arguments}")


def launch(binary, policy, log):
    machine.execute("rm -rf %s && mkdir -p %s" % (WORK, WORK))
    machine.execute("systemctl stop tomoe 2>/dev/null")
    machine.execute("systemctl reset-failed tomoe 2>/dev/null")
    machine.succeed(
        "systemd-run --unit=tomoe --property=Type=exec --remain-after-exit "
        "--property=RuntimeDirectory=tomoe --property=RuntimeDirectoryMode=0700 "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=DBUS_SESSION_BUS_ADDRESS={BUS} "
        "--setenv=HOME=/root --setenv=PATH=/run/current-system/sw/bin "
        "--setenv=MESA_LOADER_DRIVER_OVERRIDE=zink --setenv=LIBGL_ALWAYS_SOFTWARE=1 "
        f"--setenv=TOMOE_SCENARIO_LOG={log} "
        f"{binary} --socket check --backend headless --no-watch --config {policy}")
    for _ in range(600):
        if machine.execute(
                f"XDG_RUNTIME_DIR={RUNTIME} {binary} --socket check inspect")[0] == 0:
            return
        assert machine.execute("systemctl show -P SubState tomoe | grep -qx running")[0] == 0, (
            "tomoe stopped while starting:\n"
            + machine.succeed("journalctl -u tomoe -o cat | tail -40"))
        time.sleep(0.1)
    raise AssertionError("tomoe did not answer inspect within 60 s")


def shut(binary):
    control(binary, "quit")
    machine.wait_until_succeeds(
        "systemctl show -P SubState tomoe | grep -qx 'exited\\|failed'", timeout=60)
    status = machine.succeed("systemctl show -P ExecMainStatus tomoe").strip()
    assert status == "0", (
        f"tomoe exited {status}: " + machine.succeed("journalctl -u tomoe -o cat | tail -40"))


def dispatch(binary):
    reply = read(control(binary, "inspect"))
    assert reply[1] == ":OK", show(reply)
    fields = plist(reply[2])
    return " ".join(
        f"{text(plist(entry)[':NAME'])}={text(plist(entry)[':DISPATCHES'])}"
        for entry in items(fields[":EXTENSIONS"]))


def state(binary):
    reply = read(control(binary, "inspect"))
    assert reply[1] == ":OK", show(reply)
    fields = plist(reply[2])
    return " ".join(f"{key}={show(fields[key])}" for key in
                    (":LAYOUT", ":FOCUS", ":STACKING", ":WINDOWS", ":OUTPUTS", ":WORKAREAS"))


def pixels(tag):
    machine.succeed(f"XDG_RUNTIME_DIR={RUNTIME} WAYLAND_DISPLAY=check grim -t ppm {WORK}/{tag}.ppm")
    return machine.succeed(f"sha256sum {WORK}/{tag}.ppm").split()[0]


def window(unit, name):
    machine.execute(f"systemctl stop {unit} 2>/dev/null")
    machine.execute(f"systemctl reset-failed {unit} 2>/dev/null")
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
        f"tomoe-test-client --seconds 3600 --app-id {name} --title {name} --size 320x200")
    machine.wait_until_succeeds(f"journalctl -u {unit} -o cat | grep -qx mapped", timeout=60)


def journal(unit):
    return sorted(machine.succeed(f"journalctl -u {unit} -o cat").splitlines())


def scenario(binary, policy, tag, log, transcript):
    launch(binary, policy, log)
    for name in ("one", "two", "three"):
        window(f"w-{name}", name)
    script, plan = [], []

    def act(*commands):
        script.extend(commands)

    def move(x, y):
        act(f"move {x} {y}", "sync", "sleep 60")

    def handshake(name, action=None):
        act(f"signal {WORK}/{tag}-{name}-ready", f"wait {WORK}/{tag}-{name}-go")
        plan.append((name, action))

    def press(button):
        act(f"press {button}", "sync", "sleep 120")

    def release(button):
        act(f"release {button}", "sync", "sleep 160")

    def chord(*codes):
        for code in codes[:-1]:
            act(f"key {code} down")
        act(f"key {codes[-1]} down", f"key {codes[-1]} up")
        for code in reversed(codes[:-1]):
            act(f"key {code} up")
        act("sync", "sleep 160")

    move(200, 300)
    handshake("hover")
    act(*[f"press {LEFT}", f"release {LEFT}", "sync", "sleep 120"])
    handshake("click")
    move(1000, 150)
    handshake("hover-two")
    act(*[f"press {LEFT}", f"release {LEFT}", "sync", "sleep 120"])
    handshake("click-two")
    chord(KEY_A)
    chord(SUPER, KEY_J)
    handshake("keys")
    move(2, 2)
    handshake("corner")
    move(200, 300)
    handshake("corner-left")
    move(2, 2)
    handshake("corner-again")
    move(400, 300)
    act(f"key {SUPER} down", "sync")
    press(LEFT)
    act("move 430 320", "move 470 350", "move 520 380", "sync", "sleep 120")
    handshake("drag-mid")
    act("move 560 400", "move 600 420", "sync", "sleep 120")
    release(LEFT)
    act(f"key {SUPER} up", "sync", "sleep 160")
    handshake("drag-end")
    move(560, 420)
    act(f"key {SUPER} down", "sync")
    press(RIGHT)
    act("move 600 450", "move 640 480", "sync", "sleep 120")
    handshake("resize-mid")
    act("move 660 500", "sync", "sleep 120")
    release(RIGHT)
    act(f"key {SUPER} up", "sync", "sleep 160")
    handshake("resize-end")
    move(200, 300)
    act(f"key {SUPER} down", "sync")
    press(LEFT)
    act("move 240 330", "sync", "sleep 120")
    handshake("drag-before-open")
    act("move 280 360", "sync", "sleep 120")
    handshake("drag-open", lambda: window("w-four", "four"))
    act("move 320 390", "sync", "sleep 120")
    handshake("drag-reload", lambda: control(binary, "reload"))
    act("move 360 420", "sync", "sleep 120")
    handshake("drag-close", lambda: machine.succeed("systemctl stop w-four"))
    act("move 400 450", "sync", "sleep 120")
    release(LEFT)
    act(f"key {SUPER} up", "sync", "sleep 200")
    handshake("drag-after-close")
    move(400, 200)
    act(*[f"press {LEFT}", f"release {LEFT}", "sync", "sleep 120"])
    chord(SUPER, KEY_F)
    handshake("fullscreen")
    chord(SUPER, KEY_F)
    handshake("tiled")
    chord(SUPER, KEY_F2)
    move(640, 500)
    handshake("final")

    machine.succeed(
        f"systemd-run --unit=inject-{tag} --property=Type=exec --remain-after-exit "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
        f"{INJECTOR} --extent {EXTENT} " + quote(script))
    for name, action in plan:
        machine.wait_for_file(f"{WORK}/{tag}-{name}-ready")
        if action:
            action()
        if name in MID_DRAG:
            transcript.append(f"-- checkpoint {name} (mid drag: the screen only, "
                              "the drag position is native until it ends)")
        else:
            transcript.append(f"-- checkpoint {name}")
            transcript.append("state " + state(binary))
        transcript.append("pixels " + pixels(f"{tag}-{name}"))
        transcript.append("runs " + dispatch(binary))
        machine.succeed(f"touch {WORK}/{tag}-{name}-go")
    machine.wait_until_succeeds(
        f"systemctl show -P SubState inject-{tag} | grep -qx 'exited\\|failed'", timeout=120)
    status = machine.succeed(f"systemctl show -P ExecMainStatus inject-{tag}").strip()
    assert status == "0", (
        f"the injector exited {status}: "
        + machine.succeed(f"journalctl -u inject-{tag} -o cat | tail -20"))
    transcript.append("-- clients")
    for name in ("one", "two", "three"):
        transcript.append(f"w-{name} " + str(journal(f"w-{name}")))
    transcript.append("-- final")
    transcript.append("state " + state(binary))
    transcript.append("pixels " + pixels(f"{tag}-final"))
    fields = plist(read(control(binary, "inspect"))[2])
    for entry in items(fields[":EXTENSIONS"]):
        record = plist(entry)
        transcript.append("mount %s dispatches=%s failures=%s state=%s" % (
            text(record[":NAME"]), record[":DISPATCHES"], record[":FAILURES"],
            show(record[":STATE"])))
    transcript.append("-- effects")
    transcript.append(machine.succeed(f"cat {log} 2>/dev/null || true").strip())
    for name in ("one", "two", "three", "four"):
        machine.execute(f"systemctl stop w-{name} 2>/dev/null")
    shut(binary)
    return transcript


def compare(baseline, fastpath, what):
    assert baseline == fastpath, (
        f"{what}: the two builds diverged\n"
        + "\n".join(difflib.unified_diff("\n".join(baseline).splitlines(),
                                        "\n".join(fastpath).splitlines(),
                                        "origin/main", "fastpath", lineterm="")))


machine.wait_for_unit("multi-user.target")
machine.execute("systemctl stop check-bus 2>/dev/null")
machine.execute("systemctl reset-failed check-bus 2>/dev/null")
machine.succeed(
    "systemd-run --unit=check-bus --property=Type=exec "
    f"dbus-daemon --session --nofork --address={BUS}")
machine.wait_for_file("/run/check-bus")

baseline = scenario(BASELINE, POLICY, "baseline", "/run/baseline.log", [])
fastpath = scenario(FASTPATH, POLICY, "fastpath", "/run/fastpath.log", [])
compare(baseline, fastpath, "the shipped desktop")
assert "command mark key" in machine.succeed("cat /run/fastpath.log"), (
    "the bound command did not run on the fast path")
for line in baseline:
    if line.startswith("mount ") and "failures=0 " not in line:
        raise AssertionError(f"an extension failed on origin/main: {line}")
for line in fastpath:
    if line.startswith("mount ") and "failures=0 " not in line:
        raise AssertionError(f"an extension failed on the fast path: {line}")
print("equivalence: the shipped desktop makes the same decisions and draws the same frames")

baseline = scenario(BASELINE, COUNTER_POLICY, "count-baseline", "/run/count-baseline.log", [])
fastpath = scenario(FASTPATH, COUNTER_POLICY, "count-fastpath", "/run/count-fastpath.log", [])
compare(baseline, fastpath, "a reducer without a contract")
counts = [machine.succeed(f"grep -c '^counter' {log}").strip()
          for log in ("/run/count-baseline.log", "/run/count-fastpath.log")]
assert counts[0] == counts[1] and int(counts[0]) > 0, f"the counter saw {counts}"
print(f"equivalence: a reducer without a contract saw all {counts[0]} pointer events, as before")

transcript = scenario(FASTPATH, CONTRACT_POLICY, "contract", "/run/contract.log", [])
wakes = machine.succeed("grep '^contract' /run/contract.log || true").splitlines()
assert wakes, "the contract published no wakes at all"
dispatches = None
for line in transcript:
    if line.startswith("mount scenario-contract "):
        dispatches = int(line.split("dispatches=")[1].split()[0])
assert dispatches is not None, "the contract extension is not mounted"
assert dispatches - 1 == len(wakes), (
    f"the contract holder ran {dispatches - 1} times for {len(wakes)} published wakes")
assert any("corner" in line for line in wakes), wakes
assert any(":top" in line for line in wakes), wakes
assert any("distance=" in line for line in wakes), wakes
print(f"contract: {len(wakes)} wakes, {dispatches - 1} Lisp runs, "
      "the corner, the top edge and the distance threshold all reported")
