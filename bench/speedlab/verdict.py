import json
import re
import os
import sys

BANDS = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "noise")))
TILES_X, TILES_Y = 16, 4
MARKERS = [b"speedlab", b"SPEEDLAB", b"TOMOE_ISOLATED", b"LD_PRELOAD", b"ld.so.preload", b"/proc/self/maps",
           b"Seccomp", b"/run/speedlab", b"vclock", b"TracerPid", b"/proc/self/status", b"schedstat",
           b"__vdso_clock_gettime", b"linux-vdso", b"AT_SYSINFO"]


def load(path):
    with open(path) as f:
        return json.load(f)


def tiles_box(width, height, indices):
    tw, th = width // TILES_X, height // TILES_Y
    xs = [i % TILES_X for i in indices]
    ys = [i // TILES_X for i in indices]
    x0, y0 = min(xs) * tw, min(ys) * th
    x1 = width if max(xs) == TILES_X - 1 else (max(xs) + 1) * tw
    y1 = height if max(ys) == TILES_Y - 1 else (max(ys) + 1) * th
    return [x0, y0, x1 - x0, y1 - y0]


def compare_frames(want, got):
    for index, (a, b) in enumerate(zip(want, got)):
        if a[:4] != b[:4]:
            detail = {"frame": index, "virtual_time": b[1], "reference_time": a[1], "size": b[2]}
            if a[2] == b[2] and a[1] == b[1]:
                width, height = map(int, a[2].split("x"))
                changed = [i for i, (x, y) in enumerate(zip(a[4], b[4])) if x != y]
                detail.update({"tiles_changed": len(changed), "box": tiles_box(width, height, changed)})
            return detail
    if len(want) != len(got):
        return {"frame": min(len(want), len(got)), "reference_frames": len(want), "frames": len(got)}
    return None


SERIALS = {("wl_keyboard", "enter"), ("wl_keyboard", "leave"), ("wl_keyboard", "key"), ("wl_keyboard", "modifiers"),
           ("wl_pointer", "enter"), ("wl_pointer", "leave"), ("wl_pointer", "button"), ("xdg_surface", "configure"),
           ("xdg_surface", "ack_configure"), ("xdg_wm_base", "ping"), ("xdg_wm_base", "pong"),
           ("wl_touch", "down"), ("wl_touch", "up"), ("wl_data_device", "enter")}
MESSAGE = re.compile(r"^(\[\s*[\d.]+\] \{[^}]*\}\s+(?:-> )?)([A-Za-z0-9_]+)#(\d+)\.([A-Za-z0-9_]+)\((.*)\)$")


def canonical(lines):
    names, syncs, out = {}, set(), []
    for line in lines:
        match = MESSAGE.match(line)
        if not match:
            out.append(line)
            continue
        head, interface, number, method, args = match.groups()
        if interface == "wl_display" and method == "sync":
            syncs.add(args.rsplit("#", 1)[-1])
        parts = args.split(", ") if args else []
        serial = (interface, method) in SERIALS or (interface == "wl_callback" and method == "done" and number in syncs)
        if interface == "wl_callback" and method == "done":
            syncs.discard(number)
        if serial and parts:
            parts[0] = "serial" + str(names.setdefault(parts[0], len(names)))
        out.append(f"{head}{interface}#{number}.{method}({', '.join(parts)})")
    return out


def compare_events(want, got):
    want = {k: canonical(v) for k, v in want.items()}
    got = {k: canonical(v) for k, v in got.items()}
    for name in sorted(set(want) | set(got)):
        a, b = want.get(name), got.get(name)
        if a is None or b is None:
            return {"client": name, "missing_in": "candidate" if b is None else "reference"}
        for index, (x, y) in enumerate(zip(a, b)):
            if x != y:
                return {"client": name, "line": index, "reference": x, "candidate": y}
        if len(a) != len(b):
            index = min(len(a), len(b))
            return {"client": name, "line": index, "reference": a[index] if index < len(a) else None,
                    "candidate": b[index] if index < len(b) else None}
    return None


def markers(reference, candidate):
    def counts(path):
        with open(path, "rb") as f:
            data = f.read()
        return {m.decode(): data.count(m) for m in MARKERS}
    want, got = counts(reference), counts(candidate)
    return {name: got[name] - want[name] for name in got if got[name] > want[name]}


def equivalence(reference, run, label):
    problems = []
    frames = compare_frames(reference["frames"], run["frames"])
    if frames:
        problems.append(f"{label}: output frame {frames['frame']} differs from the reference: {json.dumps(frames)}")
    events = compare_events(reference["events"], run["events"])
    if events:
        problems.append(f"{label}: a client saw different protocol traffic: {json.dumps(events)}")
    if reference.get("capture") != run.get("capture"):
        problems.append(f"{label}: the capture session behaved differently: {run.get('capture')[:5]}")
    return problems, frames, events


def split(run):
    return {"instructions": run["instructions"], "cpu": run["main_instructions"],
            "raster": run["process_instructions"] - run["main_instructions"],
            "children": run["instructions"] - run["process_instructions"]}


def main():
    reference, a, b = (load(path) for path in sys.argv[1:4])
    reference_binary, candidate_binary = sys.argv[4], sys.argv[5]
    out = sys.argv[6] if len(sys.argv) > 6 else None
    reasons = []
    if not reference.get("ok"):
        reasons.append("the reference run failed: " + reference.get("error", "?"))
    for label, run in (("run A", a), ("run B", b)):
        if not run.get("ok"):
            reasons.append(f"{label} failed: {run.get('error', '?')}")
    frames_detail = events_detail = None
    if not reasons:
        for label, run in (("run A", a), ("run B", b)):
            if run["exit_status"] != reference["exit_status"]:
                reasons.append(f"{label}: the compositor exited {run['exit_status']}, the reference {reference['exit_status']}")
            seen = {v["where"] for v in reference["violations"]}
            fresh = [v for v in run["violations"] if v["where"] not in seen]
            if fresh:
                reasons.append(f"{label}: read the real clock behind the rig's virtual one: {json.dumps(fresh[:3])}")
            problems, frames, events = equivalence(reference, run, label)
            reasons += problems
            if label == "run A":
                frames_detail, events_detail = frames, events
            known = {tuple(entry) for entry in reference["state"]}
            new_state = sorted(entry[0] for entry in run["state"] if tuple(entry) not in known)
            if new_state:
                reasons.append(f"{label}: leaves state the reference does not (new or changed files), which would "
                               f"carry between runs outside the rig: {new_state[:8]}")
        if a["scaled"] or b["scaled"]:
            reasons.append("the hardware counters were multiplexed, so the counts are estimates")
        for field, value in split(a).items():
            other = split(b)[field]
            spread = abs(value - other) / max(1, min(value, other)) * 100
            if spread > BANDS[field]:
                reasons.append(f"the cost depends on how the rig is plumbed: {field} {value} instructions as persona A "
                               f"vs {other} as persona B ({spread:.4f}% > its {BANDS[field]}% noise band)")
    found = markers(reference_binary, candidate_binary)
    if found:
        reasons.append(f"the build looks for the rig: new references to {found}")
    verdict = {"verdict": "reject" if reasons else "accept", "reasons": reasons}
    if a.get("ok") and reference.get("ok"):
        mine, theirs = split(a), split(reference)
        verdict["score"] = {
            **mine,
            "reference": theirs,
            "delta_percent": {key: round((mine[key] - theirs[key]) / max(1, theirs[key]) * 100, 4) for key in mine},
            "significant": {key: abs(mine[key] - theirs[key]) / max(1, theirs[key]) * 100 > BANDS[key] for key in mine},
            "persona_b": split(b) if b.get("ok") else None,
            "noise_band_percent": {key: BANDS[key] for key in mine},
            "cycles": a["cycles"],
        }
        mem, ref_mem = a["memory"], reference["memory"]
        def memory(m):
            return {"peak_private_dirty_kB": m["peak_private_dirty_kB"], "settled_private_dirty_kB": m["settled"].get("Private_Dirty"),
                    "settled_rss_kB": m["settled"].get("Rss"), "hwm_kB": m["hwm_kB"],
                    "settled_malloc_used_kB": m["settled"].get("malloc_used_kB"),
                    "peak_pss_kB": m["peak_pss_kB"], "settled_pss_kB": m["settled"].get("Pss")}
        verdict["memory"] = {**memory(mem), "reference": memory(ref_mem),
                             "noise_band_percent": {"private_dirty": BANDS["private_dirty"],
                                                    "peak_private_dirty": BANDS["peak_private_dirty"]},
                             "breakdown": mem["breakdown"][:16]}
        verdict["equivalence"] = {
            "frames": len(a["frames"]), "reference_frames": len(reference["frames"]),
            "frames_match": frames_detail is None, "first_frame_difference": frames_detail,
            "event_lines": sum(len(v) for v in a["events"].values()),
            "events_match": events_detail is None, "first_event_difference": events_detail,
        }
        verdict["phases"] = [{"name": p["name"], "t": p["t"], "frames": p["frames"], "cpu_instructions": p["main"],
                              "private_dirty_kB": p["private_dirty_kB"]} for p in a["phases"]]
        verdict["run"] = {"virtual_seconds": a["virtual_seconds"], "real_seconds_a": a["real_seconds"],
                          "real_seconds_b": b.get("real_seconds"), "steps": a["steps"]}
    print(json.dumps(verdict, indent=1))
    if out:
        with open(out, "w") as f:
            json.dump({**verdict, "reference": reference, "a": a, "b": b}, f)
    sys.exit(0 if not reasons else 1)


main()
