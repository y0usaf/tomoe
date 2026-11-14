import collections
import re
import sys

MAP = re.compile(r"^([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ \S+\s*(.*)$")
GRAPHICS = re.compile(r"lib(GL|EGL|gbm|glapi|vulkan|drm)|mesa|llvmpipe|swrast|zink|libLLVM|libnvidia|libcuda")
FONTS = ("/share/fonts/", "/fontconfig/")


def category(perm, name, size, binary):
    if not name:
        return "lisp space" if size >= 100 * 1048576 and "x" in perm else "anon"
    base = name.rsplit("/", 1)[-1]
    if base == "[heap]":
        return "malloc heap"
    if name == binary or base == "tomoe":
        return "tomoe image"
    if name.startswith("/dev/shm") or "memfd:" in name or name.startswith("[shm"):
        return "shm"
    if base.endswith((".ttf", ".otf", ".ttc")) or any(part in name for part in FONTS):
        return "fonts"
    if GRAPHICS.search(base):
        return "graphics"
    if ".so" in base:
        return "libraries"
    return "files"


def read(pid):
    rows = []
    rss = collections.Counter()
    pss = collections.Counter()
    mapped = collections.Counter()
    counts = collections.Counter()
    current = None
    with open(f"/proc/{pid}/smaps") as handle:
        for line in handle:
            match = MAP.match(line.rstrip())
            if match:
                start, end, perm, name = match.groups()
                current = category(perm, name, int(end, 16) - int(start, 16), binary)
                mapped[current] += int(end, 16) - int(start, 16)
                counts[current] += 1
                rows.append([current, name or "(anon)", int(end, 16) - int(start, 16), 0])
            elif line.startswith("Rss:"):
                rss[current] += int(line.split()[1])
                rows[-1][3] += int(line.split()[1])
            elif line.startswith("Pss:"):
                pss[current] += int(line.split()[1])
    return rss, pss, mapped, counts, rows


pid = sys.argv[1]
binary = sys.argv[2] if len(sys.argv) > 2 else ""
rss, pss, mapped, counts, rows = read(pid)
status = dict(line.split(":", 1) for line in open(f"/proc/{pid}/status") if ":" in line)
print("category rss_kb pss_kb mapped_kb mappings")
for name in sorted(rss, key=lambda key: -pss[key]):
    print(f"{name} {rss[name]} {pss[name]} {mapped[name] // 1024} {counts[name]}")
print(f"total {sum(rss.values())} {sum(pss.values())} {status['VmSize'].split()[0]} {status['Threads'].strip()}")
for row in sorted(rows, key=lambda item: -item[3])[:12]:
    if row[3] >= 1024:
        print(f"mapping {row[0]} {row[1]} {row[2] // 1024} {row[3]}")
