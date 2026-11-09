start("--bare")
CLIENT_ENV = ENV + "WAYLAND_DISPLAY=check "


def randr(arguments=""):
    return machine.succeed(CLIENT_ENV + "wlr-randr " + arguments)


assert "zwlr_output_manager_v1" in machine.succeed(CLIENT_ENV + "wayland-info")
before = randr()
assert "1280x720" in before and "Enabled: yes" in before, before
randr("--dryrun --output HEADLESS-1 --custom-mode 1024x768@60Hz --pos 17,23 --scale 1.25")
assert randr() == before, "test request changed output state"
for scenario in ["stale", "omit", "duplicate", "property", "invalid-scale", "twice", "mode"]:
    machine.succeed(CLIENT_ENV + "tomoe-output-management-client " + scenario)
machine.succeed(
    "systemd-run --unit=output-stale --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
    "tomoe-output-management-client cancel"
)
machine.wait_until_succeeds("journalctl -u output-stale -o cat | grep -qx ready", timeout=30)
randr("--output HEADLESS-1 --custom-mode 1024x768@60Hz --pos 17,23 --scale 1.25 --transform 90")
machine.wait_until_succeeds("journalctl -u output-stale -o cat | grep -qx 'cancel passed'", timeout=30)
changed = randr()
assert "1024x768" in changed and "Position: 17,23" in changed, changed
assert "Scale: 1.250000" in changed and "Transform: 90" in changed, changed
state = inspect()
output = plist(items(state[":OUTPUTS"])[0])
assert output[":WIDTH"] == "768" and output[":HEIGHT"] == "1024", show(output)
failed = machine.execute(CLIENT_ENV + "wlr-randr --output HEADLESS-1 --adaptive-sync enabled")
assert failed[0] != 0, "unsupported VRR was accepted"
assert randr() == changed, "failed transaction changed output state"
failed = machine.execute(CLIENT_ENV + "wlr-randr --output HEADLESS-1 --pos 2147483647,0")
assert failed[0] != 0, "overflowing placement was accepted"
assert randr() == changed, "invalid placement changed output state"
randr("--output HEADLESS-1 --off")
assert "Enabled: no" in randr()
assert not items(inspect()[":OUTPUTS"])
cli(f"mount {OUTPUT_OVERRIDE_PROBE}")
assert "800x600" in randr() and "Enabled: yes" in randr()
cli("unmount output-override-probe")
assert "Enabled: no" in randr(), "unmount did not restore externally disabled baseline"
randr("--output HEADLESS-1 --on --custom-mode 1024x768@60Hz --scale 1.25 --transform 90 --pos 17,23")
assert randr() == changed, "enable did not apply the requested mode, scale or placement"
cli(f"mount {OUTPUT_OVERRIDE_PROBE}")
assert "800x600" in randr()
cli("unmount output-override-probe")
assert randr() == changed, "unmount did not restore the accepted output baseline"
cli(f"mount {VIRTUAL_PROBE}")
for _ in range(100):
    if "VIRTUAL-2" in randr():
        break
    time.sleep(0.1)
else:
    raise AssertionError("virtual heads were not advertised")
before_multi = randr()
failed = machine.execute(
    CLIENT_ENV + "wlr-randr --output HEADLESS-1 --custom-mode 800x600@60Hz "
    "--output VIRTUAL-1 --adaptive-sync enabled"
)
assert failed[0] != 0 and randr() == before_multi, "failed multihead transaction changed a head"
randr("--output VIRTUAL-1 --custom-mode 960x540@60Hz --pos 1200,0")
multi = randr()
assert "960x540" in multi and "Position: 1200,0" in multi, multi
assert changed.strip() in multi, "configuring a second head changed the first head"
machine.succeed(
    "systemd-run --unit=output-watch --property=Type=exec "
    f"--setenv=XDG_RUNTIME_DIR={RUNTIME} --setenv=WAYLAND_DISPLAY=check "
    "tomoe-output-management-client watch VIRTUAL-1"
)
machine.wait_until_succeeds("journalctl -u output-watch -o cat | grep -qx ready", timeout=30)
cli("unmount virtual-probe")
machine.wait_until_succeeds(
    "journalctl -u output-watch -o cat | grep -qx 'head and modes finished'", timeout=30
)
assert "VIRTUAL-1" not in randr()
assert "Position: 17,23" in randr(), "unrelated extension lost external output placement"
quit()
