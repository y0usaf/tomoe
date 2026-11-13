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
      portal =
        pkgs:
        pkgs.rustPlatform.buildRustPackage {
          pname = "xdg-desktop-portal-tomoe";
          inherit version;
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
        in
        pkgs.stdenv.mkDerivation {
          pname = "tomoe";
          inherit version;
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
              ./Makefile
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
            make -j"$NIX_BUILD_CORES" VERSION=${version} SBCL_HOME=${sbcl}/lib/sbcl \
              WLR=${pkgs.wlr-protocols}/share/wlr-protocols/unstable \
              WP=${pkgs.wayland-protocols}/share/wayland-protocols
            SBCL_HOME=${sbcl}/lib/sbcl \
            TOMOE_VERSION=${version} \
            TOMOE_BUILTINS=$out/share/tomoe/desktop.lisp \
            TOMOE_FONTCONFIG_FILE=${pkgs.makeFontsConf { fontDirectories = [ pkgs.dejavu_fonts.minimal ]; }} \
            TOMOE_PATH=$out/bin \
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
      apps = eachSystem (
        system:
        let
          pkgs = pkgsFor system;
        in
        {
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
        }
      );
      formatter = eachSystem (system: (pkgsFor system).nixfmt);
    };
}
