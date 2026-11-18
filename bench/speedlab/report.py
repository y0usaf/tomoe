import bisect
import json
import re
import subprocess
import sys

PERIOD = 10_000_000
ROW = re.compile(r"^\s+([\d.]+)%\s+(\d+)\s+(.*)$")
ALLOCATORS = ("libspeedlab-census.so", "libc.so.6")
WRAPPERS = {"malloc", "calloc", "realloc", "free", "posix_memalign", "aligned_alloc"}


class Symbols:
    def __init__(self, binary):
        out = subprocess.run(["nm", "-n", "--defined-only", binary], capture_output=True, text=True).stdout
        self.starts, self.names = [], []
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "tTwW":
                self.starts.append(int(parts[0], 16))
                self.names.append(parts[2])

    def name(self, offset):
        index = bisect.bisect_right(self.starts, offset) - 1
        return self.names[index] if index >= 0 else f"0x{offset:x}"


def thread_class(thread, server):
    tid, _, comm = thread.partition(":")
    if comm.startswith("llvmpipe"):
        return "raster (llvmpipe threads)"
    if tid.strip() == str(server):
        return "compositor main thread"
    if comm == "tomoe":
        return "other tomoe threads and processes (shader compiler copies, workers)"
    return f"other: {comm}"


def perf_rows(text, columns):
    rows = []
    for line in text.splitlines():
        match = ROW.match(line)
        if not match:
            continue
        _, samples, rest = match.groups()
        fields = re.split(r"\s{2,}", rest.strip(), maxsplit=columns - 1)
        if len(fields) < columns:
            continue
        rows.append((int(samples) * PERIOD, [f.strip() for f in fields]))
    return rows


def ranked(table, total, limit):
    return [{"what": key, "instructions": value, "percent": round(value / total * 100, 3)}
            for key, value in sorted(table.items(), key=lambda kv: -kv[1])[:limit]]


def cost(perf):
    server = perf["server"]
    rows = perf_rows(perf["threads"], 2)
    total = sum(value for value, _ in rows)
    classes, dsos = {}, {}
    for value, (comm, dso) in rows:
        group = thread_class(comm, server)
        classes[group] = classes.get(group, 0) + value
        if group == "compositor main thread":
            dsos[dso] = dsos.get(dso, 0) + value
    functions, raster = {}, {}
    for value, (comm, dso, symbol) in perf_rows(perf["functions"], 3):
        symbol = symbol.replace("[.] ", "").strip()
        if thread_class(comm, server) == "raster (llvmpipe threads)":
            key = f"{dso}: {symbol}"
            raster[key] = raster.get(key, 0) + value
    for value, (comm, dso, symbol) in perf_rows(perf["main"], 3):
        key = f"{dso}: {symbol.replace('[.] ', '').strip()}"
        functions[key] = functions.get(key, 0) + value
    main = classes.get("compositor main thread", 1)
    inclusive = []
    for line in perf.get("inclusive", "").splitlines():
        match = re.match(r"^\s+([\d.]+)%\s+([\d.]+)%\s+(\d+)\s+(\S+)\s+\[.\]\s+(.*)$", line)
        if match:
            self_share, children, _, dso, symbol = match.groups()
            inclusive.append({"function": f"{dso}: {symbol.strip()}", "inclusive_percent": float(children),
                              "self_percent": float(self_share),
                              "inclusive_instructions": int(float(children) / 100 * classes.get("compositor main thread", 0))})
    inclusive.sort(key=lambda row: -row["inclusive_percent"])
    return {"sampled_instructions": total, "by_thread": ranked(classes, total, 12),
            "main_thread_inclusive": inclusive[:120] or perf.get("inclusive", "")[:3000],
            "main_thread_by_library": ranked(dsos, main, 25),
            "main_thread_functions": ranked(functions, main, 80),
            "main_thread_tomoe_functions": ranked({k: v for k, v in functions.items() if k.startswith("tomoe:")},
                                                  main, 40),
            "raster_functions": ranked(raster, max(1, total - main), 15)}


