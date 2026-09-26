{
  description = "Tomoe, a Common Lisp Wayland compositor with reloadable policy";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/e52c192be9d7b2c4bd4aed326c8731b35f8bb75c";

  outputs =
    { nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      eachSystem = nixpkgs.lib.genAttrs systems;
      pkgsFor =
        system:
        import nixpkgs { inherit system; };
      pipewire =
        pkgs:
        pkgs.pipewire.overrideAttrs {
          outputs = [
            "out"
            "dev"
          ];
          nativeBuildInputs = [
            pkgs.meson
            pkgs.ninja
            pkgs.pkg-config
            pkgs.python3
          ];
          buildInputs = [ pkgs.dbus ];
          mesonFlags = [
            "-Dauto_features=disabled"
            "-Dexamples=disabled"
            "-Dtests=disabled"
            "-Dpipewire-jack=disabled"
            "-Dpipewire-v4l2=disabled"
            "-Dflatpak=disabled"
            "-Dsession-managers=[]"
            "-Drlimits-install=false"
            "-Dsysconfdir=/etc"
          ];
          postInstall = "";
          doCheck = false;
          doInstallCheck = false;
        };
      portal =
        pkgs:
        pkgs.rustPlatform.buildRustPackage {
          pname = "xdg-desktop-portal-tomoe";
          version = "0.1.0";
          src = ./portal;
          cargoLock.lockFile = ./portal/Cargo.lock;
          nativeBuildInputs = [
            pkgs.rustPlatform.bindgenHook
            pkgs.pkg-config
          ];
          buildInputs = [
            (pipewire pkgs)
            pkgs.libgbm
            pkgs.libdrm
          ];
          doCheck = false;
        };
      package =
        pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "tomoe";
          version = "0.1.0";
          src = pkgs.lib.fileset.toSource {
            root = ./.;
            fileset = pkgs.lib.fileset.unions [
              ./native
              ./support
              ./src
              ./builtins
              ./examples
              ./share
              ./build.lisp
            ];
          };
          nativeBuildInputs = [
            pkgs.pkg-config
            pkgs.sbcl
            pkgs.makeBinaryWrapper
            pkgs.wayland-scanner
          ];
          buildInputs = [
            pkgs.libdrm
            pkgs.libinput
            (pkgs.seatd.override { systemd = pkgs.systemdLibs; })
            pkgs.libGL
            pkgs.libgbm
            pkgs.wayland
            pkgs.wayland-protocols
            pkgs.wlr-protocols
            pkgs.libxkbcommon
            pkgs.pixman
            pkgs.cairo
            pkgs.pango
            pkgs.libjpeg
            pkgs.librsvg
            pkgs.systemdLibs
          ];
          strictDeps = true;
          dontStrip = true;
          buildPhase = ''
            runHook preBuild
            mkdir build
            $CC -std=c11 -Wall -Wextra -Werror -fPIC -shared \
              support/executions.c -o build/libtomoe-executions.so
            $CC -std=c11 -Wall -Wextra -Werror -fPIC -shared \
              support/watches.c -o build/libtomoe-watches.so
            $CC -std=c11 -Wall -Wextra -Werror -fPIC -shared \
              $(pkg-config --cflags libsystemd) support/notifications.c \
              -o build/libtomoe-notifications.so $(pkg-config --libs libsystemd)
            $CC -std=c11 -Wall -Wextra -Werror -fPIC -shared \
              $(pkg-config --cflags libsystemd) support/mpris.c \
              -o build/libtomoe-mpris.so $(pkg-config --libs libsystemd)
            $CC -std=c11 -Wall -Wextra -Werror -fPIC -shared \
              $(pkg-config --cflags libsystemd) support/battery.c \
              -o build/libtomoe-battery.so $(pkg-config --libs libsystemd)
            $CC -std=c11 -Wall -Wextra -Werror -fPIC -shared \
              $(pkg-config --cflags libsystemd) support/network.c \
              -o build/libtomoe-network.so $(pkg-config --libs libsystemd)
            $CC -std=c11 -Wall -Wextra -Werror -fPIC -shared \
              $(pkg-config --cflags libsystemd) support/tray.c \
              -o build/libtomoe-tray.so $(pkg-config --libs libsystemd)
            $CC -std=c11 -Wall -Wextra -Werror support/xwayland.c -o build/tomoe-xwayland
            wlr=${pkgs.wlr-protocols}/share/wlr-protocols/unstable
            wp=${pkgs.wayland-protocols}/share/wayland-protocols
            for xml in \
              $wlr/wlr-layer-shell-unstable-v1.xml \
              $wlr/wlr-screencopy-unstable-v1.xml \
              $wlr/wlr-gamma-control-unstable-v1.xml \
              $wlr/wlr-foreign-toplevel-management-unstable-v1.xml \
              $wlr/wlr-data-control-unstable-v1.xml \
              $wlr/wlr-virtual-pointer-unstable-v1.xml \
              native/virtual-keyboard-unstable-v1.xml \
              native/server-decoration.xml \
              $wp/stable/xdg-shell/xdg-shell.xml \
              $wp/stable/linux-dmabuf/linux-dmabuf-v1.xml \
              $wp/stable/viewporter/viewporter.xml \
              $wp/stable/presentation-time/presentation-time.xml \
              $wp/staging/fractional-scale/fractional-scale-v1.xml \
              $wp/staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml \
              $wp/staging/ext-image-capture-source/ext-image-capture-source-v1.xml \
              $wp/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml \
              $wp/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml \
              $wp/staging/ext-idle-notify/ext-idle-notify-v1.xml \
              $wp/staging/ext-session-lock/ext-session-lock-v1.xml \
              $wp/staging/xdg-activation/xdg-activation-v1.xml \
              $wp/staging/tearing-control/tearing-control-v1.xml \
              $wp/staging/ext-background-effect/ext-background-effect-v1.xml \
              $wp/staging/ext-data-control/ext-data-control-v1.xml \
              $wp/staging/pointer-warp/pointer-warp-v1.xml \
              $wp/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml \
              $wp/unstable/relative-pointer/relative-pointer-unstable-v1.xml \
              $wp/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml \
              $wp/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml \
              $wp/unstable/xdg-output/xdg-output-unstable-v1.xml \
              $wp/unstable/primary-selection/primary-selection-unstable-v1.xml; do
              name=$(basename "$xml" .xml)
              wayland-scanner server-header "$xml" "build/$name-protocol.h"
              wayland-scanner private-code "$xml" "build/$name-protocol.c"
              wayland-scanner client-header "$xml" "build/$name-client-protocol.h"
            done
            $CC -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
              -Wno-unused-parameter -fPIC -shared -Ibuild \
              $(pkg-config --cflags wayland-server xkbcommon pixman-1 pangocairo libjpeg librsvg-2.0 libdrm libinput glesv2 egl gbm libseat libudev wayland-client) \
              native/*.c build/*-protocol.c -o build/libtomoe-backend.so \
              $(pkg-config --libs wayland-server xkbcommon pixman-1 pangocairo libjpeg librsvg-2.0 libdrm libinput glesv2 egl gbm libseat libudev wayland-client) -lm
            sbcl --noinform --non-interactive --load build.lisp
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            install -Dm755 build/tomoe $out/libexec/tomoe
            install -Dm755 build/tomoe-xwayland $out/libexec/tomoe-bin/tomoe-xwayland
            install -Dm755 build/libtomoe-backend.so $out/lib/libtomoe-backend.so
            install -Dm644 builtins/desktop.lisp $out/share/tomoe/desktop.lisp
            install -Dm755 build/libtomoe-executions.so $out/lib/libtomoe-executions.so
            install -Dm755 build/libtomoe-watches.so $out/lib/libtomoe-watches.so
            install -Dm755 build/libtomoe-notifications.so $out/lib/libtomoe-notifications.so
            install -Dm755 build/libtomoe-mpris.so $out/lib/libtomoe-mpris.so
            install -Dm755 build/libtomoe-battery.so $out/lib/libtomoe-battery.so
            install -Dm755 build/libtomoe-network.so $out/lib/libtomoe-network.so
            install -Dm755 build/libtomoe-tray.so $out/lib/libtomoe-tray.so
            install -Dm644 share/tomoe-session.target $out/share/systemd/user/tomoe-session.target
            install -Dm644 share/tomoe-portals.conf $out/share/xdg-desktop-portal/tomoe-portals.conf
            install -Dm644 share/tomoe.portal $out/share/xdg-desktop-portal/portals/tomoe.portal
            install -Dm755 ${portal pkgs}/bin/xdg-desktop-portal-tomoe $out/libexec/xdg-desktop-portal-tomoe
            install -d $out/share/dbus-1/services
            printf '[D-BUS Service]\nName=org.freedesktop.impl.portal.desktop.tomoe\nExec=%s/libexec/xdg-desktop-portal-tomoe\n' \
              "$out" > $out/share/dbus-1/services/org.freedesktop.impl.portal.desktop.tomoe.service
            install -d $out/share/tomoe/examples
            install -m 644 examples/*.lisp $out/share/tomoe/examples/
            makeWrapper $out/libexec/tomoe $out/bin/tomoe \
              --set TOMOE_BACKEND_LIB $out/lib/libtomoe-backend.so \
              --set TOMOE_EXEC_LIB $out/lib/libtomoe-executions.so \
              --set TOMOE_WATCH_LIB $out/lib/libtomoe-watches.so \
              --set TOMOE_NOTIFICATION_LIB $out/lib/libtomoe-notifications.so \
              --set TOMOE_MPRIS_LIB $out/lib/libtomoe-mpris.so \
              --set TOMOE_BATTERY_LIB $out/lib/libtomoe-battery.so \
              --set TOMOE_NETWORK_LIB $out/lib/libtomoe-network.so \
              --set TOMOE_TRAY_LIB $out/lib/libtomoe-tray.so \
              --set TOMOE_SHELL ${pkgs.bashNonInteractive}/bin/sh \
              --set TOMOE_BUILTINS $out/share/tomoe/desktop.lisp \
              --set-default FONTCONFIG_FILE ${pkgs.makeFontsConf { fontDirectories = [ pkgs.dejavu_fonts ]; }} \
              --prefix PATH : $out/libexec/tomoe-bin:${
                pkgs.lib.makeBinPath [
                  pkgs.foot
                  pkgs.fuzzel
                  (pkgs.xwayland-satellite.override {
                    xwayland = pkgs.xwayland.override {
                      libdecor = null;
                      libei = pkgs.libei.override { systemd = pkgs.systemdLibs; };
                    };
                  })
                ]
              }
            runHook postInstall
          '';
          meta = {
            description = "Common Lisp window management on a native Wayland stack";
            mainProgram = "tomoe";
            platforms = systems;
          };
        };
    in
    {
      packages = eachSystem (
        system:
        let
          pkgs = pkgsFor system;
        in
        {
          default = package pkgs;
        }
      );
      devShells = eachSystem (
        system:
        let
          pkgs = pkgsFor system;
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ (package pkgs) ];
            packages = [
              pkgs.wayland-utils
              pkgs.wayland-scanner
              pkgs.wlr-protocols
              pkgs.foot
            ];
            LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath [
              pkgs.wayland
              pkgs.libxkbcommon
              pkgs.pixman
            ];
            WLR_PROTOCOLS_XML = "${pkgs.wlr-protocols}/share/wlr-protocols";
          };
        }
      );
      formatter = eachSystem (system: (pkgsFor system).nixfmt);
    };
}
