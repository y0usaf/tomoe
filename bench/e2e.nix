{ pkgs, tomoe }:
let
  client = pkgs.callPackage ./client.nix { };
  capture = pkgs.callPackage ./capture.nix { };
in
pkgs.testers.runNixOSTest {
  name = "tomoe-e2e";
  globalTimeout = 600;
  nodes.machine = {
    boot.kernelModules = [
      "vkms"
      "vgem"
      "udmabuf"
    ];
    virtualisation.qemu.options = [
      "-vga"
      "none"
    ];
    hardware.graphics.enable = true;
    hardware.uinput.enable = true;
    services.seatd.enable = true;
    systemd.services.seatd.environment.SEATD_VTBOUND = "0";
    environment.systemPackages = [
      tomoe
      client
      capture
      pkgs.python3
      pkgs.procps
    ];
    environment.pathsToLink = [ "/share/dbus-1" ];
    environment.variables.LP_NUM_THREADS = "4";
    virtualisation.memorySize = 2048;
    virtualisation.cores = 4;
  };
  testScript = ''
    import re
    import time

    machine.wait_for_unit("seatd.service")
    machine.succeed("mkdir -p /run/e2e/rt /run/e2e/cfg /run/e2e/cap && chmod 700 /run/e2e/rt")
    machine.succeed(
        "printf '(in-package #:tomoe-user)\n(load-policy \"desktop\")\n' > /run/e2e/cfg/init.lisp"
    )

    env_vars = {
        "HOME": "/root",
        "PATH": "/run/current-system/sw/bin",
        "XDG_RUNTIME_DIR": "/run/e2e/rt",
        "LIBGL_ALWAYS_SOFTWARE": "1",
        "GALLIUM_DRIVER": "llvmpipe",
        "LP_NUM_THREADS": "4",
        "MESA_LOADER_DRIVER_OVERRIDE": "zink",
        "NO_AT_BRIDGE": "1",
    }
    unit_env = " ".join(f"--setenv={k}={v}" for k, v in env_vars.items())
    sh_env = " ".join(f"{k}={v}" for k, v in env_vars.items())

    machine.succeed(
        f"systemd-run --unit=e2e-tomoe --collect {unit_env} "
        "tomoe --socket e2e --backend drm --config /run/e2e/cfg/init.lisp --no-watch"
    )

    inspect = ""
    for _ in range(300):
        rc, out = machine.execute(f"{sh_env} tomoe --socket e2e inspect")
        if rc == 0 and out.strip():
            inspect = out
            break
        time.sleep(0.1)
    assert inspect.strip(), "tomoe did not become ready"
    assert not re.search(r":CONFIG-ERROR \(", inspect), f"tomoe config error: {inspect[:300]}"

    machine.succeed("test -S /run/e2e/rt/e2e")
    wl_display = "e2e"

    frames_out = machine.succeed(
        f"{sh_env} WAYLAND_DISPLAY={wl_display} ${client}/bin/bench-client --phase frames --seconds 1 --windows 1"
    )
    m = re.search(r"frames\s+(\d+)", frames_out)
    assert m and int(m.group(1)) > 0, f"bench-client presented no frames: {frames_out}"

    m_out = re.search(r':NAME\s+"([^"]+)"', inspect)
    output_name = m_out.group(1) if m_out else "Virtual-1"

    machine.succeed(
        f"bash -c '{sh_env} WAYLAND_DISPLAY={wl_display} ${capture}/bin/tomoe-e2e-capture --output {output_name} --dump-dir /run/e2e/cap --dump 1 >/run/e2e/cap.log 2>&1 & "
        "cpid=$!; "
        f"{sh_env} WAYLAND_DISPLAY={wl_display} ${client}/bin/bench-client --phase frames --seconds 1 --windows 1 >/dev/null; "
        "for i in $(seq 1 50); do [ -s /run/e2e/cap/frame-000001.ppm ] && break; sleep 0.1; done; "
        "kill $cpid 2>/dev/null || true; wait $cpid 2>/dev/null || true'"
    )
    machine.succeed("test -s /run/e2e/cap/frame-000001.ppm")

    machine.succeed("systemctl stop e2e-tomoe")
    for _ in range(50):
        rc, _ = machine.execute("pgrep -x tomoe")
        if rc != 0:
            break
        time.sleep(0.1)
    rc, _ = machine.execute("pgrep -x tomoe")
    assert rc != 0, "leftover tomoe process after stop"
    leftover = [
        x for x in machine.succeed("ls -A /run/e2e/rt").split()
        if x == "e2e" or x.startswith("wayland-")
    ]
    assert not leftover, f"leftover sockets in /run/e2e/rt: {leftover}"
  '';
}
