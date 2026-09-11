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
      package =
        pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "tomoe";
          version = "0.1.0";
          src = pkgs.lib.fileset.toSource {
            root = ./.;
            fileset = pkgs.lib.fileset.unions [
              ./native
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
            pkgs.wayland
            pkgs.wayland-protocols
            pkgs.wlr-protocols
            pkgs.wayland-scanner
            pkgs.libxkbcommon
            pkgs.pixman
            # wlroots' Xwayland header includes the XCB window-manager ones.
            pkgs.libxcb
            pkgs.xcbutilwm
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
              $(pkg-config --cflags wlroots-0.20 wayland-server xkbcommon pixman-1 xcb xcb-ewmh xcb-icccm) \
              native/backend.c -o build/libtomoe-backend.so \
              $(pkg-config --libs wlroots-0.20 wayland-server xkbcommon pixman-1)
            sbcl --noinform --non-interactive --load build.lisp
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            install -Dm755 build/tomoe $out/libexec/tomoe
            install -Dm755 build/libtomoe-backend.so $out/lib/libtomoe-backend.so
            install -Dm644 builtins/desktop.lisp $out/share/tomoe/desktop.lisp
            # Shipped policies: a session can seed one as a starting point.
            install -d $out/share/tomoe/examples
            install -m 644 examples/*.lisp $out/share/tomoe/examples/
            makeWrapper $out/libexec/tomoe $out/bin/tomoe \
              --set TOMOE_BACKEND_LIB $out/lib/libtomoe-backend.so \
              --set TOMOE_BUILTINS $out/share/tomoe/desktop.lisp \
              --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.foot ]}
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
      packages = eachSystem (system: {
        default = package nixpkgs.legacyPackages.${system};
      });
      checks = eachSystem (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          tomoe = package pkgs;
          # The end-to-end check needs its driver, fixtures and client together.
          tests = pkgs.lib.fileset.toSource {
            root = ./tests;
            fileset = ./tests;
          };
        in
        {
          integration =
            pkgs.runCommand "tomoe-integration-test"
              {
                nativeBuildInputs = [
                  pkgs.sbcl
                  pkgs.stdenv.cc
                  pkgs.pkg-config
                  pkgs.wayland-scanner
                  pkgs.wayland
                  pkgs.wayland-protocols
                  pkgs.wlr-protocols
                ];
              }
              ''
                # The compositor requires an owned 0700 runtime directory, and the
                # check must not depend on the caller's session.
                export XDG_RUNTIME_DIR="$(mktemp -d "''${TMPDIR:-$NIX_BUILD_TOP}/tomoe-runtime-XXXXXX")"
                chmod 700 "$XDG_RUNTIME_DIR"
                export TOMOE_TEST_RUNTIME_DIR="$XDG_RUNTIME_DIR"
                export TOMOE_TEST_BUILD="$(mktemp -d "''${TMPDIR:-$NIX_BUILD_TOP}/tomoe-client-XXXXXX")"
                export TOMOE_BIN=${tomoe}/bin/tomoe
                export LD_LIBRARY_PATH=${
                  pkgs.lib.makeLibraryPath [
                    pkgs.wayland
                    pkgs.wlroots
                    pkgs.libxkbcommon
                    pkgs.pixman
                  ]
                }
                bash ${tests}/run-integration.sh
                touch $out
              '';
        }
      );
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
            # The compositor dlopens these, so ./dev.sh needs them on the
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
