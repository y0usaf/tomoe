{ pkgs, tomoe, compositors ? "tomoe,sway,niri,hyprland" }:
let
  client = pkgs.callPackage ./client.nix { };
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
    environment.systemPackages = [
      tomoe
      client
      pkgs.python3
      pkgs.sway
      pkgs.niri
      pkgs.hyprland
      pkgs.wayland-utils
      pkgs.procps
    ];
    environment.pathsToLink = [ "/share/dbus-1" ];
    environment.variables.LP_NUM_THREADS = "4";
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
    }
    exec(compile(open(BENCH + "/driver.py").read()
                 + "\nmain(machine, PATHS)\n", "bench", "exec"))
  '';
}
