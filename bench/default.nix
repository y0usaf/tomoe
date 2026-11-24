{ pkgs, tomoe, compositors ? "tomoe,sway,niri,hyprland" }:
let
  client = pkgs.callPackage ./client.nix { };
  closure = package: pkgs.closureInfo { rootPaths = [ package ]; };
  niri = pkgs.callPackage ./niri.nix { };
in
pkgs.testers.runNixOSTest {
  name = "tomoe-bench";
  globalTimeout = 7200;
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
      pkgs.python3
      pkgs.sway
      niri
      pkgs.hyprland
      pkgs.wayland-utils
      pkgs.procps
    ];
    environment.pathsToLink = [ "/share/dbus-1" ];
    environment.variables.LP_NUM_THREADS = "4";
    environment.variables.XKB_CONFIG_ROOT = "${pkgs.xkeyboard_config}/etc/X11/xkb";
    virtualisation.memorySize = 4096;
    virtualisation.cores = 4;
  };
  testScript = ''
    BENCH = "${./.}"
    PATHS = {
        "client": "${client}/bin/bench-client",
        "compositors": "${compositors}",
        "cores": "4",
        "memory": "4096",
        "lavapipe": "/run/opengl-driver/share/vulkan/icd.d/lvp_icd.${pkgs.stdenv.hostPlatform.uname.processor}.json",
        "closures": {
            "tomoe": "${closure tomoe}",
            "sway": "${closure pkgs.sway}",
            "niri": "${closure niri}",
            "hyprland": "${closure pkgs.hyprland}",
        },
    }
    exec(compile(open(BENCH + "/driver.py").read()
                 + "\nmain(machine, PATHS)\n", "bench", "exec"))
  '';
}
