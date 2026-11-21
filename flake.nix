{
  description = "Tomoe, a Lisp-configured Wayland compositor with reloadable policy";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/e52c192be9d7b2c4bd4aed326c8731b35f8bb75c";

  outputs =
    { self, nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      pkgsFor = nixpkgs.lib.genAttrs systems (system: import nixpkgs { inherit system; });
      version = "0.1.0";
      lean =
        pkgs:
        let
          libdrm = pkgs.libdrm.override { withIntel = false; };
          libglvnd = pkgs.libglvnd.overrideAttrs (old: {
            buildInputs = [ pkgs.xorgproto ];
            configureFlags = (old.configureFlags or [ ]) ++ [
              "--disable-x11"
              "--disable-glx"
            ];
          });
          libgbm = (pkgs.libgbm.override { inherit libdrm libglvnd; }).overrideAttrs (old: {
            propagatedBuildInputs = [ ];
            buildInputs = [ libdrm ];
            patches = (old.patches or [ ]) ++ [ ./support/patches/gbm-drm-version.patch ];
            postFixup = (old.postFixup or "") + ''
              lib=$out/lib/libgbm.so.1
              if nm -D --undefined-only $lib | grep -w 'drm[A-Za-z0-9_]*'; then
                echo "libgbm still calls libdrm"
                exit 1
              fi
              patchelf --remove-needed libdrm.so.2 $lib
              patchelf --shrink-rpath $lib
            '';
          });
        in
        {
          inherit
            libdrm
            libgbm
            libglvnd
            ;
        };
      packageFrom =
        root: pkgs:
        {
          cpu ? "",
          opt ? "-O2",
          lto ? false,
          portal ? true,
        }:
        let
          freetype = pkgs.freetype.overrideAttrs (old: {
            propagatedBuildInputs = pkgs.lib.subtractLists [
              pkgs.bzip2
              pkgs.brotli
              pkgs.libpng
            ] old.propagatedBuildInputs;
            patches = (old.patches or [ ]) ++ [ ./support/patches/freetype-png.patch ];
            configureFlags = old.configureFlags ++ [
              "--with-bzip2=no"
              "--with-brotli=no"
              "--with-png=no"
            ];
            NIX_CFLAGS_COMPILE = toString (old.NIX_CFLAGS_COMPILE or "") +
              " -DFT_CONFIG_OPTION_USE_PNG";
          });
          harfbuzz = (pkgs.harfbuzz.override {
            withGraphite2 = false;
            withIntrospection = false;
            inherit freetype;
          }).overrideAttrs (old: {
            buildInputs = pkgs.lib.subtractLists [ pkgs.glib pkgs.cairo pkgs.icu ] (old.buildInputs or [ ]);
            propagatedBuildInputs = pkgs.lib.subtractLists [ pkgs.glib pkgs.cairo pkgs.icu ] (old.propagatedBuildInputs or [ ]);
            mesonFlags = (pkgs.lib.filter (x: !(pkgs.lib.hasPrefix "-Dglib=" x || pkgs.lib.hasPrefix "-Dgobject=" x || pkgs.lib.hasPrefix "-Dcairo=" x || pkgs.lib.hasPrefix "-Dicu=" x)) (old.mesonFlags or [ ])) ++ [
              "-Dpng=disabled"
              "-Dglib=disabled"
              "-Dgobject=disabled"
              "-Dcairo=disabled"
              "-Dicu=disabled"
            ];
          });
          libs = lean pkgs;
          dropIn =
            {
              name,
              pc,
              library,
              version,
              includeSubdir ? "",
              adapter ? null,
            }:
            pkgs.stdenv.mkDerivation {
              pname = "tomoe-${name}";
              inherit version;
              src = ./lib + "/${name}";
              strictDeps = true;
              dontConfigure = true;
              buildPhase = ''
                runHook preBuild
                make -j"$NIX_BUILD_CORES" OUT=build build/libtomoe-${name}.a
                runHook postBuild
              '';
              installPhase = ''
                runHook preInstall
                install -Dm644 build/libtomoe-${name}.a $out/lib/lib${library}.a
                mkdir -p $out/include $out/lib/pkgconfig
                cp -r include/. $out/include/
                ${pkgs.lib.optionalString (adapter != null) "cp -r ${adapter}/. $out/include/"}
                cat > $out/lib/pkgconfig/${pc}.pc <<EOF
                prefix=$out
                libdir=\''${prefix}/lib
                includedir=\''${prefix}/include

                Name: ${pc}
                Description: Tomoe's lib/${name}, in place of ${pc}
                Version: ${version}
                Libs: -L\''${libdir} -l${library}
                Cflags: -I\''${includedir}${includeSubdir}
                EOF
                runHook postInstall
              '';
            };
          mtdev = dropIn {
            name = "mtdev";
            pc = "mtdev";
            library = "mtdev";
            version = "1.1.7";
            adapter = "libinput";
          };
          evdev = dropIn {
            name = "evdev";
            pc = "libevdev";
            library = "evdev";
            version = "1.13.6";
            includeSubdir = "/libevdev-1.0";
          };
          udev = dropIn {
            name = "udev";
            pc = "libudev";
            library = "udev";
            version = "260";
          };
          libinput =
            (pkgs.libinput.override {
              luaSupport = false;
              wacomSupport = false;
              inherit mtdev;
              libevdev = evdev;
              inherit udev;
            }).overrideAttrs
              (old: {
                postPatch = old.postPatch + ''
                  sed -i \
                    -e "/^executable('libinput-device-group',/,/^udev_rules_config = /{/^udev_rules_config = /!d}" \
                    -e "/^############ tools ############/,/^############ output files ############/{/^############ output files/!d}" \
                    meson.build
                  if grep -q "^executable('libinput-\|^############ tools" meson.build; then
                    echo "libinput: the tools are still in meson.build"
                    exit 1
                  fi
                '';
                postInstall = (old.postInstall or "") + ''
                  mkdir -p $bin
                '';
              });
        in
        pkgs.stdenv.mkDerivation {
          pname = "tomoe";
          inherit version;
          src =
            if builtins.isPath root then
              pkgs.lib.fileset.toSource {
                root = root;
                fileset = pkgs.lib.fileset.unions [
                  ./native
                  ./lib
                  ./support
                  ./lisp
                  ./examples
                  ./share
                  ./Makefile
                  ./LICENSE
                  ./NOTICE
                  ./COPYING-LGPL-2.1
                  ./COPYING-MPL-1.1
                ];
              }
            else
              root;
          nativeBuildInputs = [
            pkgs.pkg-config
            pkgs.wayland-scanner
          ];
          buildInputs = [
            libinput
            libs.libglvnd
            libs.libgbm
            pkgs.wayland-protocols
            pkgs.wlr-protocols
            harfbuzz
            freetype
          ];
          strictDeps = true;
          dontStrip = true;
          buildPhase = ''
            runHook preBuild
            make -j"$NIX_BUILD_CORES" VERSION=${version} \
              PATH_PREFIX=$out/bin \
              FONT_DIR=${pkgs.dejavu_fonts.minimal} \
              XKB_CONFIG_ROOT=${pkgs.xkeyboard_config}/etc/X11/xkb \
              OPT='${opt}' LTO='${pkgs.lib.optionalString lto "-flto=auto"}' \
              CPU='${pkgs.lib.optionalString (cpu != "") "-march=${cpu} -mtune=${cpu}"}' \
              WLR=${pkgs.wlr-protocols}/share/wlr-protocols/unstable \
              WP=${pkgs.wayland-protocols}/share/wayland-protocols \
              WAYLAND_XML=${pkgs.wayland-scanner}/share/wayland/wayland.xml \
              build/tomoe build/tomoe-portal
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            install -Dm755 build/tomoe $out/bin/tomoe
            install -d $out/share/tomoe/lisp
            install -m 644 lisp/*.lisp $out/share/tomoe/lisp/
            install -Dm644 -t $out/share/doc/tomoe LICENSE NOTICE COPYING-LGPL-2.1 COPYING-MPL-1.1
            install -Dm644 share/tomoe-session.target $out/share/systemd/user/tomoe-session.target
            install -Dm644 share/tomoe-portals.conf $out/share/xdg-desktop-portal/tomoe-portals.conf
            install -Dm644 share/tomoe.portal $out/share/xdg-desktop-portal/portals/tomoe.portal
            ${pkgs.lib.optionalString portal ''
              install -Dm755 build/tomoe-portal $out/libexec/xdg-desktop-portal-tomoe
              install -d $out/share/dbus-1/services
              printf '[D-BUS Service]\nName=org.freedesktop.impl.portal.desktop.tomoe\nExec=%s/libexec/xdg-desktop-portal-tomoe\n' \
                "$out" > $out/share/dbus-1/services/org.freedesktop.impl.portal.desktop.tomoe.service
            ''}
            install -d $out/share/tomoe/examples
            install -m 644 examples/*.lisp $out/share/tomoe/examples/
            install -Dm644 -t $out/share/tomoe/examples/shaders examples/shaders/*.glsl
            runHook postInstall
          '';
          meta = {
            description = "Lisp-configured window management on a native Wayland stack";
            mainProgram = "tomoe";
            platforms = systems;
          };
        };
      package = packageFrom ./.;
      devShell =
        pkgs:
        pkgs.mkShell {
          inputsFrom = [ (package pkgs { }) ];
          packages = [
            pkgs.wayland-utils
            pkgs.wayland-scanner
            pkgs.wlr-protocols
          ];
          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath [
            pkgs.wayland
          ];
        };
    in
    {
      packages = nixpkgs.lib.mapAttrs (_: pkgs: rec {
        default = package pkgs { };
        tomoe = default;
        znver4 = package pkgs {
          cpu = "znver4";
          opt = "-O3";
          lto = true;
        };
      }) pkgsFor;
      devShells = nixpkgs.lib.mapAttrs (_: pkgs: {
        default = devShell pkgs;
      }) pkgsFor;
      checks = nixpkgs.lib.mapAttrs (_: pkgs: {
        tomoe = package pkgs { };
      }) pkgsFor;
      legacyPackages = nixpkgs.lib.mapAttrs (_: pkgs: {
        bench = import ./bench {
          inherit pkgs;
          tomoe = package pkgs { };
        };
        speedlab = import ./bench/speedlab { inherit pkgs; };
      }) pkgsFor;
      apps = nixpkgs.lib.mapAttrs (
        system: pkgs:
        {
          bench = {
            type = "app";
            program = pkgs.lib.getExe (
              pkgs.writeShellApplication {
                name = "tomoe-bench";
                runtimeInputs = [
                  pkgs.nix
                  pkgs.coreutils
                  pkgs.gnused
                  pkgs.gnugrep
                ];
                text = ''
                  drv=$(nix eval --raw "${self}#legacyPackages.${system}.bench.drvPath")
                  dir=$(mktemp -d)
                  trap 'rm -rf "$dir"' EXIT
                  nix build --no-link --print-build-logs "$drv" >/dev/null 2>"$dir/build" || true
                  log="$dir/build"
                  if nix log "$drv" >"$dir/stored" 2>/dev/null \
                    && grep -q '^BENCH-JSON-BEGIN$' "$dir/stored"; then
                    log="$dir/stored"
                  else
                    sed -E -i 's/^[A-Za-z0-9._+-]+> //' "$log"
                  fi
                  if ! grep -q '^BENCH-JSON-BEGIN$' "$log"; then
                    echo "tomoe-bench: the benchmark produced no scoreboard" >&2
                    tail -80 "$log" >&2
                    exit 1
                  fi
                  sed -n '/^bench: table$/,/^BENCH-JSON-BEGIN$/{/^bench: table$/d;/^BENCH-JSON-BEGIN$/d;p;}' "$log"
                  sed -n '/^BENCH-JSON-BEGIN$/,/^BENCH-JSON-END$/{/^BENCH-JSON-/d;p;}' "$log"
                '';
              }
            );
          };
        }
      ) pkgsFor;
      formatter = nixpkgs.lib.mapAttrs (_: pkgs: pkgs.nixfmt) pkgsFor;
    };
}
