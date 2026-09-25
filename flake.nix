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
            ./patches/wlroots-xwm-queued-events.patch
            ./patches/wlroots-screencopy-buffer.patch
          ];
        });
      };
      pkgsFor =
        system:
        import nixpkgs {
          inherit system;
          overlays = [ wlrootsOverlay ];
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
            pkgs.libxcb
            pkgs.xcbutilwm
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
            $CC -std=c11 -D_GNU_SOURCE -DWLR_USE_UNSTABLE -Wall -Wextra -Werror \
              -Wno-unused-parameter -fPIC -shared -Ibuild \
              -I$(pkg-config --variable=includedir wayland-protocols) \
              $(pkg-config --cflags wlroots-0.20 wayland-server xkbcommon pixman-1 pangocairo libpng libjpeg librsvg-2.0 libdrm libinput xcb xcb-ewmh xcb-icccm) \
              native/*.c -o build/libtomoe-backend.so \
              $(pkg-config --libs wlroots-0.20 wayland-server xkbcommon pixman-1 pangocairo libpng libjpeg librsvg-2.0 libinput) -lm
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
              --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.foot pkgs.fuzzel ]}
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
          };
        }
      );
      formatter = eachSystem (system: (pkgsFor system).nixfmt);
    };
}
