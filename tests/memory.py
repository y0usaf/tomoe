SHOT = "/run/memory"
machine.succeed(f"rm -rf {SHOT} && cp -r {SOURCE} {SHOT}")
machine.succeed("mkdir -p /run/user/1001/bolo && printf idle > /run/user/1001/bolo/state")

ANON_LIMIT = 60000
DIRTY_LIMIT = 74000
BUDGET_REFRESH_MHZ = 240000
ANIMATED_WINDOW = 6
CONTROL_WINDOW = 4
CHURN_LIMIT = 6 * 1024 * 1024

exec(compile(open(HANDBACK_GATE).read(), "memory-handback", "exec"), globals())


def tomoe_process():
    pids = machine.succeed(
        "cat /sys/fs/cgroup/system.slice/tomoe.service/cgroup.procs 2>/dev/null || true"
    ).split()
    for pid in pids:
        if "tomoe --socket check" in machine.succeed(f"tr '\\0' ' ' < /proc/{pid}/cmdline"):
            return pid
    raise AssertionError("no tomoe process")


def rollup():
    values = {}
    for line in machine.succeed(f"cat /proc/{tomoe_process()}/smaps_rollup").splitlines():
        key, _, rest = line.partition(":")
        if rest:
            values[key] = int(rest.split()[0])
    report = cli("memory").strip()
    values["heap-used"] = int(report.split(":HEAP-USED ")[1].split()[0])
    values["heap-held"] = int(report.split(":HEAP-HELD ")[1].split()[0])
    values["dynamic-usage"] = int(report.split(":DYNAMIC-USAGE ")[1].split()[0])
    values["gc-count"] = int(report.split(":GC-COUNT ")[1].split()[0])
    return values


