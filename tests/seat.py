ERROR_SOCKET = "/run/fake-seatd-error.sock"
DRM_SOCKET = "/run/fake-seatd-drm.sock"

machine.wait_for_unit("multi-user.target")
drm = machine.succeed(f"{DRM_VERSION} /dev/dri/card* /dev/dri/renderD* 2>&1")
print(drm)
assert " vkms vkms" in drm, drm
machine.succeed("mkdir -p -m 700 /run/tomoe")
machine.execute("systemctl stop fake-seatd-error fake-seatd-drm 2>/dev/null",
                check_return=False)

machine.succeed(
    f"systemd-run --unit=fake-seatd-error --property=Type=exec {FAKE_SEATD} "
    f"{ERROR_SOCKET} error")
machine.wait_for_file(ERROR_SOCKET, timeout=30)
machine.execute(
    f"XDG_RUNTIME_DIR=/run/tomoe SEATD_SOCK={ERROR_SOCKET} HOME=/root "
    "tomoe --socket check --backend drm --no-watch",
    check_return=False)
machine.wait_until_succeeds(
    "journalctl -u fake-seatd-error -o cat | grep -qx 'seat closed'", timeout=30)
log = machine.succeed("journalctl -u fake-seatd-error -o cat")
assert "refused with disable" in log, log
assert "ping" in log, log
assert log.index("refused with disable") < log.index("ping") < log.index("seat closed"), log

machine.succeed(
    f"systemd-run --unit=fake-seatd-drm --property=Type=exec {FAKE_SEATD} "
    f"{DRM_SOCKET} drm")
machine.wait_for_file(DRM_SOCKET, timeout=30)
start("--bare", backend="drm", env={"SEATD_SOCK": DRM_SOCKET})
machine.wait_until_succeeds(
    "journalctl -u fake-seatd-drm -o cat | grep -qx 'disable acked'", timeout=60)
log = machine.succeed("journalctl -u fake-seatd-drm -o cat")
assert "opened with disable" in log, log
assert "ping" in log, log
assert log.index("opened with disable") < log.index("ping") < log.index("disable acked"), log
log = machine.succeed(
    "journalctl -o cat _SYSTEMD_INVOCATION_ID=$(systemctl show -P InvocationID tomoe)")
assert "the seat is inactive; outputs stop drawing" in log, log
quit()
log = machine.succeed("journalctl -u fake-seatd-drm -o cat")
assert "seat closed" in log, log
