{ pkgs, tomoe }:
let
  client = pkgs.stdenv.mkDerivation {
    name = "tomoe-test-client";
    src = ./client.c;
    dontUnpack = true;
    strictDeps = true;
    nativeBuildInputs = [
      pkgs.pkg-config
      pkgs.wayland-scanner
    ];
    buildInputs = [ pkgs.wayland ];
    buildPhase = ''
      for xml in ${pkgs.wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
        ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml; do
        name=$(basename "$xml" .xml)
        wayland-scanner client-header "$xml" "$name-client-protocol.h"
        wayland-scanner private-code "$xml" "$name-protocol.c"
      done
      $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. $src *-protocol.c \
        $(pkg-config --cflags --libs wayland-client) -o tomoe-test-client
    '';
    installPhase = "install -Dm755 tomoe-test-client $out/bin/tomoe-test-client";
  };
  wallpaperSettings = pkgs.writeText "wallpaper-settings.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "check-wallpaper-settings" (:reads () :state nil) (snapshot state event)
      (declare (ignore snapshot event))
      (values state
              (list (publish-state :wallpaper-settings
                                   '(:directory "${pkgs.nixos-icons}/share/icons/hicolor"
                                     :bind ((:mod :shift) "w" :next))))
              nil))
  '';
  check =
    name: script:
    pkgs.testers.runNixOSTest {
      name = "tomoe-${name}";
      nodes.machine = {
        boot.kernelModules = [
          "vgem"
          "udmabuf"
        ];
        hardware.graphics.enable = true;
        environment.systemPackages = [
          tomoe
          client
          pkgs.libnotify
          pkgs.xdpyinfo
        ];
        virtualisation.memorySize = 2048;
        virtualisation.cores = 4;
      };
      testScript = builtins.readFile ./tomoe.py + script;
    };
in
{
  integration = check "integration" (
    ''
      ZOOMER = "${tomoe}/share/tomoe/examples/zoomer.lisp"
    ''
    + builtins.readFile ./integration.py
  );
  bare = check "bare" (builtins.readFile ./bare.py);
  clean-unmount = check "clean-unmount" (
    ''
      WALLPAPER_SETTINGS = "${wallpaperSettings}"
      SOURCES = ["${tomoe}/share/tomoe/desktop.lisp"] + sorted(
          "${tomoe}/share/tomoe/examples/" + name for name in ${builtins.toJSON (builtins.filter (pkgs.lib.hasSuffix ".lisp") (builtins.attrNames (builtins.readDir ../examples)))}
      )
    ''
    + builtins.readFile ./clean-unmount.py
  );
}
