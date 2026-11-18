SHOT = "/run/memory"
machine.succeed(f"rm -rf {SHOT} && cp -r {SOURCE} {SHOT}")
machine.succeed("mkdir -p /run/user/1001/bolo && printf idle > /run/user/1001/bolo/state")

ANON_LIMIT = 60000
DIRTY_LIMIT = 77600
BUDGET_REFRESH_MHZ = 240000
ANIMATED_WINDOW = 6
CONTROL_WINDOW = 4
CHURN_LIMIT = 3 * 1024 * 1024

exec(compile(open(HANDBACK_GATE).read(), "memory-arena", "exec"), globals())


def tomoe_process():
    pids = machine.succeed(
        "cat /sys/fs/cgroup/system.slice/tomoe.service/cgroup.procs 2>/dev/null || true"
    ).split()
    for pid in pids:
        if "tomoe --socket check" in machine.succeed(f"tr '\\0' ' ' < /proc/{pid}/cmdline"):
            return pid
    raise AssertionError("no tomoe process")


def report_value(report, key):
    return int(report.split(f":{key} ")[1].split()[0].rstrip(")"))


def rollup():
    values = {}
    for line in machine.succeed(f"cat /proc/{tomoe_process()}/smaps_rollup").splitlines():
        key, _, rest = line.partition(":")
        if rest:
            values[key] = int(rest.split()[0])
    report = cli("memory").strip()
    for key in ("RSS-BYTES", "MALLOC-HEAP-USED", "MALLOC-HEAP-HELD", "TEXTURE-BYTES",
                "PERSISTENT-USED", "ARENA-USED", "ARENA-PEAK", "RECLAIM-LONGEST-US",
                "HOT-ALLOCS", "EVENTS-DROPPED"):
        values[key.lower()] = report_value(report, key)
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
        f" rss={peak['rss-bytes']} malloc_used={peak['malloc-heap-used']}"
        f" malloc_held={peak['malloc-heap-held']} gpu_textures={peak['texture-bytes']}"
        f" lisp_persistent={peak['persistent-used']} lisp_arena_peak={peak['arena-peak']}"
        f" hot_allocs={peak['hot-allocs']} events_dropped={peak['events-dropped']}",
        flush=True,
    )
    if attribute:
        print(machine.succeed(f"{ATTRIBUTE} {tomoe_process()} {TOMOE_BIN}").strip(),
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
    longest = max([duration for _, duration in over], default=0)
    print(
        f"STALL {label} frames_over={len(over)} longest_ns={longest}"
        f" frames={channels.get('frame-total', {}).get(':COUNT', 0)}"
        f" frame_p50={channels.get('frame-total', {}).get(':P50', 0)}"
        f" frame_p99={channels.get('frame-total', {}).get(':P99', 0)}",
        flush=True,
    )
    return longest


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

fresh("--config", CONFIG, "--test-timings")
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
longest = read_timings("policy-2-animated")
settled = measure("policy-2-settled", attribute=True)
close_windows(2, prefix="p")
time.sleep(2)
measure("policy-0")
quit()

machine.succeed("systemctl stop tomoe check-bus")
arena = arena_window("a")

assert len(state[":EXTENSIONS"]) > 1, state[":EXTENSIONS"]
for row, label in ((tiles_one, "tiles-1"), (tiles_ten, "tiles-10"), (tiles_fifty, "tiles-50"),
                   (policy_windows, "policy-2"), (settled, "policy-2-settled")):
    assert row["Pss_Anon"] <= ANON_LIMIT, f"{label} anon {row['Pss_Anon']} KB over {ANON_LIMIT}"
    assert row["Private_Dirty"] <= DIRTY_LIMIT, (
        f"{label} private dirty {row['Private_Dirty']} KB over {DIRTY_LIMIT}")
assert settled["hot-allocs"] == 0, f"{settled['hot-allocs']} allocations on the frame or input path"
assert settled["events-dropped"] == 0, f"{settled['events-dropped']} events dropped"
print("memory bounds: anon", ANON_LIMIT, "dirty", DIRTY_LIMIT, "arena", arena,
      "tiles", tiles, "longest_frame", longest, flush=True)
