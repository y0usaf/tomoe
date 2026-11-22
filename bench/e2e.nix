{ pkgs, tomoe }:
let
  client = pkgs.callPackage ./client.nix { };
  capture = pkgs.callPackage ./capture.nix { };
  subsurface = pkgs.callPackage ./e2e-subsurface.nix { };
  layerBlur = pkgs.callPackage ./e2e-layer-blur.nix { };
  pwPortal = pkgs.callPackage ./e2e-pw-portal.nix { };
  pyDbus = pkgs.python3.withPackages (ps: [ ps.dbus-python ]);
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
      subsurface
      layerBlur
      pwPortal
      pyDbus
      pkgs.dbus
      pkgs.pipewire
      pkgs.wireplumber
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
    machine.wait_for_file("/dev/dri/card0")
    machine.succeed("mkdir -p /run/e2e/rt /run/e2e/cfg /run/e2e/cap && chmod 700 /run/e2e/rt")
    machine.succeed(
        "cat <<'EOF' > /run/e2e/cfg/init.lisp\n"
        "(in-package #:tomoe-user)\n"
        "(load-policy \"desktop\")\n"
        "(define-extension \"e2e-vout\" (:reads (:outputs :windows)) (snapshot state event)\n"
        "  (declare (ignore state event))\n"
        "  (let* ((outs (context snapshot :outputs))\n"
        "         (first-out (find-if (lambda (o) (not (equal (getf o :name) \"VOUT-2\"))) outs))\n"
        "         (first-name (and first-out (getf first-out :name)))\n"
        "         (fw (if first-out (getf first-out :width 1024) 1024))\n"
        "         (sub-win (find \"e2e-subsurface\" (context snapshot :windows)\n"
        "                        :test #'equal :key (lambda (w) (getf w :title)))))\n"
        "    (values nil\n"
        "            (remove nil\n"
        "                    (list (and first-name (configure-output first-name :position '(0 0)))\n"
        "                          (virtual-output \"VOUT-2\" :mode '(1280 720 60))\n"
        "                          (configure-output \"VOUT-2\" :position (list fw 0))\n"
        "                          (and sub-win (set-view 0 0 1))\n"
        "                          (and sub-win (place (getf sub-win :id) (- fw 250) 50 600 300 t)))))))\n"
        "EOF"
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
        if rc == 0 and "VOUT-2" in out:
            inspect = out
            break
        time.sleep(0.1)
    assert inspect.strip(), "tomoe did not become ready with VOUT-2"
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

    sub_rc, sub_out = machine.execute(
        f"{sh_env} WAYLAND_DISPLAY={wl_display} ${subsurface}/bin/tomoe-e2e-subsurface"
    )
    print(f"E2E_SUBSURFACE rc={sub_rc} out={sub_out.strip()}")

    blur_rc, blur_out = machine.execute(
        f"{sh_env} WAYLAND_DISPLAY={wl_display} ${layerBlur}/bin/tomoe-e2e-layer-blur --frames 3"
    )
    print(f"E2E_LAYER_BLUR rc={blur_rc} out={blur_out.strip()}")

    bus_addr = "unix:path=/run/e2e/rt/bus"
    machine.succeed(
        f"systemd-run --unit=e2e-dbus --collect {unit_env} "
        "dbus-daemon --session --nofork --address=unix:path=/run/e2e/rt/bus"
    )
    for _ in range(100):
        rc, _ = machine.execute("test -S /run/e2e/rt/bus")
        if rc == 0:
            break
        time.sleep(0.05)
    machine.succeed("test -S /run/e2e/rt/bus")

    machine.succeed(
        f"systemd-run --unit=e2e-pipewire --collect {unit_env} "
        f"--setenv=DBUS_SESSION_BUS_ADDRESS={bus_addr} "
        "--setenv=PIPEWIRE_RUNTIME_DIR=/run/e2e/rt "
        "pipewire"
    )
    for _ in range(100):
        rc, _ = machine.execute("test -S /run/e2e/rt/pipewire-0")
        if rc == 0:
            break
        time.sleep(0.05)
    machine.succeed("test -S /run/e2e/rt/pipewire-0")

    machine.succeed(
        f"systemd-run --unit=e2e-wireplumber --collect {unit_env} "
        f"--setenv=DBUS_SESSION_BUS_ADDRESS={bus_addr} "
        "--setenv=PIPEWIRE_RUNTIME_DIR=/run/e2e/rt "
        "wireplumber"
    )
    time.sleep(1.0)

    machine.succeed(
        f"systemd-run --unit=e2e-portal --collect {unit_env} "
        f"--setenv=WAYLAND_DISPLAY={wl_display} "
        f"--setenv=DBUS_SESSION_BUS_ADDRESS={bus_addr} "
        "--setenv=PIPEWIRE_RUNTIME_DIR=/run/e2e/rt "
        "--setenv=TOMOE_SOCKET=/nonexistent "
        "${tomoe}/libexec/xdg-desktop-portal-tomoe"
    )

    machine.succeed(
        "cat <<'EOF' > /run/e2e/pw_driver.py\n"
        "import os, subprocess, sys, time, dbus\n"
        "bus = dbus.SessionBus()\n"
        "for _ in range(100):\n"
        "    if bus.name_has_owner('org.freedesktop.impl.portal.desktop.tomoe'):\n"
        "        break\n"
        "    time.sleep(0.05)\n"
        "else:\n"
        "    print('portal did not claim dbus name', file=sys.stderr)\n"
        "    sys.exit(2)\n"
        "obj = bus.get_object('org.freedesktop.impl.portal.desktop.tomoe', '/org/freedesktop/portal/desktop')\n"
        "iface = dbus.Interface(obj, 'org.freedesktop.impl.portal.ScreenCast')\n"
        "sp = dbus.ObjectPath('/org/freedesktop/portal/desktop/session/e2e/s1')\n"
        "rc1, _ = iface.CreateSession(dbus.ObjectPath('/org/freedesktop/portal/desktop/request/e2e/r1'), sp, 'e2e', {})\n"
        "assert int(rc1) == 0, f'CreateSession rc={rc1}'\n"
        "rc2, _ = iface.SelectSources(dbus.ObjectPath('/org/freedesktop/portal/desktop/request/e2e/r2'), sp, 'e2e', {'types': dbus.UInt32(1)})\n"
        "assert int(rc2) == 0, f'SelectSources rc={rc2}'\n"
        "rc3, res = iface.Start(dbus.ObjectPath('/org/freedesktop/portal/desktop/request/e2e/r3'), sp, 'e2e', str(), {})\n"
        "assert int(rc3) == 0, f'Start rc={rc3} res={res}'\n"
        "streams = res.get('streams', [])\n"
        "assert len(streams) > 0, f'no streams in {res}'\n"
        "node_id = int(streams[0][0])\n"
        "bench = subprocess.Popen([sys.argv[1], '--phase', 'frames', '--seconds', '4', '--windows', '1'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)\n"
        "try:\n"
        "    r = subprocess.run([sys.argv[2], '--node', str(node_id), '--timeout', '6', '--frames', '1'] + sys.argv[3:])\n"
        "finally:\n"
        "    bench.terminate()\n"
        "    try:\n"
        "        bench.wait(timeout=2)\n"
        "    except Exception:\n"
        "        bench.kill()\n"
        "sys.exit(r.returncode)\n"
        "EOF"
    )

    pw_rc, pw_out = machine.execute(
        f"{sh_env} WAYLAND_DISPLAY={wl_display} DBUS_SESSION_BUS_ADDRESS={bus_addr} "
        f"PIPEWIRE_RUNTIME_DIR=/run/e2e/rt ${pyDbus}/bin/python3 /run/e2e/pw_driver.py "
        f"${client}/bin/bench-client ${pwPortal}/bin/tomoe-e2e-pw-portal"
    )
    print(f"E2E_PW_PORTAL rc={pw_rc} out={pw_out.strip()}")
    pw_shm_rc, pw_shm_out = machine.execute(
        f"{sh_env} WAYLAND_DISPLAY={wl_display} DBUS_SESSION_BUS_ADDRESS={bus_addr} "
        f"PIPEWIRE_RUNTIME_DIR=/run/e2e/rt ${pyDbus}/bin/python3 /run/e2e/pw_driver.py "
        f"${client}/bin/bench-client ${pwPortal}/bin/tomoe-e2e-pw-portal --shm-only"
    )
    print(f"E2E_PW_PORTAL_SHM rc={pw_shm_rc} out={pw_shm_out.strip()}")
    if pw_rc != 0 or pw_shm_rc != 0:
        _, plog = machine.execute("journalctl -u e2e-portal -u e2e-pipewire --no-pager -n 60")
        print(f"E2E_PORTAL_LOG:\n{plog}")

    machine.execute("systemctl stop e2e-portal e2e-wireplumber e2e-pipewire e2e-dbus")
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
    assert sub_rc == 0, f"e2e-subsurface failed (rc={sub_rc}): {sub_out}"
    assert blur_rc == 0, f"e2e-layer-blur failed (rc={blur_rc}): {blur_out}"
    assert pw_rc == 0, f"e2e-pw-portal failed (rc={pw_rc}): {pw_out}"
    assert pw_shm_rc == 0, f"e2e-pw-portal --shm-only failed (rc={pw_shm_rc}): {pw_shm_out}"
  '';
}
