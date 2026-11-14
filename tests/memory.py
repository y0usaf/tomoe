SHOT = "/run/memory"
machine.succeed(f"rm -rf {SHOT} && cp -r {SOURCE} {SHOT}")
machine.succeed("mkdir -p /run/user/1001/bolo && printf idle > /run/user/1001/bolo/state")

ANON_LIMIT = 60000
DIRTY_LIMIT = 74000
COLLECTION_LIMIT = 4170000


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


def read_timings(label):
    reply = read(cli("timings"))
    assert reply[1] == ":OK", show(reply)
    timings = plist(reply[2])
    channels = {text(plist(entry)[":NAME"]): plist(entry) for entry in items(timings[":CHANNELS"])}
    over = [(int(entry[0]), int(entry[1])) for entry in items(timings[":STALLS"])]
    intervals = [(int(entry[0]), int(entry[1])) for entry in items(timings[":GC-INTERVALS"])]
    handbacks = [(int(entry[0]), int(entry[1])) for entry in items(timings[":HANDBACK-INTERVALS"])]
    longest = max([duration for _, duration in over], default=0)
    collection = max([b - a for a, b in intervals], default=0)
    handback = max([b - a for a, b in handbacks], default=0)
    print(
        f"STALL {label} frames_over={len(over)} longest_ns={longest}"
        f" collections={len(intervals)} collection_ns={collection}"
        f" handbacks={len(handbacks)} handback_ns={handback}"
        f" frames={channels.get('frame-total', {}).get(':COUNT', 0)}"
        f" frame_p50={channels.get('frame-total', {}).get(':P50', 0)}"
        f" frame_p99={channels.get('frame-total', {}).get(':P99', 0)}",
        flush=True,
    )
    return longest, collection, handback


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
time.sleep(20)
longest, collection, handback = read_timings("policy-2-animated")
settled = measure("policy-2-settled", attribute=True)
close_windows(2, prefix="p")
time.sleep(2)
measure("policy-0")
quit()

assert len(state[":EXTENSIONS"]) > 1, state[":EXTENSIONS"]
for row, label in ((tiles_one, "tiles-1"), (tiles_ten, "tiles-10"), (tiles_fifty, "tiles-50"),
                   (policy_windows, "policy-2"), (settled, "policy-2-settled")):
    assert row["Pss_Anon"] <= ANON_LIMIT, f"{label} anon {row['Pss_Anon']} KB over {ANON_LIMIT}"
    assert row["Private_Dirty"] <= DIRTY_LIMIT, (
        f"{label} private dirty {row['Private_Dirty']} KB over {DIRTY_LIMIT}")
pause = max(collection, handback)
assert pause <= COLLECTION_LIMIT, f"collection {pause} ns over {COLLECTION_LIMIT}"
print("memory bounds: anon", ANON_LIMIT, "dirty", DIRTY_LIMIT, "collection", COLLECTION_LIMIT,
      "tiles", tiles, "longest_frame", longest, "idle_handback", handback, flush=True)
