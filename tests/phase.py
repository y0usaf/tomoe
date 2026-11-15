KEYS = ("count", "mean", "p50", "p90", "p99", "max")
DRM = {
    "TOMOE_DEBUG": "1",
    "LIBSEAT_BACKEND": "noop",
    "WLR_NO_HARDWARE_CURSORS": "1",
}
PHASES = (("input", "--motion-stream 1000 3000"), ("commit", "--commit-stream 500 3000"))
WARMUP = "--motion-stream 1000 800"
SCENES = (("plain", {}), ("scene", {"TOMOE_PHASE_SHADER": PHASE_SHADER, "TOMOE_PHASE_BLUR": "1"}))
MODES = (("pooled", "--test-pacing-class=0"), ("class", "--test-pacing-class=1"))
MUST_DROP = (("vkms", "plain", "input"),)
CARD = machine.succeed(
    "for c in /sys/class/drm/card*-Virtual-1; do basename $c | cut -d- -f1; done").strip()
CASES = (
    ("headless", 240, ("--bare", "--test-timings", "--test-frame-clock"),
     {"backend": "headless", "bus": True, "env": {}}),
    ("vkms", 60, ("--bare", "--test-timings", f"--drm_device /dev/dri/{CARD}"),
     {"backend": "drm", "bus": False, "env": DRM}),
)
RUNS = int(RUNS)


def frames(socket="check"):
    reply = read(cli("frames", socket))
    assert reply[1] == ":OK", show(reply)
    for entry in items(reply[2]):
        fields = plist(entry)
        if ":NAME" in fields:
            return {key: int(fields.get(":" + key.upper(), "0"))
                    for key in ("missed", "frame", "predicted", "slack")}
    raise AssertionError("no output in frames: " + show(reply))


def channels(socket="check"):
    reply = read(cli("timings", socket))
    assert reply[1] == ":OK", show(reply)
    result = {}
    for entry in items(plist(reply[2])[":CHANNELS"]):
        fields = plist(entry)
        result[text(fields[":NAME"])] = {
            key: int(fields.get(":" + key.upper(), "0")) for key in KEYS
        }
    return result


def output(name, socket="check"):
    for entry in items(inspect(socket)[":OUTPUTS"]):
        fields = plist(entry)
        if text(fields[":NAME"]) == name:
            return fields
    return None


def wait_output(name, socket="check"):
    for _ in range(600):
        fields = output(name, socket)
        if fields and text(fields[":WIDTH"]) != "0":
            return fields
        time.sleep(0.1)
    raise AssertionError(f"no output named {name}")


def settle(limit=8, socket="check"):
    previous = frames(socket)["frame"]
    for _ in range(limit):
        time.sleep(0.25)
        current = frames(socket)["frame"]
        if current == previous:
            return
        previous = current


def measure(socket, arguments):
    cli("timings reset", socket)
    before = frames(socket)
    machine.succeed(ENV + f"WAYLAND_DISPLAY={socket} tomoe-timing-client " + arguments)
    machine.sleep(0.4)
    after = frames(socket)
    data = channels(socket)
    return dict(
        present=data["present-input"], commit=data["present-commit"], latch=data["latch-input"],
        frames=data["frame-total"]["count"], missed=after["missed"] - before["missed"],
        margin=(after["predicted"] + after["slack"]) / 1000,
    )


def spread(runs, phase, channel="present"):
    values = sorted(run[phase][channel]["p50"] for run in runs)
    return values[0] / 1000, values[len(values) // 2] / 1000, values[-1] / 1000


def total(runs, phase, key):
    return sum(run[phase][key] for run in runs)


def margin(runs, phase):
    values = sorted(run[phase]["margin"] for run in runs)
    return values[len(values) // 2]


measured = {}
for backend, refresh, options, settings in CASES:
    for scene, scene_env in SCENES:
        runs = {mode: [] for mode, _ in MODES}
        for run in range(RUNS):
            order = [mode for mode, _ in MODES]
            if run % 2:
                order.reverse()
            for mode in order:
                machine.execute("systemctl stop tomoe; systemctl reset-failed tomoe",
                                check_return=False)
                start(*options, dict(MODES)[mode], backend=settings["backend"],
                      bus=settings["bus"], env=dict(settings["env"], **scene_env))
                cli(f"mount {PHASE_POLICY}")
                wait_output("HEADLESS-1" if backend == "headless" else "Virtual-1")
                assert inspect()[":LAST-ERROR"] == "NIL", inspect()[":LAST-ERROR"]
                settle()
                measure("check", WARMUP)
                runs[mode].append({name: measure("check", arguments) for name, arguments in PHASES})
                quit()
        measured[(backend, scene)] = runs
        for name, _ in PHASES:
            for mode, _ in MODES:
                low, median, high = spread(runs[mode], name)
                print(f"{backend} {refresh} Hz {scene} {name} {mode}: present-input p50 "
                      f"{low:.2f}..{median:.2f}..{high:.2f} ms, commit p50 "
                      f"{spread(runs[mode], name, 'commit')[1]:.2f} ms, latch "
                      f"{spread(runs[mode], name, 'latch')[1]:.2f} ms, margin "
                      f"{margin(runs[mode], name):.2f} us, missed "
                      f"{total(runs[mode], name, 'missed')} of {total(runs[mode], name, 'frames')}")

for backend, refresh, options, settings in CASES:
    for scene, _ in SCENES:
        runs = measured[(backend, scene)]
        for name, _ in PHASES:
            pooled, split = spread(runs["pooled"], name)[1], spread(runs["class"], name)[1]
            print(f"{backend} {refresh} Hz {scene} {name}: pooled {pooled:.3f} ms, "
                  f"class {split:.3f} ms, {100 * (pooled - split) / pooled:+.1f}%")
            assert split <= pooled * 1.02, (
                f"{backend} {scene} {name}: the class split was slower: "
                f"{split:.3f} versus {pooled:.3f} ms")
            if (backend, scene, name) in MUST_DROP:
                assert split * 100 <= pooled * 95, (
                    f"{backend} {scene} {name}: the class split dropped only "
                    f"{100 * (pooled - split) / pooled:.1f}%")
        missed = {mode: sum(total(runs[mode], name, "missed") for name, _ in PHASES)
                  for mode, _ in MODES}
        frames = {mode: sum(total(runs[mode], name, "frames") for name, _ in PHASES)
                  for mode, _ in MODES}
        print(f"{backend} {refresh} Hz {scene}: missed pooled {missed['pooled']} of "
              f"{frames['pooled']} frames, class {missed['class']} of {frames['class']} frames, "
              f"margin pooled {margin(runs['pooled'], 'input'):.2f} us class "
              f"{margin(runs['class'], 'input'):.2f} us")
        assert missed["class"] <= missed["pooled"] + frames["pooled"] // 50, (
            f"{backend} {scene}: the class split missed more frames: "
            f"{missed['class']} versus {missed['pooled']}")
        assert missed["class"] * 20 <= frames["class"], (
            f"{backend} {scene}: the class split lost {missed['class']} of "
            f"{frames['class']} frames")
