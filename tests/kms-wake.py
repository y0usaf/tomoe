CARD = machine.succeed(
    "for c in /sys/class/drm/card*-Virtual-1; do basename $c | cut -d- -f1; done"
).strip()
CONNECTOR = f"/sys/class/drm/{CARD}-Virtual-1"
JOURNAL = "journalctl -o cat _SYSTEMD_INVOCATION_ID=$(systemctl show -P InvocationID tomoe)"
DRM = {
    "TOMOE_DEBUG": "1",
    "LIBSEAT_BACKEND": "noop",
}


def kms_log():
    return machine.succeed(f"{JOURNAL} | grep -F 'tomoe: kms' || true")

def wait_log(text, count):
    machine.wait_until_succeeds(f"test $({JOURNAL} | grep -cF '{text}') -ge {count}", timeout=30)



def output(name="Virtual-1"):
    for entry in items(inspect()[":OUTPUTS"]):
        entry = plist(entry)
        if text(entry[":NAME"]) == name:
            return entry
    return None


def probe(command):
    cli(f"command {command}")


def kernel_mode(mode):
    machine.wait_until_succeeds(
        f"{JOURNAL} | grep -F 'Virtual-1 kernel state' | tail -1 | grep -F 'ACTIVE 1' | grep -qF 'mode {mode}'",
        timeout=30,
    )


def dark():
    probe("dark")
    machine.wait_until_succeeds(f"grep -qx disabled {CONNECTOR}/enabled", timeout=30)
    machine.wait_until_succeeds(
        f"{JOURNAL} | grep -F 'Virtual-1 kernel state' | tail -1 | grep -qF 'CRTC_ID 0'",
        timeout=30,
    )


start("--bare", f"--drm_device /dev/dri/{CARD}", backend="drm", env=dict(DRM, TOMOE_DRM_WAKE_MS="1500"))
cli(f"mount {WAKE_PROBE}")
kernel_mode("1920x1080@60")
assert "was dark" not in kms_log(), kms_log()
assert "Virtual-1 state before Tomoe: connected" in kms_log(), kms_log()

dark()
mark = len(kms_log().splitlines())
probe("light")
machine.wait_until_succeeds(f"grep -qx enabled {CONNECTOR}/enabled", timeout=30)
machine.wait_until_succeeds(f"{JOURNAL} | grep -qF 'Virtual-1 was dark; lit at its preferred 1024x768@60'", timeout=30)
kernel_mode("1024x768@60")
woke = output()
assert (woke[":WIDTH"], woke[":HEIGHT"]) == ("1920", "1080"), woke
kernel_mode("1920x1080@60")
after = kms_log().splitlines()[mark:]
lit = next(i for i, line in enumerate(after) if "was dark" in line)
target = next(i for i, line in enumerate(after) if i > lit and "kernel state" in line and "mode 1920x1080@60" in line)
assert any("mode 1024x768@60" in line for line in after[lit:target]), "\n".join(after)
assert any("Virtual-1 probed before lighting: connected" in line for line in after[:lit]), "\n".join(after)
assert inspect()[":LAST-ERROR"] == "NIL", inspect()[":LAST-ERROR"]

mark = len(kms_log().splitlines())
probe("off")
machine.wait_until_succeeds(f"grep -qx disabled {CONNECTOR}/enabled", timeout=30)
probe("on")
wait_log("Virtual-1 was dark; lit at its preferred 1024x768@60", 2)
machine.wait_until_succeeds(f"grep -qx enabled {CONNECTOR}/enabled", timeout=30)
kernel_mode("1920x1080@60")
after = kms_log().splitlines()[mark:]
probed = next(i for i, line in enumerate(after) if "Virtual-1 probed before lighting: connected" in line)
lit = next(i for i, line in enumerate(after) if i > probed and "was dark" in line)
assert any("kernel state" in line and "mode 1920x1080@60" in line for line in after[lit:]), "\n".join(after)
assert "link-status=0" in machine.succeed(JOURNAL)
assert inspect()[":LAST-ERROR"] == "NIL", inspect()[":LAST-ERROR"]
quit()

machine.succeed("systemctl stop tomoe check-bus")
start("--bare", f"--drm_device /dev/dri/{CARD}", backend="drm", bus=False, env=dict(DRM, TOMOE_DRM_WAKE_MS="0"))
cli(f"mount {WAKE_PROBE}")
kernel_mode("1920x1080@60")
dark()
probe("light")
kernel_mode("1920x1080@60")
assert "was dark" not in kms_log(), kms_log()
quit()
