start("--bare")
client("pixel", "--mode layer --anchor top,left --size 120x80 --color e04020 --single-pixel")
for _ in range(100):
    data = machine.succeed(ENV + "WAYLAND_DISPLAY=check grim -g '40,40 1x1' -t ppm - | od -An -v -tu1").split()
    if [int(v) for v in data[-3:]] == [224, 64, 32]:
        break
    time.sleep(0.05)
else:
    raise AssertionError(data)
machine.succeed("systemctl stop pixel")

def pixel():
    return [int(v) for v in machine.succeed(ENV + "WAYLAND_DISPLAY=check grim -g '40,40 1x1' -t ppm - | od -An -v -tu1").split()][-3:]

def expect_pixel(want):
    for _ in range(100):
        got = pixel()
        if all(abs(a - b) <= 2 for a, b in zip(got, want)):
            return
        time.sleep(0.05)
    raise AssertionError((got, want))

client("alpha", "--mode layer --anchor top,left --size 120x80 --color e04020 --alpha-probe")
expect_pixel([119, 39, 23])
for step, want in enumerate([[119, 39, 23], [13, 13, 13], [13, 13, 13], [224, 64, 32]], 1):
    machine.succeed("systemctl kill --kill-whom=main --signal=USR1 alpha")
    machine.wait_until_succeeds(f"journalctl -u alpha -o cat | grep -qx 'alpha-step {step}'")
    expect_pixel(want)
machine.succeed("systemctl stop alpha")
for mode, interface in [("alpha-duplicate", "wp_alpha_modifier_v1"), ("alpha-orphan", "wp_alpha_modifier_surface_v1")]:
    error = machine.fail(ENV + f"WAYLAND_DISPLAY=check tomoe-test-client --protocol-error {mode} 2>&1")
    assert f"protocol error on {interface}@" in error and ": 0" in error, error

def content_type():
    return plist(items(inspect()[":WINDOWS"])[0])[":CONTENT-TYPE"]

client("content", "--content-probe")
for _ in range(100):
    if content_type() == ":GAME":
        break
    time.sleep(0.05)
else:
    raise AssertionError(inspect())
for step, want in enumerate([":GAME", ":PHOTO", ":PHOTO", ":NONE"], 1):
    machine.succeed("systemctl kill --kill-whom=main --signal=USR1 content")
    machine.wait_until_succeeds(f"journalctl -u content -o cat | grep -qx 'content-step {step}'")
    assert content_type() == want, inspect()
machine.succeed("systemctl stop content")
for mode in ["content-duplicate", "content-invalid"]:
    error = machine.fail(ENV + f"WAYLAND_DISPLAY=check tomoe-test-client --protocol-error {mode} 2>&1")
    assert "protocol error on wp_content_type" in error, error
machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-test-client --protocol-error content-orphan")

quit()
