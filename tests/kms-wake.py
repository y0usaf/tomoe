CARD = machine.succeed(
    "for c in /sys/class/drm/card*-Virtual-1; do basename $c | cut -d- -f1; done"
).strip()
CONNECTOR = f"/sys/class/drm/{CARD}-Virtual-1"
JOURNAL = "journalctl -o cat _SYSTEMD_INVOCATION_ID=$(systemctl show -P InvocationID tomoe)"
DRM = {
    "TOMOE_DEBUG": "1",
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


start(f"--config {WAKE_PROBE}", "--test-drm-wake-ms=1500", f"--drm_device /dev/dri/{CARD}", backend="drm", env=DRM)
kernel_mode("1920x1080@60")
before = kms_log().splitlines()
assert any("Virtual-1 state before Tomoe: connected" in line and "ACTIVE 1" in line
           for line in before), "\n".join(before)
states = [line for line in before if "Virtual-1 kernel state" in line]
assert states and "mode 1920x1080@60" in states[0], "\n".join(before)
assert "at its preferred" not in "\n".join(before), "\n".join(before)

dark()
mark = len(kms_log().splitlines())
probe("light")
machine.wait_until_succeeds(f"grep -qx enabled {CONNECTOR}/enabled", timeout=30)
wait_log("Virtual-1 was dark; lit at its preferred 1024x768@60", 1)
kernel_mode("1024x768@60")
woke = output()
assert (woke[":WIDTH"], woke[":HEIGHT"]) == ("1920", "1080"), woke
kernel_mode("1920x1080@60")
after = kms_log().splitlines()[mark:]
lit = next(i for i, line in enumerate(after) if "at its preferred" in line)
target = next(i for i, line in enumerate(after) if i > lit and "kernel state" in line and "mode 1920x1080@60" in line)
assert any("mode 1024x768@60" in line for line in after[lit:target]), "\n".join(after)
assert any("Virtual-1 probed before lighting: connected" in line for line in after[:lit]), "\n".join(after)
assert inspect()[":LAST-ERROR"] == "NIL", inspect()[":LAST-ERROR"]

machine.succeed(f"echo change > /sys/class/drm/{CARD}/uevent")
machine.wait_until_succeeds(
    f"{JOURNAL} | grep -qE 'connectors were rescanned|could not read the DRM resources'",
    timeout=30)
assert "could not read the DRM resources" not in machine.succeed(JOURNAL), "the rescan failed"
machine.wait_until_succeeds(f"grep -qx enabled {CONNECTOR}/enabled", timeout=30)
kernel_mode("1920x1080@60")
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
lit = next(i for i, line in enumerate(after) if i > probed and "at its preferred" in line)
assert any("kernel state" in line and "mode 1920x1080@60" in line for line in after[lit:]), "\n".join(after)
assert "link-status=0" in machine.succeed(JOURNAL)
assert inspect()[":LAST-ERROR"] == "NIL", inspect()[":LAST-ERROR"]
quit()

machine.succeed("systemctl stop tomoe check-bus")
start("--bare", "--test-drm-wake-ms=0", f"--drm_device /dev/dri/{CARD}", backend="drm", bus=False, env=DRM)
cli(f"mount {WAKE_PROBE}")
kernel_mode("1920x1080@60")
dark()
probe("light")
kernel_mode("1920x1080@60")
assert "at its preferred" not in kms_log(), kms_log()
quit()
