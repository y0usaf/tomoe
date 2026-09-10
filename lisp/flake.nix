{
  description = "Tomoe Lisp, a Common Lisp Wayland compositor with reloadable policy";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/e52c192be9d7b2c4bd4aed326c8731b35f8bb75c";

  outputs =
    { nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      eachSystem = nixpkgs.lib.genAttrs systems;
      package =
        pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "tomoe-lisp";
          version = "0.1.0";
          src = pkgs.lib.fileset.toSource {
            root = ./.;
            fileset = pkgs.lib.fileset.unions [
              ./native
              ./src
              ./builtins
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
            pkgs.wayland
            pkgs.wayland-protocols
            pkgs.wlr-protocols
            pkgs.wayland-scanner
            pkgs.libxkbcommon
            pkgs.pixman
          ];
          strictDeps = true;
          # strip discards the Lisp image appended to the SBCL executable.
          dontStrip = true;
          buildPhase = ''
            runHook preBuild
            mkdir build
            # nixpkgs' wlroots ships no generated protocol headers, but includes
            # wlr-layer-shell-unstable-v1-protocol.h from wlr/types.
            ${pkgs.lib.getBin pkgs.wayland-scanner}/bin/wayland-scanner server-header \
              ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml \
              build/wlr-layer-shell-unstable-v1-protocol.h
            $CC -std=c11 -D_GNU_SOURCE -DWLR_USE_UNSTABLE -Wall -Wextra -Werror \
              -Wno-unused-parameter -fPIC -shared -Ibuild \
              -I$(pkg-config --variable=includedir wayland-protocols) \
              $(pkg-config --cflags wlroots-0.20 wayland-server xkbcommon pixman-1) \
              native/backend.c -o build/libtomoe-backend.so \
              $(pkg-config --libs wlroots-0.20 wayland-server xkbcommon pixman-1)
            sbcl --noinform --non-interactive --load build.lisp
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            install -Dm755 build/tomoe-lisp $out/libexec/tomoe-lisp
            install -Dm755 build/libtomoe-backend.so $out/lib/libtomoe-backend.so
            install -Dm644 builtins/desktop.lisp $out/share/tomoe-lisp/desktop.lisp
            makeWrapper $out/libexec/tomoe-lisp $out/bin/tomoe-lisp \
              --set TOMOE_LISP_BACKEND $out/lib/libtomoe-backend.so \
              --set TOMOE_LISP_BUILTINS $out/share/tomoe-lisp/desktop.lisp \
              --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.foot ]}
            runHook postInstall
          '';
          meta = {
            description = "Common Lisp window management on wlroots";
            mainProgram = "tomoe-lisp";
            platforms = systems;
          };
        };
    in
    {
      packages = eachSystem (system: {
        default = package nixpkgs.legacyPackages.${system};
      });
      devShells = eachSystem (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
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
            # The compositor dlopens these, so ../../dev.sh needs them on the
            # library path rather than only at link time.
            LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath [
              pkgs.wlroots
              pkgs.wayland
              pkgs.libxkbcommon
              pkgs.pixman
            ];
            # dev.sh and tests/build-client.sh read the layer-shell protocol
            # XML from here when pkg-config cannot find wlr-protocols.
            WLR_PROTOCOLS_XML = "${pkgs.wlr-protocols}/share/wlr-protocols";
          };
        }
      );
      formatter = eachSystem (system: nixpkgs.legacyPackages.${system}.nixfmt);
    };
}
