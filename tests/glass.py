DELAY_MS = 120
ICONS = "/run/glass-icons"
CURSOR = {"XCURSOR_PATH": ICONS, "XCURSOR_THEME": "glass", "XCURSOR_SIZE": "32",
          "TOMOE_DEBUG": "1"}
SIZE = 32
OLD, NEW = (0x22, 0x66, 0xAA), (0xAA, 0x66, 0x22)
POLICY = f"""(in-package #:tomoe-user)
(define-extension "glass-test" (:reads (:outputs :windows) :state nil) (snapshot state event)
  (declare (ignore state event))
  (let ((output (first (context snapshot :outputs))))
    (values nil
            (append (list (settings :cursor-glass (list :enabled t :delay-ms {DELAY_MS})))
                    (when (and output (equal (getf output :name) "HEADLESS-1"))
                      (list (configure-output (getf output :name)
                                              :mode (list 1280 720 60) :scale 1
                                              :position '(0 0))))
                    (when output
                      (loop for window in (context snapshot :windows)
                            when (equal (getf window :app-id) "scanout-client")
                              append (list (fullscreen (getf window :id) t)
                                           (place (getf window :id) (getf output :x) (getf output :y)
                                                  (getf output :width) (getf output :height))))))
            nil)))
"""


MAKE = r"""
import os, struct, sys

def cursor(tint, alpha, size=32):
    out = []
    for y in range(size):
        for x in range(size):
            body = 2 <= x <= 2 + (y - 2) * 0.7 and 2 <= y <= 26
            shadow = 4 <= x <= 4 + (y - 5) * 0.7 and 5 <= y <= 29
            if body:
                r, g, b = (c * alpha // 255 for c in tint)
                out.append(alpha << 24 | r << 16 | g << 8 | b)
            else:
                out.append(0x50 << 24 if shadow else 0)
    head = struct.pack("<4I3I9I", 0x72756358, 16, 0x10000, 1, 0xfffd0002, size, 28,
                       36, 0xfffd0002, size, 1, size, size, 2, 2, 0)
    return head + struct.pack("<%dI" % (size * size), *out)

icons = sys.argv[1]
for variant, tint in (("glass-dark", (0xDC, 0xE2, 0xFF)), ("glass-light", (0x3A, 0x3E, 0x58))):
    os.makedirs(f"{icons}/{variant}/cursors", exist_ok=True)
    with open(f"{icons}/{variant}/cursors/default", "wb") as handle:
        handle.write(cursor(tint, 0x60))
    os.symlink("default", f"{icons}/{variant}/cursors/left_ptr")
rows = bytearray()
for y in range(720):
    for x in range(1280):
        rows += bytes((0xF0, 0xF0, 0xF0) if (x // 3) % 2 else (0x10, 0x10, 0x30))
with open("/run/stripes.ppm", "wb") as handle:
    handle.write(b"P6\n1280 720\n255\n" + bytes(rows))
"""


def relink(variant):
    machine.succeed(f"ln -sfn {variant} {ICONS}/.glass && mv -T {ICONS}/.glass {ICONS}/glass")


def output_frames(name):
    reply = read(cli("frames"))
    assert reply[1] == ":OK", show(reply)
    for entry in items(reply[2]):
        entry = plist(entry)
        if ":NAME" in entry and text(entry[":NAME"]) == name:
            return entry
    raise AssertionError(f"{name} is missing from frames")


def flag(entry, key):
    return text(entry[key]).lower() == "t"


def point(x, y):
    machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-pointer-script --extent 1280x720 "
                    f"'move {x - 6} {y}' 'sleep 10' 'move {x} {y}'")


def wait_glass(name, want, timeout=3.0):
    deadline = time.monotonic() + timeout
    entry = output_frames(name)
    while time.monotonic() < deadline:
        entry = output_frames(name)
        if flag(entry, ":GLASS") == want:
            return entry
        time.sleep(0.01)
    raise AssertionError(f"{name}: glass {'never came' if want else 'never left'}: {entry}")


def region(x, y, w, h, name):
    path = f"/run/glass-{name}.ppm"
    machine.succeed(ENV + f"WAYLAND_DISPLAY=check grim -c -g '{x},{y} {w}x{h}' -t ppm - > {path}")
    data = machine.succeed(f"od -An -v -tu1 {path}").split()
    return [int(v) for v in data][-3 * w * h:]


