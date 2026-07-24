{
  description = "tomoe — a Wayland compositor built with Smithay and embedded Lua";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;
      systems = lib.intersectLists lib.systems.flakeExposed lib.platforms.linux;
      forAllSystems = lib.genAttrs systems;
      nixpkgsFor = forAllSystems (system: nixpkgs.legacyPackages.${system});

      # RUSTFLAGS needed for dlopen() to find EGL/wayland-client at runtime.
      devRustflags = toString (
        map (arg: "-C link-arg=" + arg) [
          "-Wl,--push-state,--no-as-needed"
          "-lEGL"
          "-lwayland-client"
          "-Wl,--pop-state"
        ]
      );

      tomoe-package =
        {
          lib,
          pkg-config,
          rustPlatform,
          libGL,
          libdisplay-info,
          libinput,
          seatd,
          libxkbcommon,
          libgbm,
          wayland,
          systemd,        # provides libudev
          dbus,
          pipewire,       # xdg-desktop-portal-tomoe links libpipewire-0.3
        }:
        rustPlatform.buildRustPackage {
          pname = "tomoe";
          version = "0.1.0";

          src = lib.fileset.toSource {
            root = ./.;
            fileset = lib.fileset.unions [
              ./crates
              # resources/ also carries the moonshell `ui.*` stdlib
              # (resources/moonshell/), include_str!'d by moonshell-runtime.
              ./resources
              # simple-bar acceptance fixture, include_str!'d by
              # moonshell-runtime tests.
              ./examples
              # The docgen test compares docs/lua-api.md against what the
              # sources generate; without it in the sandbox the test always
              # reads an empty file and fails as "stale".
              ./docs
              ./Cargo.toml
              ./Cargo.lock
            ];
          };

          cargoLock = {
            allowBuiltinFetchGit = true;
            lockFile = ./Cargo.lock;
          };

          strictDeps = true;

          nativeBuildInputs = [
            rustPlatform.bindgenHook
            pkg-config
          ];

          buildInputs = [
            libGL
            libdisplay-info
            libinput
            seatd
            libxkbcommon
            libgbm
            wayland
            systemd   # libudev
            dbus
            pipewire
          ];

          env = {
            RUSTFLAGS = devRustflags;
          };

          # tomoe-session.target is how the compositor activates
          # graphical-session.target (which refuses manual start): starting
          # the session target pulls it up via BindsTo.
          # Portal discovery: the .portal file tells xdg-desktop-portal the
          # backend exists, portals.conf routes ScreenCast to it under
          # XDG_CURRENT_DESKTOP=tomoe, and the D-Bus service file lets the
          # bus activate the binary on demand.
          postInstall = ''
            install -Dm644 resources/tomoe-session.target \
              $out/share/systemd/user/tomoe-session.target
            install -Dm644 resources/tomoe.portal \
              $out/share/xdg-desktop-portal/portals/tomoe.portal
            install -Dm644 resources/tomoe-portals.conf \
              $out/share/xdg-desktop-portal/tomoe-portals.conf
            install -d $out/share/dbus-1/services
            printf '[D-BUS Service]\nName=org.freedesktop.impl.portal.desktop.tomoe\nExec=%s/bin/xdg-desktop-portal-tomoe\n' \
              "$out" > $out/share/dbus-1/services/org.freedesktop.impl.portal.desktop.tomoe.service
          '';

          meta = {
            description = "Wayland compositor with Smithay + embedded Lua";
            license = lib.licenses.agpl3Plus;
            mainProgram = "tomoe";
            platforms = lib.platforms.linux;
          };
        };
    in
    {
      packages = forAllSystems (system:
        let pkgs = nixpkgsFor.${system};
        in {
          default = pkgs.callPackage tomoe-package { };
          tomoe = pkgs.callPackage tomoe-package { };
        }
      );

      devShells = forAllSystems (system:
        let
          pkgs = nixpkgsFor.${system};
          # Build inputs shared between the package and devShell
          buildInputs = [
            pkgs.libGL
            pkgs.libdisplay-info
            pkgs.libinput
            pkgs.seatd
            pkgs.libxkbcommon
            pkgs.libgbm
            pkgs.wayland
            pkgs.systemd   # libudev
            pkgs.dbus
            pkgs.pipewire
          ];
        in
        {
          default = pkgs.mkShell {
            packages = [
              pkgs.rustc
              pkgs.cargo
              pkgs.cargo-modules
              pkgs.clippy
              pkgs.jq
              pkgs.rustfmt
              pkgs.rust-analyzer
            ];

            nativeBuildInputs = [
              pkgs.rustPlatform.bindgenHook
              pkgs.pkg-config
            ];

            inherit buildInputs;

            env = {
              # Required for dlopen() to find EGL and wayland-client at runtime.
              RUSTFLAGS = devRustflags;
              RUST_LOG = "tomoe=debug";
            };
          };
        }
      );

      # ARCHITECTURE.md is generated by scripts/gen-arch.sh; this check
      # regenerates it in the sandbox and fails if the committed copy is stale.
      # cargo-modules runs the workspace's build scripts (bindgen etc.), so the
      # check needs the same native inputs as the package build.
      #
      # fmt / clippy / boot are moonshell's flake checks, folded in at
      # FUSION.md F0. `boot` gates the transitional standalone moonshell
      # binary (doctrine 06 + the M0 RSS budget) and is deleted with that
      # binary at F6.
      checks = forAllSystems (system:
        let
          pkgs = nixpkgsFor.${system};
          tomoe = self.packages.${system}.default;
          nativeCheckInputs = [
            pkgs.rustPlatform.cargoSetupHook
            pkgs.rustPlatform.bindgenHook
            pkgs.pkg-config
            pkgs.cargo
            pkgs.rustc
          ];
          checkBuildInputs = [
            pkgs.libGL
            pkgs.libdisplay-info
            pkgs.libinput
            pkgs.seatd
            pkgs.libxkbcommon
            pkgs.libgbm
            pkgs.wayland
            pkgs.systemd
            pkgs.dbus
            pkgs.pipewire
          ];
          cargoDeps = pkgs.rustPlatform.importCargoLock {
            allowBuiltinFetchGit = true;
            lockFile = ./Cargo.lock;
          };
        in
        {
          # Build + unit tests for the whole workspace (buildRustPackage
          # runs `cargo test`): tomoe, the portal, and moonshell.
          build = tomoe;

          fmt =
            pkgs.runCommand "tomoe-fmt"
              {
                nativeBuildInputs = [
                  pkgs.cargo
                  pkgs.rustfmt
                ];
              }
              ''
                cd ${self}
                cargo fmt --check
                touch $out
              '';

          clippy = pkgs.stdenv.mkDerivation {
            name = "tomoe-clippy";
            src = tomoe.src;
            nativeBuildInputs = nativeCheckInputs ++ [ pkgs.clippy ];
            buildInputs = checkBuildInputs;
            inherit cargoDeps;
            env.RUSTFLAGS = devRustflags;
            buildPhase = ''
              export HOME=$TMPDIR
              cargo clippy --workspace --all-targets -- -D warnings
              touch $out
            '';
            dontInstall = true;
          };

          # Doctrine 06: the bare, no-config moonshell binary must boot —
          # map a layer surface, draw, commit — under a headless
          # compositor. Also gates idle RSS at the M0 budget (20 MB).
          # Retired at F6 together with the standalone binary.
          boot =
            pkgs.runCommand "moonshell-boot"
              {
                # Unwrapped: the sway wrapper execs via dbus-run-session,
                # and there is no session bus config in the sandbox.
                nativeBuildInputs = [ pkgs.sway-unwrapped ];
              }
              ''
                export XDG_RUNTIME_DIR=$(mktemp -d)
                chmod 700 $XDG_RUNTIME_DIR
                export WLR_BACKENDS=headless
                export WLR_RENDERER=pixman
                export WLR_LIBINPUT_NO_DEVICES=1

                # fontdb honors FONTCONFIG_FILE; give the sandbox one real
                # font so the text path is exercised, not skipped.
                printf '%s\n' \
                  '<?xml version="1.0"?>' \
                  '<fontconfig>' \
                  "  <dir>${pkgs.dejavu_fonts}/share/fonts</dir>" \
                  "  <cachedir>$XDG_RUNTIME_DIR/fc-cache</cachedir>" \
                  '</fontconfig>' > fonts.conf
                export FONTCONFIG_FILE=$PWD/fonts.conf

                touch empty-config
                sway -c empty-config &
                for _ in $(seq 100); do
                  [ -S $XDG_RUNTIME_DIR/wayland-1 ] && break
                  sleep 0.1
                done
                [ -S $XDG_RUNTIME_DIR/wayland-1 ] || { echo "headless sway never came up" >&2; exit 1; }
                export WAYLAND_DISPLAY=wayland-1

                # Boot: first frame committed => exit 0.
                timeout 30 ${tomoe}/bin/moonshell --boot-check

                # RSS gate: idle for 3s, then check against the M0 budget.
                ${tomoe}/bin/moonshell &
                ms=$!
                sleep 3
                rss_kb=$(awk '/^VmRSS/{print $2}' /proc/$ms/status)
                kill $ms
                echo "idle RSS: ''${rss_kb} kB (budget 20480 kB)"
                [ "$rss_kb" -le 20480 ] || { echo "RSS over M0 budget" >&2; exit 1; }
                touch $out
              '';

          arch-fresh = pkgs.stdenv.mkDerivation {
            name = "arch-fresh";
            src = self;
            nativeBuildInputs = [
              pkgs.rustPlatform.cargoSetupHook
              pkgs.rustPlatform.bindgenHook
              pkgs.pkg-config
              pkgs.cargo
              pkgs.rustc
              pkgs.jq
              pkgs.cargo-modules
            ];
            buildInputs = [
              pkgs.libGL
              pkgs.libdisplay-info
              pkgs.libinput
              pkgs.seatd
              pkgs.libxkbcommon
              pkgs.libgbm
              pkgs.wayland
              pkgs.systemd
              pkgs.dbus
              pkgs.pipewire
            ];
            cargoDeps = pkgs.rustPlatform.importCargoLock {
              allowBuiltinFetchGit = true;
              lockFile = ./Cargo.lock;
            };
            buildPhase = ''
              export HOME=$TMPDIR
              cp ARCHITECTURE.md $TMPDIR/committed.md
              bash scripts/gen-arch.sh
              diff -u $TMPDIR/committed.md ARCHITECTURE.md || {
                echo 'ARCHITECTURE.md is stale — run scripts/gen-arch.sh and commit the result.' >&2
                exit 1
              }
              touch $out
            '';
            dontInstall = true;
          };
        }
      );

      formatter = forAllSystems (system: nixpkgsFor.${system}.nixfmt-rfc-style);
    };
}
