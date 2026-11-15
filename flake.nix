{
  description = "Tomoe, a Common Lisp Wayland compositor with reloadable policy";

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
      dbus =
        pkgs:
        pkgs.dbus.override {
          enableSystemd = false;
          x11Support = false;
        };
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
          buildInputs = [ (dbus pkgs) ];
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
          libxkbcommon = pkgs.libxkbcommon.overrideAttrs (old: {
            buildInputs = [
              pkgs.xkeyboard_config
              pkgs.wayland
              pkgs.wayland-protocols
            ];
            mesonFlags = [
              "-Dxkb-config-root=${pkgs.xkeyboard_config}/etc/X11/xkb"
              "-Dxkb-config-extra-path=/etc/xkb"
              "-Denable-docs=true"
              "-Denable-x11=false"
              "-Denable-xkbregistry=false"
              "-Denable-wayland=true"
            ];
          });
          libgbm = (pkgs.libgbm.override { inherit libdrm libglvnd; }).overrideAttrs {
            propagatedBuildInputs = [ ];
            buildInputs = [ libdrm ];
          };
        in
        {
          inherit
            libdrm
            libgbm
            libglvnd
            libxkbcommon
            ;
        };
      baselineSource = builtins.fetchGit {
        url = "https://github.com/y0usaf/tomoe";
        rev = "e3f69434a6e913af5c014d385f293dcf024d10ed";
        narHash = "sha256-j7uXheIewijC7V0FzrVlZhR7xnt3tmID1etcUR5DWHY=";
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
          cairo = pkgs.cairo.override {
            x11Support = false;
            xcbSupport = false;
          };
          pango = pkgs.pango.override {
            x11Support = false;
            inherit cairo;
          };
          libs = lean pkgs;
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
                  ./support
                  ./lisp
                  ./tools
                  ./examples
                  ./share
                  ./Makefile
                ];
              }
            else
              root;
          nativeBuildInputs = [
            pkgs.pkg-config
            pkgs.wayland-scanner
          ];
          buildInputs = [
            libs.libdrm
            libs.libdrm.dev
            (pkgs.libinput.override { luaSupport = false; })
            (pkgs.seatd.override { systemd = pkgs.systemdLibs; })
            libs.libglvnd
            libs.libgbm
            pkgs.wayland
            pkgs.wayland-protocols
            pkgs.wlr-protocols
            libs.libxkbcommon
            cairo
            pango
            pkgs.libjpeg
            (resvg pkgs)
            pkgs.systemdLibs
            pkgs.zstd
            (pipewire pkgs)
          ];
          strictDeps = true;
          dontStrip = true;
          buildPhase = ''
            runHook preBuild
            make -j"$NIX_BUILD_CORES" VERSION=${version} \
              PATH_PREFIX=$out/bin \
              FONTCONFIG_FILE=${pkgs.makeFontsConf { fontDirectories = [ pkgs.dejavu_fonts.minimal ]; }} \
              OPT='${opt}' LTO='${pkgs.lib.optionalString lto "-flto=auto"}' \
              CPU='${pkgs.lib.optionalString (cpu != "") "-march=${cpu} -mtune=${cpu}"}' \
              WLR=${pkgs.wlr-protocols}/share/wlr-protocols/unstable \
              WP=${pkgs.wayland-protocols}/share/wayland-protocols
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            install -Dm755 build/tomoe $out/bin/tomoe
            install -d $out/share/tomoe/lisp
            install -m 644 lisp/*.lisp $out/share/tomoe/lisp/
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
            (lean pkgs).libxkbcommon
          ];
          WLR_PROTOCOLS_XML = "${pkgs.wlr-protocols}/share/wlr-protocols";
        };
      appsFor = pkgs: {
        release = {
          type = "app";
          program = pkgs.lib.getExe (
            pkgs.writeShellApplication {
              name = "tomoe-release";
              runtimeInputs = [
                pkgs.git
                pkgs.gh
                pkgs.gnused
                pkgs.gawk
              ];
              text = builtins.readFile ./release.sh;
            }
          );
        };
      };
    in
    {
      packages = nixpkgs.lib.mapAttrs (_: pkgs: {
        default = package pkgs { };
        znver4 = package pkgs {
          cpu = "znver4";
          opt = "-O3";
          lto = true;
        };
      }) pkgsFor;
      devShells = nixpkgs.lib.mapAttrs (_: pkgs: {
        default = devShell pkgs;
      }) pkgsFor;
      checks = nixpkgs.lib.mapAttrs (
        _: pkgs:
        removeAttrs (import ./tests {
          inherit pkgs;
          tomoe = package pkgs { };
          baseline = packageFrom baselineSource pkgs { portal = false; };
        }) [ "suites" ]
      ) pkgsFor;
      legacyPackages = nixpkgs.lib.mapAttrs (_: pkgs: {
        suites =
          (import ./tests {
            inherit pkgs;
            tomoe = package pkgs { };
            baseline = packageFrom baselineSource pkgs { portal = false; };
          }).suites;
        bench = import ./bench {
          inherit pkgs;
          tomoe = package pkgs { };
        };
      }) pkgsFor;
      apps = nixpkgs.lib.mapAttrs (
        system: pkgs:
        appsFor pkgs
        // {
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
