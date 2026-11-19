DELAY_MS = 120
KMS_DELAY_MS = 3000
ICONS = "/run/glass-icons"
CURSOR = {"XCURSOR_PATH": ICONS, "XCURSOR_THEME": "glass", "XCURSOR_SIZE": "32",
          "TOMOE_DEBUG": "1"}
SIZE = 32
OLD, NEW = (0x22, 0x66, 0xAA), (0xAA, 0x66, 0x22)
def policy(delay_ms):
    return f"""(in-package #:tomoe-user)
(define-extension "glass-test" (:reads (:outputs :windows) :state nil) (snapshot state event)
  (declare (ignore state event))
  (let ((output (first (context snapshot :outputs))))
    (values nil
            (append (list (settings :cursor-glass (list :enabled t :delay-ms {delay_ms})))
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


POLICY = policy(DELAY_MS)




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


def wait_plain(name, timeout=2.0):
    deadline = time.monotonic() + timeout
    entry = output_frames(name)
    while time.monotonic() < deadline:
        entry = output_frames(name)
        if flag(entry, ":CURSOR-PLANE") and not flag(entry, ":GLASS"):
            return entry
        time.sleep(0.01)
    raise AssertionError(f"{name}: the cursor never returned to its plane: {entry}")


def region(x, y, w, h, name):
    path = f"/run/glass-{name}.ppm"
    machine.succeed(ENV + f"WAYLAND_DISPLAY=check grim -c -g '{x},{y} {w}x{h}' -t ppm - > {path}")
    data = machine.succeed(f"od -An -v -tu1 {path}").split()
    return [int(v) for v in data][-3 * w * h:]


def pixels(values):
    return list(zip(values[0::3], values[1::3], values[2::3]))


machine.wait_for_unit("multi-user.target")
machine.succeed(f"{GLASS_MAKE} {ICONS}")
relink("glass-dark")
machine.succeed(f"cat > /run/glass.lisp <<'EOF'\n{POLICY}EOF")
machine.succeed(f"cat > /run/glass-kms.lisp <<'EOF'\n{policy(KMS_DELAY_MS)}EOF")

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


dark_miss = misses(range(12, 21), lambda row: crop(plain, 9, row)[0] < 150 or
                   crop(glass, 9, row)[0] <= crop(plain, 9, row)[0] - 40)
light_miss = misses(range(12, 21), lambda row: crop(plain, 8, row)[0] >= 150 or
                    crop(glass, 8, row)[0] >= crop(plain, 8, row)[0] + 40)
for row in (12, 16, 20):
    print(f"edge row {row}: light {crop(plain, 8, row)} -> {crop(glass, 8, row)}, "
          f"dark {crop(plain, 9, row)} -> {crop(glass, 9, row)}")
assert not dark_miss, f"the glass has no dark line against a light backdrop: {dark_miss[:3]}"
assert not light_miss, f"the glass has no light line over a dark backdrop: {light_miss[:3]}"


def solid(unit, colour):
    machine.execute(f"systemctl stop {unit} 2>/dev/null; systemctl reset-failed {unit}",
                    check_return=False)
    client(unit, "--mode layer --layer top --anchor top,bottom,left,right "
                 f"--size 1280x720 --color {colour}")
    point(400, 300)
    wait_glass(HEAD, True)
    return region(390, 290, 48, 48, unit)


def body(values):
    inside = [(col, row) for row in range(27, 33) for col in range(13, 19)]
    return sum(crop(values, col, row)[0] for col, row in inside) / len(inside)


light = body(solid("glass-flat-light", "F4F4F6"))
dark = body(solid("glass-flat-dark", "121218"))
print(f"body over a light backdrop {light:.0f}, over a dark one {dark:.0f}")
assert light < 190, f"the glass body does not read over a light backdrop: {light:.0f}"
assert dark > 120, f"the glass body does not read over a dark backdrop: {dark:.0f}"
machine.execute("systemctl stop glass-flat-light glass-flat-dark 2>/dev/null; "
                "systemctl reset-failed glass-flat-light glass-flat-dark",
                check_return=False)
point(400, 300)
wait_glass(HEAD, True)
outside = region(300, 200, 40, 40, "outside")
assert region(300, 200, 40, 40, "outside-again") == outside

held = int(output_frames(HEAD)[":FRAME"])
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
start("--bare", "--config /run/glass-kms.lisp", f"--drm_device {DEVICE}", backend="drm", env=CURSOR)
wait_glass(KMS, True, timeout=8)
nudge(40, 30)
wait_plain(KMS)
assert cursor_fb() != "0", "the moving cursor is not on the cursor plane"
entry = wait_glass(KMS, True, timeout=8)
assert not flag(entry, ":CURSOR-PLANE"), entry
machine.wait_until_succeeds(
    f"tomoe-planes-probe dump {DEVICE} | grep 'type=CURSOR' | grep -q ' fb=0'", timeout=5)
held = int(entry[":FRAME"])
time.sleep(1.0)
entry = output_frames(KMS)
print(f"vkms frames while the glass rests: {int(entry[':FRAME']) - held}")
assert int(entry[":FRAME"]) - held <= 1, entry
nudge(5, 5)
wait_plain(KMS)
assert cursor_fb() != "0", "motion did not put the cursor back on its plane"

wait_glass(KMS, True, timeout=8)
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
