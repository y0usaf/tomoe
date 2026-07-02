{
  description = "takhti — a Wayland compositor built with Smithay and embedded Lua";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;
      systems = lib.intersectLists lib.systems.flakeExposed lib.platforms.linux;
      forAllSystems = lib.genAttrs systems;
      nixpkgsFor = forAllSystems (system: nixpkgs.legacyPackages.${system});

      # RUSTFLAGS needed for dlopen() to find EGL/wayland-client at runtime.
      # Pattern taken from niri's flake.nix.
      devRustflags = toString (
        map (arg: "-C link-arg=" + arg) [
          "-Wl,--push-state,--no-as-needed"
          "-lEGL"
          "-lwayland-client"
          "-Wl,--pop-state"
        ]
      );

      takhti-package =
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
        }:
        rustPlatform.buildRustPackage {
          pname = "takhti";
          version = "0.1.0";

          src = lib.fileset.toSource {
            root = ./.;
            fileset = lib.fileset.unions [
              ./crates
              ./resources
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
          ];

          env = {
            RUSTFLAGS = devRustflags;
          };

          meta = {
            description = "Wayland compositor with Smithay + embedded Lua";
            license = lib.licenses.gpl3Only;
            mainProgram = "takhti";
            platforms = lib.platforms.linux;
          };
        };
    in
    {
      packages = forAllSystems (system:
        let pkgs = nixpkgsFor.${system};
        in {
          default = pkgs.callPackage takhti-package { };
          takhti = pkgs.callPackage takhti-package { };
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
          ];
        in
        {
          default = pkgs.mkShell {
            packages = [
              pkgs.rustc
              pkgs.cargo
              pkgs.clippy
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
              RUST_LOG = "takhti=debug";
            };
          };
        }
      );

      formatter = forAllSystems (system: nixpkgsFor.${system}.nixfmt-rfc-style);
    };
}