def pixels(values):
    return list(zip(values[0::3], values[1::3], values[2::3]))


machine.wait_for_unit("multi-user.target")
machine.succeed(f"cat > /run/glass-make.py <<'EOF'\n{MAKE}EOF")
machine.succeed(f"python3 /run/glass-make.py {ICONS}")
relink("glass-dark")
machine.succeed(f"cat > /run/glass.lisp <<'EOF'\n{POLICY}EOF")

start("--bare", "--config /run/glass.lisp", env=CURSOR)
assert inspect()[":LAST-ERROR"] == "NIL", inspect()
client("glass-backdrop", "--mode layer --layer background --anchor top,bottom,left,right "
       "--size 1280x720 --image /run/stripes.ppm")
HEAD = "HEADLESS-1"
point(400, 300)
started = time.monotonic()
moving = output_frames(HEAD)
assert not flag(moving, ":GLASS"), f"the glass came before the pointer rested: {moving}"
plain = region(390, 290, 48, 48, "plain")
rested = wait_glass(HEAD, True)
waited = time.monotonic() - started
print(f"glass after {waited * 1000:.0f} ms of rest (delay {DELAY_MS} ms)")
assert waited >= DELAY_MS / 1000 * 0.5, f"the glass came {waited * 1000:.0f} ms after motion"
glass = region(390, 290, 48, 48, "glass")
changed = sum(1 for a, b in zip(pixels(plain), pixels(glass)) if a != b)
print(f"glass changed {changed} of {48 * 48} pixels under the cursor")
assert changed > 40, "the glass draws nothing different from the plain cursor"
def crop(values, column, row):
    at = (row * 48 + column) * 3
    return values[at:at + 3]


def misses(rows, test):
    return [(row, test(row)) for row in rows if not test(row)]


dark_miss = misses(range(12, 21), lambda row: crop(glass, 9, row)[0] * 2 <=
                   crop(plain, 9, row)[0] + 6)
light_miss = misses(range(12, 21), lambda row: crop(glass, 8, row)[0] >= 150 and
                    crop(glass, 8, row)[0] >= crop(plain, 8, row)[0] + 6)
for row in (12, 16, 20):
    print(f"edge row {row}: light {crop(plain, 8, row)} -> {crop(glass, 8, row)}, "
          f"dark {crop(plain, 9, row)} -> {crop(glass, 9, row)}")
assert not dark_miss, f"the glass has no dark line against the silhouette: {dark_miss[:3]}"
assert not light_miss, f"the glass has no light line outside the silhouette: {light_miss[:3]}"
outside = region(300, 200, 40, 40, "outside")
assert region(300, 200, 40, 40, "outside-again") == outside

held = int(rested[":FRAME"])
time.sleep(1.0)
still = output_frames(HEAD)
print(f"frames while still: {int(still[':FRAME']) - held}")
assert int(still[":FRAME"]) - held <= 1, f"the still glass kept drawing: {held} -> {still}"
assert flag(still, ":GLASS")

relink("glass-light")
deadline = time.monotonic() + 10
flipped = glass
while time.monotonic() < deadline and flipped == glass:
    time.sleep(0.2)
    flipped = region(390, 290, 48, 48, "flipped")
if flipped == glass:
    print(machine.succeed(f"ls -la {ICONS} {ICONS}/glass/; ls -la /proc/$(systemctl show -P MainPID tomoe)/fd | grep -c inotify; journalctl -u tomoe -o cat | tail -40"))
    print(output_frames(HEAD))
assert flipped != glass, "the glass did not follow the relinked theme"
assert flag(output_frames(HEAD), ":GLASS"), "the relink dropped the glass"
relink("glass-dark")

point(420, 310)
moved = output_frames(HEAD)
assert not flag(moved, ":GLASS"), f"motion left the glass on screen: {moved}"
print("motion hands the cursor back at once")

client("glass-window", "--mode layer --layer top --anchor top,left --size 300x300 "
       "--commit-rate 30 --repaint --color 2266aa --change-to aa6622")
