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
      wlrootsOverlay = final: prev: {
        wlroots = prev.wlroots.overrideAttrs (old: {
          patches = (old.patches or [ ]) ++ [
            ./patches/wlroots-keyboard-cap.patch
            ./patches/wlroots-modifier-input.patch
          ];
        });
      };
      pkgsFor =
        system:
        import nixpkgs {
          inherit system;
          overlays = [ wlrootsOverlay ];
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
            pkgs.pipewire
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
            pkgs.makeWrapper
          ];
          buildInputs = [
            pkgs.wlroots
            pkgs.libdrm
            pkgs.libinput
            pkgs.libGL
            pkgs.libgbm
            pkgs.wayland
            pkgs.wayland-protocols
            pkgs.wlr-protocols
            pkgs.wayland-scanner
            pkgs.libxkbcommon
            pkgs.pixman
            pkgs.cairo
            pkgs.pango
            pkgs.libpng
            pkgs.libjpeg
            pkgs.librsvg
            pkgs.systemd
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
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner server-header \
              ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml \
              build/wlr-layer-shell-unstable-v1-protocol.h
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner server-header \
              ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-screencopy-unstable-v1.xml \
              build/wlr-screencopy-unstable-v1-protocol.h
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner private-code \
              ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-screencopy-unstable-v1.xml \
              build/wlr-screencopy-unstable-v1-protocol.c
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner server-header \
              ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-gamma-control-unstable-v1.xml \
              build/wlr-gamma-control-unstable-v1-protocol.h
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner private-code \
              ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-gamma-control-unstable-v1.xml \
              build/wlr-gamma-control-unstable-v1-protocol.c
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner server-header \
              ${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-image-capture-source/ext-image-capture-source-v1.xml \
              build/ext-image-capture-source-v1-protocol.h
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner private-code \
              ${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-image-capture-source/ext-image-capture-source-v1.xml \
              build/ext-image-capture-source-v1-protocol.c
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner server-header \
              ${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml \
              build/ext-image-copy-capture-v1-protocol.h
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner private-code \
              ${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml \
              build/ext-image-copy-capture-v1-protocol.c
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner private-code \
              ${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml \
              build/ext-foreign-toplevel-list-v1-protocol.c
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner server-header \
              ${pkgs.wayland-protocols}/share/wayland-protocols/staging/xdg-activation/xdg-activation-v1.xml \
              build/xdg-activation-v1-protocol.h
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner private-code \
              ${pkgs.wayland-protocols}/share/wayland-protocols/staging/xdg-activation/xdg-activation-v1.xml \
              build/xdg-activation-v1-protocol.c
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner server-header \
              ${pkgs.wayland-protocols}/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml \
              build/xdg-decoration-unstable-v1-protocol.h
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner private-code \
              ${pkgs.wayland-protocols}/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml \
              build/xdg-decoration-unstable-v1-protocol.c
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner server-header \
              ${pkgs.kdePackages.plasma-wayland-protocols}/share/plasma-wayland-protocols/server-decoration.xml \
              build/server-decoration-protocol.h
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner private-code \
              ${pkgs.kdePackages.plasma-wayland-protocols}/share/plasma-wayland-protocols/server-decoration.xml \
              build/server-decoration-protocol.c
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner private-code \
              ${pkgs.wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
              build/xdg-shell-protocol.c
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner server-header \
              ${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml \
              build/ext-foreign-toplevel-list-v1-protocol.h
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner server-header \
              ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-foreign-toplevel-management-unstable-v1.xml \
              build/wlr-foreign-toplevel-management-unstable-v1-protocol.h
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner private-code \
              ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-foreign-toplevel-management-unstable-v1.xml \
              build/wlr-foreign-toplevel-management-unstable-v1-protocol.c
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner server-header \
              ${pkgs.wayland-protocols}/share/wayland-protocols/staging/tearing-control/tearing-control-v1.xml \
              build/tearing-control-v1-protocol.h
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner private-code \
              ${pkgs.wayland-protocols}/share/wayland-protocols/staging/tearing-control/tearing-control-v1.xml \
              build/tearing-control-v1-protocol.c
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner server-header \
              ${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-background-effect/ext-background-effect-v1.xml \
              build/ext-background-effect-v1-protocol.h
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner private-code \
              ${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-background-effect/ext-background-effect-v1.xml \
              build/ext-background-effect-v1-protocol.c
            $CC -std=c11 -D_GNU_SOURCE -DWLR_USE_UNSTABLE -Wall -Wextra -Werror \
              -Wno-unused-parameter -fPIC -shared -Ibuild \
              -I$(pkg-config --variable=includedir wayland-protocols) \
              $(pkg-config --cflags wlroots-0.20 wayland-server xkbcommon pixman-1 pangocairo libpng libjpeg librsvg-2.0 libdrm libinput glesv2 egl gbm) \
              native/*.c build/*-protocol.c -o build/libtomoe-backend.so \
              $(pkg-config --libs wlroots-0.20 wayland-server xkbcommon pixman-1 pangocairo libpng libjpeg librsvg-2.0 libdrm libinput glesv2 egl gbm) -lm
            sbcl --noinform --non-interactive --load build.lisp
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            install -Dm755 build/tomoe $out/libexec/tomoe
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
              --set TOMOE_SHELL ${pkgs.bash}/bin/sh \
              --set TOMOE_BUILTINS $out/share/tomoe/desktop.lisp \
              --set-default FONTCONFIG_FILE ${pkgs.makeFontsConf { fontDirectories = [ pkgs.dejavu_fonts ]; }} \
              --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.foot pkgs.fuzzel pkgs.xwayland-satellite ]}
            runHook postInstall
          '';
          meta = {
            description = "Common Lisp window management on wlroots";
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
              pkgs.wlroots
              pkgs.wayland
              pkgs.libxkbcommon
              pkgs.pixman
            ];
            WLR_PROTOCOLS_XML = "${pkgs.wlr-protocols}/share/wlr-protocols";
            PLASMA_WAYLAND_PROTOCOLS_XML = "${pkgs.kdePackages.plasma-wayland-protocols}/share/plasma-wayland-protocols";
          };
        }
      );
      formatter = eachSystem (system: (pkgsFor system).nixfmt);
    };
}
