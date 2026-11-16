SOCKET = "/run/fake-seatd.sock"

machine.wait_for_unit("multi-user.target")
machine.execute("systemctl stop fake-seatd 2>/dev/null", check_return=False)
machine.succeed("mkdir -p -m 700 /run/tomoe")
machine.succeed(
    f"systemd-run --unit=fake-seatd --property=Type=exec python3 {FAKE_SEATD} {SOCKET}")
machine.wait_for_file(SOCKET, timeout=30)
status, output = machine.execute(
    f"XDG_RUNTIME_DIR=/run/tomoe SEATD_SOCK={SOCKET} HOME=/root "
    "tomoe --socket check --backend drm --no-watch --drm_device /dev/zero",
    check_return=False)
assert status != 0, output
assert "could not open /dev/zero" in output, output
log = machine.succeed("journalctl -u fake-seatd -o cat")
assert "seat opened" in log, log
assert "device refused" in log, log
assert "seat closed" in log, log
