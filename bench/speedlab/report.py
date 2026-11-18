import bisect
import json
import re
import subprocess
import sys

PERIOD = 10_000_000
ROW = re.compile(r"^\s+([\d.]+)%\s+(\d+)\s+(.*)$")
ALLOCATORS = ("libspeedlab-census.so", "libc.so")
WRAPPERS = {"malloc", "calloc", "realloc", "free", "os_malloc", "os_calloc", "align_malloc", "os_malloc_aligned"}


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


def perf_rows(text, columns):
    rows = []
    for line in text.splitlines():
        match = ROW.match(line)
        if not match:
            continue
        share, samples, rest = match.groups()
        fields = rest.split(None, columns - 1)
        if len(fields) < columns:
            continue
        rows.append({"percent": float(share), "instructions": int(samples) * PERIOD, "fields": fields})
    return rows


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
        owner = meaningful[0] if meaningful else ("?", "?")
        tomoe = next((f[1] for f in meaningful if f[0] == binary_name), None)
        owners[owner[0]] = owners.get(owner[0], 0) + live
        key = tomoe or f"(outside tomoe) {owner[0]}"
        functions[key] = functions.get(key, 0) + live
        sites.append({"live_kB": live // 1024, "blocks": count, "stack": [f"{d}:{s}" for d, s in meaningful[:5]]})
    return {"total_kB": total // 1024,
            "by_library_kB": {k: v // 1024 for k, v in sorted(owners.items(), key=lambda kv: -kv[1])},
            "by_tomoe_function_kB": {k: v // 1024 for k, v in sorted(functions.items(), key=lambda kv: -kv[1])[:30]},
            "top_sites": sites[:25]}


def main():
    run = json.load(open(sys.argv[1]))
    binary = sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else None
    if not run.get("ok"):
        print(json.dumps({"ok": False, "error": run.get("error"), "log": run.get("log_tail", [])[-20:]}, indent=1))
        sys.exit(1)
    symbols = Symbols(binary)
    binary_name = binary.rsplit("/", 1)[-1]
    profile = run["profile"]
    threads = [{"thread": r["fields"][0], "dso": r["fields"][1], "percent": r["percent"],
                "instructions": r["instructions"]} for r in perf_rows(profile["threads"], 2)]
    functions = []
    for r in perf_rows(profile["functions"], 3):
        comm, dso, symbol = r["fields"]
        symbol = symbol.replace("[.] ", "").strip()
        functions.append({"thread": comm, "dso": dso, "function": symbol, "percent": r["percent"],
                          "instructions": r["instructions"]})
    main_thread = [f for f in functions if f["thread"] == "tomoe"]
    phases = profile.get("census_phases", {})
    censuses = {}
    for name, text in profile.get("census", {}).items():
        gen = name.rsplit("-", 1)[-1]
        censuses[phases.get(gen, gen)] = census(text, symbols, binary_name)
    report = {
        "ok": True,
        "virtual_seconds": run["virtual_seconds"], "frames": len(run["frames"]), "real_seconds": run["real_seconds"],
        "sample_period_instructions": PERIOD,
        "threads": threads[:20],
        "main_thread_functions": main_thread[:40],
        "all_functions": functions[:60],
        "memory": {"phases": run["phases"], "peak_pss_kB": run["memory"]["peak_pss_kB"],
                   "peak_private_dirty_kB": run["memory"]["peak_private_dirty_kB"],
                   "settled": run["memory"]["settled"], "breakdown": run["memory"]["breakdown"][:25]},
        "heap": censuses,
    }
    print(json.dumps(report, indent=1))
    if out:
        with open(out, "w") as f:
            json.dump(report, f, indent=1)


main()
