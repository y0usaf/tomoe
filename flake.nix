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
      pkgsFor = system: import nixpkgs { inherit system; };
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
          buildInputs = [
            (pkgs.dbus.override {
              enableSystemd = false;
              x11Support = false;
            })
          ];
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
      resvg =
        pkgs:
        pkgs.runCommand "resvg-${pkgs.resvg.version}-lib" { } ''
          mkdir -p $out/lib
          cp -r ${pkgs.resvg}/include $out
          cp ${pkgs.resvg}/lib/libresvg.so $out/lib
        '';
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
        let
          sbcl = pkgs.sbcl.overrideAttrs { markRegionGC = false; };
          xwayland-satellite = pkgs.xwayland-satellite.override {
            xwayland =
              (pkgs.xwayland.override {
                bash = pkgs.bashNonInteractive;
                openssl = pkgs.libmd;
                libdecor = null;
                libtirpc = null;
                libei = pkgs.libei.override { systemd = pkgs.systemdLibs; };
              }).overrideAttrs
                (old: {
                  mesonFlags = old.mesonFlags ++ [
                    "-Dsha1=libmd"
                    "-Dsecure-rpc=false"
                  ];
                });
          };
        in
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
            sbcl
            pkgs.wayland-scanner
          ];
          buildInputs = [
            pkgs.libdrm
            (pkgs.libinput.override { luaSupport = false; })
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
            (resvg pkgs)
            pkgs.systemdLibs
            pkgs.lcms2
            pkgs.zstd
            (pipewire pkgs)
          ];
          strictDeps = true;
          dontStrip = true;
          buildPhase = ''
            runHook preBuild
            mkdir build
            for module in executions watches notifications mpris backlight battery network tray system xwayland; do
              $CC -std=c11 -Wall -Wextra -Werror $(pkg-config --cflags libsystemd) \
                -c support/$module.c -o build/$module.o
            done
            wlr=${pkgs.wlr-protocols}/share/wlr-protocols/unstable
            wp=${pkgs.wayland-protocols}/share/wayland-protocols
            for xml in \
              $wlr/wlr-layer-shell-unstable-v1.xml \
              $wlr/wlr-screencopy-unstable-v1.xml \
              $wlr/wlr-gamma-control-unstable-v1.xml \
              $wlr/wlr-output-power-management-unstable-v1.xml \
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
              -Wno-unused-parameter -Ibuild -Wl,--export-dynamic \
              $(pkg-config --cflags wayland-server xkbcommon pixman-1 pangocairo libjpeg libdrm libinput glesv2 egl gbm libseat libudev wayland-client lcms2 libpipewire-0.3) \
              ${sbcl}/lib/sbcl/sbcl.o native/*.c build/*-protocol.c build/*.o -o build/tomoe-runtime \
              $(pkg-config --libs wayland-server xkbcommon pixman-1 pangocairo libjpeg libdrm libinput glesv2 egl gbm libseat libudev wayland-client lcms2 libpipewire-0.3 libsystemd) \
              -lresvg -ldl -lpthread -lzstd -lm
            SBCL_HOME=${sbcl}/lib/sbcl \
            TOMOE_SHELL=${pkgs.bashNonInteractive}/bin/sh \
            TOMOE_BUILTINS=$out/share/tomoe/desktop.lisp \
            TOMOE_FONTCONFIG_FILE=${pkgs.makeFontsConf { fontDirectories = [ pkgs.dejavu_fonts.minimal ]; }} \
            TOMOE_PATH=$out/bin:${
              pkgs.lib.makeBinPath [
                pkgs.foot
                (pkgs.fuzzel.override { resvg = resvg pkgs; })
                xwayland-satellite
              ]
            } \
              build/tomoe-runtime --core ${sbcl}/lib/sbcl/sbcl.core --noinform \
              --non-interactive --no-userinit --load build.lisp
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            install -Dm755 build/tomoe $out/bin/tomoe
            install -Dm644 builtins/desktop.lisp $out/share/tomoe/desktop.lisp
            install -Dm644 share/tomoe-session.target $out/share/systemd/user/tomoe-session.target
            install -Dm644 share/tomoe-portals.conf $out/share/xdg-desktop-portal/tomoe-portals.conf
            install -Dm644 share/tomoe.portal $out/share/xdg-desktop-portal/portals/tomoe.portal
            install -Dm755 ${portal pkgs}/bin/xdg-desktop-portal-tomoe $out/libexec/xdg-desktop-portal-tomoe
            install -d $out/share/dbus-1/services
            printf '[D-BUS Service]\nName=org.freedesktop.impl.portal.desktop.tomoe\nExec=%s/libexec/xdg-desktop-portal-tomoe\n' \
              "$out" > $out/share/dbus-1/services/org.freedesktop.impl.portal.desktop.tomoe.service
            install -d $out/share/tomoe/examples
            install -m 644 examples/*.lisp $out/share/tomoe/examples/
            install -Dm644 -t $out/share/tomoe/examples/shaders examples/shaders/*.glsl
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
      checks = eachSystem (
        system:
        let
          pkgs = pkgsFor system;
        in
        import ./tests {
          inherit pkgs;
          tomoe = package pkgs;
        }
      );
      formatter = eachSystem (system: (pkgsFor system).nixfmt);
    };
}