def measure(label, attribute=False, samples=4, gap=0.4):
    rows = []
    for index in range(samples):
        if index and gap:
            time.sleep(gap)
        rows.append(rollup())
    peak = {key: max(sample[key] for sample in rows) for key in rows[0]}
    typical = {key: sorted(sample[key] for sample in rows)[len(rows) // 2] for key in rows[0]}
    print(
        f"MEASURE {label} pss={peak['Pss']} pss_median={typical['Pss']} pss_anon={peak['Pss_Anon']}"
        f" pss_anon_median={typical['Pss_Anon']} private_dirty={peak['Private_Dirty']}"
        f" pss_file={peak['Pss_File']} shmem={peak['Pss_Shmem']}"
        f" noise_anon={peak['Pss_Anon'] - min(sample['Pss_Anon'] for sample in rows)}"
        f" heap_used={peak['heap-used']} heap_held={peak['heap-held']}"
        f" heap_free={peak['heap-held'] - peak['heap-used']}"
        f" dynamic={peak['dynamic-usage']} gc={peak['gc-count']}",
        flush=True,
    )
    if attribute:
        print(machine.succeed(f"python3 {ATTRIBUTE} {tomoe_process()} {TOMOE_BIN}").strip(),
              flush=True)
    return peak


def shot(name):
    machine.succeed(f"{ENV}WAYLAND_DISPLAY=check grim -t ppm {SHOT}/{name}.ppm")
    return machine.succeed(f"sha256sum {SHOT}/{name}.ppm").split()[0]


def fresh(*options, bus=True, env=None):
    machine.execute("systemctl stop check-bus tomoe; systemctl reset-failed check-bus tomoe")
    start(*options, bus=bus, env=env)


def open_windows(count, first=0, width=320, height=240, prefix="w"):
    for index in range(first, first + count):
        client(f"{prefix}{index}",
               f"--mode xdg --app-id check-{prefix}{index} --size {width}x{height}")


def close_windows(count, first=0, prefix="w"):
    for index in range(first, first + count):
        machine.execute(f"systemctl stop {prefix}{index}")


def refresh_period():
    reply = read(cli("animation"))
    assert reply[1] == ":OK", show(reply)
    fields = plist(reply[2])
    refresh = int(fields.get(":REFRESH-MHZ", "0") or 0)
    period = int(fields.get(":PERIOD-NS", "0") or 0)
    if period <= 0:
        refresh = max([refresh] + [int(plist(output).get(":REFRESH-MHZ", "0") or 0)
                                   for output in items(inspect()[":OUTPUTS"])])
        period = 1000000000000 // (refresh or BUDGET_REFRESH_MHZ)
    return refresh, period


def read_timings(label):
    reply = read(cli("timings"))
    assert reply[1] == ":OK", show(reply)
    timings = plist(reply[2])
    channels = {text(plist(entry)[":NAME"]): plist(entry) for entry in items(timings[":CHANNELS"])}
    over = [(int(entry[0]), int(entry[1])) for entry in items(timings[":STALLS"])]
    intervals = sorted((int(entry[0]), int(entry[1]))
                       for entry in items(timings[":GC-INTERVALS"]))
    handbacks = [(int(entry[0]), int(entry[1])) for entry in items(timings[":HANDBACK-INTERVALS"])]
    longest = max([duration for _, duration in over], default=0)
    handback = max([b - a for a, b in handbacks], default=0)
    print(
        f"STALL {label} frames_over={len(over)} longest_ns={longest}"
        f" collections={len(intervals)} handbacks={len(handbacks)} handback_ns={handback}"
        f" frames={channels.get('frame-total', {}).get(':COUNT', 0)}"
        f" frame_p50={channels.get('frame-total', {}).get(':P50', 0)}"
        f" frame_p99={channels.get('frame-total', {}).get(':P99', 0)}",
        flush=True,
    )
    return longest, intervals, handbacks


def collection_gate(label, intervals, period, window):
    assert intervals, f"{label}: no collection was logged in the measured window"
    pauses = sorted(end - start for start, end in intervals)
    allowed = window // period
    print(
        f"COLLECTION {label} period_ns={period} window_ns={window} count={len(intervals)}"
        f" allowed={allowed} min_us={pauses[0] // 1000}"
        f" median_us={pauses[len(pauses) // 2] // 1000}"
        f" mean_us={sum(pauses) // len(pauses) // 1000} max_us={pauses[-1] // 1000}"
        f" pauses_us={[pause // 1000 for pause in pauses]}",
        flush=True,
    )
    assert len(intervals) <= allowed, (
        f"{label}: {len(intervals)} collections in {window} ns, more than the one per"
        f" {period} ns refresh period the budget allows")
    assert pauses[len(pauses) // 2] <= period, (
        f"{label}: the median collection paused {pauses[len(pauses) // 2]} ns over one refresh"
        f" ({period} ns)")
    return pauses


fresh("--bare", bus=False)
measure("bare")
quit()

fresh("--bare", bus=False)
cli(f"mount {SCENE_POLICY}")
time.sleep(1)
open_windows(1)
time.sleep(2)
tiles_one = measure("tiles-1")
open_windows(9, first=1)
time.sleep(3)
tiles_ten = measure("tiles-10")
tiles = shot("tiles")
time.sleep(1)
assert shot("tiles-again") == tiles, "two frames of one scene rendered differently"
open_windows(40, first=10)
time.sleep(5)
tiles_fifty = measure("tiles-50")
close_windows(50)
time.sleep(2)
measure("tiles-0")
quit()

fresh("--config", CONFIG, env={"TOMOE_TIMINGS": "1"})
time.sleep(3)
state = inspect()
print("config error:", state[":LAST-ERROR"], flush=True)
print("extensions:", state[":EXTENSIONS"], flush=True)
measure("policy-idle", attribute=True)
open_windows(2, width=320, height=240, prefix="p")
time.sleep(4)
policy_windows = measure("policy-2")
cli("timings reset")
time.sleep(ANIMATED_WINDOW)
longest, _, _ = read_timings("policy-2-animated")
settled = measure("policy-2-settled", attribute=True)
close_windows(2, prefix="p")
time.sleep(2)
measure("policy-0")
quit()

machine.succeed("systemctl stop tomoe check-bus")
fresh("--config", CONFIG, env={"TOMOE_TIMINGS": "1"})
cli(f"mount {REFRESH_POLICY}")
cli(f"mount {SCENE_POLICY}")
open_windows(2, width=320, height=240, prefix="c")
time.sleep(3)
refresh, period = refresh_period()
print("refresh:", refresh, "period:", period, flush=True)
assert refresh >= BUDGET_REFRESH_MHZ, (
    f"the measured output runs at {refresh} mHz, below the {BUDGET_REFRESH_MHZ} mHz"
    " refresh the collection budget is stated against")
cli("timings reset")
time.sleep(ANIMATED_WINDOW)
_, collections, _ = read_timings("collections")
close_windows(2, prefix="c")
quit()

machine.succeed("systemctl stop tomoe check-bus")
fresh("--config", CONFIG, env={"TOMOE_TIMINGS": "1",
                              "TOMOE_TEST_GC_PAUSE_US": str(period * 3 // 2 // 1000)})
cli(f"mount {REFRESH_POLICY}")
cli(f"mount {SCENE_POLICY}")
open_windows(2, width=320, height=240, prefix="s")
time.sleep(3)
slow = []
for attempt in range(3):
    cli("timings reset")
    time.sleep(CONTROL_WINDOW)
    _, slow, _ = read_timings(f"forced-slow-gc-{attempt}")
    if slow:
        break
close_windows(2, prefix="s")
quit()
assert slow and min(end - start for start, end in slow) > period, (
    "the test pause floor did not reach the collector: "
    + str(sorted(end - start for start, end in slow)))
try:
    collection_gate("forced-slow-gc", slow, period, CONTROL_WINDOW * 1000000000)
except AssertionError as error:
    print("forced slow collection rejected:", error, flush=True)
else:
    raise AssertionError("a collection slower than one refresh was accepted")

handbacks, handback_period = handback_window("h")
handback_gate("idle-handback", handbacks, handback_period)
slow_handback_window("s", handback_period)

assert len(state[":EXTENSIONS"]) > 1, state[":EXTENSIONS"]
for row, label in ((tiles_one, "tiles-1"), (tiles_ten, "tiles-10"), (tiles_fifty, "tiles-50"),
                   (policy_windows, "policy-2"), (settled, "policy-2-settled")):
    assert row["Pss_Anon"] <= ANON_LIMIT, f"{label} anon {row['Pss_Anon']} KB over {ANON_LIMIT}"
    assert row["Private_Dirty"] <= DIRTY_LIMIT, (
        f"{label} private dirty {row['Private_Dirty']} KB over {DIRTY_LIMIT}")
collection_gate("collections", collections, period, ANIMATED_WINDOW * 1000000000)
print("memory bounds: anon", ANON_LIMIT, "dirty", DIRTY_LIMIT, "period", period,
      "tiles", tiles, "longest_frame", longest, flush=True)