point(150, 150)
wait_glass(HEAD, True)
machine.succeed("systemctl kill -s USR1 glass-window")
time.sleep(0.4)
machine.succeed("systemctl kill -s USR1 glass-window")
time.sleep(0.4)
under = pixels(region(140, 140, 40, 40, "under"))
stale = [p for p in under if p[2] > p[0] + 40]
print(f"stale pixels under the glass after the window changed: {len(stale)}")
assert not stale, f"the glass still shows the window's old colour: {stale[:8]}"
first = region(140, 140, 40, 40, "steady-1")
time.sleep(0.3)
assert region(140, 140, 40, 40, "steady-2") == first, "the glass drifts over a steady window"
entry = output_frames(HEAD)
print(f"drawn pixels per frame over the changing window: {entry[':DRAWN-PIXELS']}")
assert flag(entry, ":GLASS")
assert inspect()[":LAST-ERROR"] == "NIL", inspect()
quit()
machine.succeed("systemctl stop glass-window glass-backdrop || true")

CARD = machine.succeed(
    "for c in /sys/class/drm/card*-Virtual-1; do basename $c | cut -d- -f1; done").strip()
DEVICE = f"/dev/dri/{CARD}"
KMS = "Virtual-1"


def cursor_fb():
    for line in machine.succeed(f"tomoe-planes-probe dump {DEVICE}").splitlines():
        words = line.split()
        if line.startswith("plane ") and "type=CURSOR" in words:
            return dict(word.partition("=")[::2] for word in words[2:]).get("fb")
    raise AssertionError("vkms has no cursor plane")


def nudge(dx, dy):
    unit = f"glass-move-{dx}-{dy}".replace("-", "n")
    machine.succeed(
        f"systemd-run --unit={unit} --property=Type=exec "
        f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
        f"{SCANOUT_CLIENT} --move {dx},{dy}")
    machine.wait_until_succeeds(f"journalctl -u {unit} -o cat | grep -qx moved", timeout=30)


machine.succeed("mount -t debugfs none /sys/kernel/debug || true")
machine.succeed("systemctl stop check-bus || true")
start("--bare", "--config /run/glass.lisp", f"--drm_device {DEVICE}", backend="drm", env=CURSOR)
nudge(40, 30)
entry = output_frames(KMS)
assert flag(entry, ":CURSOR-PLANE") and not flag(entry, ":GLASS"), entry
assert cursor_fb() != "0", "the moving cursor is not on the cursor plane"
entry = wait_glass(KMS, True)
assert not flag(entry, ":CURSOR-PLANE"), entry
machine.wait_until_succeeds(
    f"tomoe-planes-probe dump {DEVICE} | grep 'type=CURSOR' | grep -q ' fb=0'", timeout=5)
held = int(entry[":FRAME"])
time.sleep(1.0)
entry = output_frames(KMS)
print(f"vkms frames while the glass rests: {int(entry[':FRAME']) - held}")
assert int(entry[":FRAME"]) - held <= 1, entry
nudge(5, 5)
entry = output_frames(KMS)
assert flag(entry, ":CURSOR-PLANE") and not flag(entry, ":GLASS"), entry
assert cursor_fb() != "0", "motion did not put the cursor back on its plane"

wait_glass(KMS, True)
machine.succeed(
    "systemd-run --unit=glass-scanout --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
    f"{SCANOUT_CLIENT} --keep-cursor --color 336699")
machine.wait_until_succeeds("journalctl -u glass-scanout -o cat | grep -q '^mapped'", timeout=60)


def scanning():
    entry = output_frames(KMS)
    return flag(entry, ":SCANOUT") and flag(entry, ":CURSOR-PLANE") and not flag(entry, ":GLASS")


deadline = time.monotonic() + 10
while time.monotonic() < deadline and not scanning():
    time.sleep(0.05)
assert scanning(), f"the full screen window did not scan out under the glass: {output_frames(KMS)}"
nudge(10, 10)
time.sleep(0.6)
entry = output_frames(KMS)
assert scanning(), f"the resting pointer took the full screen window off scanout: {entry}"
assert cursor_fb() != "0", "the cursor left its plane over a scanned out window"
print("scanout holds under a resting cursor, which stays on its plane")
machine.succeed("systemctl stop glass-scanout")
assert inspect()[":LAST-ERROR"] == "NIL", inspect()
quit()
