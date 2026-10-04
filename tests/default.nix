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
        ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml \
        ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-virtual-pointer-unstable-v1.xml; do
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
  screenshotProbe = pkgs.writeText "screenshot-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "screenshot-probe" (:reads (:key) :state nil) (snapshot state event)
      (declare (ignore snapshot))
      (values state
              (list (bind-key '(:super) "F12" :select))
              (when (and (eq (getf event :type) :key) (equal (getf event :owner) "screenshot-probe"))
                (list (screenshot)))))
  '';
  offscreenProbe = pkgs.writeText "offscreen-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "offscreen-probe" (:reads (:windows) :state nil) (snapshot state event)
      (declare (ignore state event))
      (values nil
              (loop for window in (context snapshot :windows)
                    when (equal (getf window :app-id) "check-flood")
                      collect (place (getf window :id) 20000 20000 320 240))
              nil))
  '';
  virtualProbe = pkgs.writeText "virtual-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "virtual-probe" (:reads () :state nil) (snapshot state event)
      (declare (ignore snapshot event))
      (values state
              (list (virtual-output "VIRTUAL-1" :mode '(1280 720))
                    (virtual-output "VIRTUAL-2" :mode '(1280 720))
                    (shell-surface :rows
                                   (ui :column :children (loop for i below 500
                                                               collect (ui :text :text (format nil "~D" i))))
                                   :anchors '(:top :left) :layer :overlay))
              nil))
  '';
  busProbe = pkgs.writeText "bus-probe.lisp" ''
    (in-package #:tomoe-user)
    (define-extension "bus-probe" (:reads () :state nil) (snapshot state event)
      (declare (ignore snapshot event))
      (values state
              (list (run-once :probe "printf %s \"$DBUS_SESSION_BUS_ADDRESS\" > /run/bus-probe"))
              nil))
  '';
  checkWith =
    extra: name: script:
    pkgs.testers.runNixOSTest {
      name = "tomoe-${name}";
      nodes.machine = {
        imports = [ extra ];
        boot.kernelModules = [
          "vgem"
          "udmabuf"
        ];
        hardware.graphics.enable = true;
        environment.systemPackages = [
          tomoe
          client
          pkgs.grim
          pkgs.libnotify
          pkgs.xdpyinfo
        ];
        virtualisation.memorySize = 2048;
        virtualisation.cores = 4;
      };
      testScript = builtins.readFile ./tomoe.py + script;
    };
  check = checkWith { };
in
{
  integration = check "integration" (
    ''
      ZOOMER = "${tomoe}/share/tomoe/examples/zoomer.lisp"
      SCREENSHOT_PROBE = "${screenshotProbe}"
      OFFSCREEN_PROBE = "${offscreenProbe}"
    ''
    + builtins.readFile ./integration.py
  );
  bare = check "bare" (
    ''
      VIRTUAL_PROBE = "${virtualProbe}"
    ''
    + builtins.readFile ./bare.py
  );
  portal =
    checkWith
      {
        environment.systemPackages = [
          pkgs.pipewire
          pkgs.xdg-desktop-portal
        ];
        environment.pathsToLink = [
          "/share/dbus-1"
          "/share/xdg-desktop-portal"
        ];
      }
      "portal"
      (
        ''
          CAST = "${./cast.py}"
          CAST_PYTHON = "${
            pkgs.python3.withPackages (p: [
              p.dbus-python
              p.pygobject3
            ])
          }/bin/python3"
          PIPEWIRE_CONFIG = "${pkgs.pipewire}/share/pipewire"
        ''
        + builtins.readFile ./portal.py
      );
  session-bus = check "session-bus" (
    ''
      BUS_PROBE = "${busProbe}"
    ''
    + builtins.readFile ./session-bus.py
  );
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