def counted(run):
    return {"instructions": run["instructions"], "cpu": run["main_instructions"],
            "raster": run["process_instructions"] - run["main_instructions"],
            "children": run["instructions"] - run["process_instructions"],
            "settled_private_dirty_kB": run["memory"]["settled"]["Private_Dirty"],
            "peak_private_dirty_kB": run["memory"]["peak_private_dirty_kB"]}


def frame_name(frame, symbols, binary_name):
    match = re.match(r"(.+?)\+0x([0-9a-f]+)(?:\((.*)\))?$", frame)
    if not match:
        return frame, frame
    dso, offset, symbol = match.group(1), int(match.group(2), 16), match.group(3)
    if dso == binary_name:
        return dso, symbols.name(offset)
    return dso, symbol or f"+0x{offset:x}"


def census(text, symbols, binary_name):
    total, owners, functions, sites = 0, {}, {}, []
    for line in text.splitlines():
        parts = line.split()
        if parts and parts[0] == "total":
            total = int(parts[1])
        if not parts or parts[0] != "site":
            continue
        live, count = int(parts[1]), int(parts[2])
        frames = [frame_name(f, symbols, binary_name) for f in parts[4:]]
        meaningful = [f for f in frames if f[0] not in ALLOCATORS and f[1] not in WRAPPERS]
        owner = meaningful[0][0] if meaningful else "?"
        caller = next((f[1] for f in meaningful if f[0] == binary_name), None)
        owners[owner] = owners.get(owner, 0) + live
        key = caller or f"(no Tomoe frame) {owner}"
        functions[key] = functions.get(key, 0) + live
        sites.append({"live_kB": live // 1024, "blocks": count, "stack": [f"{d}:{s}" for d, s in meaningful[:6]]})
    return {"live_kB": total // 1024,
            "by_allocating_library_kB": {k: v // 1024 for k, v in sorted(owners.items(), key=lambda kv: -kv[1])},
            "by_tomoe_caller_kB": {k: v // 1024 for k, v in sorted(functions.items(), key=lambda kv: -kv[1])[:30]},
            "top_sites": sites[:20]}


def main():
    perf, heap, plain, bare = (json.load(open(path)) for path in sys.argv[1:5])
    binary = sys.argv[5]
    out = sys.argv[6] if len(sys.argv) > 6 else None
    failed = [run for run in (perf, heap, plain, bare) if not run.get("ok")]
    if failed:
        print(json.dumps({"ok": False, "errors": [r.get("error") for r in failed],
                          "log": [r.get("log_tail", [])[-20:] for r in failed]}, indent=1))
        sys.exit(1)
    symbols = Symbols(binary)
    binary_name = binary.rsplit("/", 1)[-1]
    phases = heap["profile"].get("census_phases", {})
    censuses = {}
    for name, text in heap["profile"].get("census", {}).items():
        gen = name.rsplit("-", 1)[-1]
        censuses[phases.get(gen, gen)] = census(text, symbols, binary_name)
    report = {
        "ok": True,
        "workload": {"virtual_seconds": perf["virtual_seconds"], "frames": len(perf["frames"]),
                     "real_seconds": perf["real_seconds"]},
        "sample_period_instructions": PERIOD,
        "counted": {name: counted(run) for name, run in (("with_capture", plain), ("without_capture", bare))},
        "cost": cost(perf["profile"]),
        "memory": {"phases": heap["phases"], "peak_pss_kB": heap["memory"]["peak_pss_kB"],
                   "peak_private_dirty_kB": heap["memory"]["peak_private_dirty_kB"],
                   "settled": heap["memory"]["settled"], "mappings": heap["memory"]["breakdown"][:25]},
        "heap": censuses,
    }
    print(json.dumps(report, indent=1))
    if out:
        with open(out, "w") as f:
            json.dump(report, f, indent=1)


main()
