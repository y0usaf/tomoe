{ pkgs }:
let
  protocols = [
    "${pkgs.wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
    "${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-image-capture-source/ext-image-capture-source-v1.xml"
    "${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml"
    "${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml"
    "${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-virtual-pointer-unstable-v1.xml"
    "${../../native/virtual-keyboard-unstable-v1.xml}"
  ];
  tools = pkgs.stdenv.mkDerivation {
    pname = "tomoe-speedlab-tools";
    version = "1";
    src = pkgs.lib.fileset.toSource {
      root = ./.;
      fileset = pkgs.lib.fileset.fileFilter (file: file.hasExt "c" || file.hasExt "h") ./.;
    };
    nativeBuildInputs = [
      pkgs.pkg-config
      pkgs.wayland-scanner
    ];
    buildInputs = [
      pkgs.wayland
      pkgs.libdrm
    ];
    buildPhase = ''
      runHook preBuild
      protocols=""
      for xml in ${toString protocols}; do
        name=$(basename "$xml" .xml)
        if [[ $name =~ ^[0-9a-z]{32}- ]]; then name=''${name:33}; fi
        wayland-scanner client-header "$xml" "$name-client-protocol.h"
        wayland-scanner private-code "$xml" "$name-protocol.c"
        protocols="$protocols $name-protocol.c"
      done
      flags="-O2 -std=gnu11 -Wall -Wextra -Werror -I. $(pkg-config --cflags libdrm)"
      wl=$(pkg-config --cflags --libs wayland-client)
      $CC $flags -shared -fPIC -o libspeedlab.so vclock.c dumb.c -ldl -lpthread
      $CC $flags -shared -fPIC -o libspeedlab-census.so census.c -ldl -lpthread
      $CC $flags -o speedlab-count count.c
      $CC $flags -o speedlab-input input.c $protocols $wl
      $CC $flags -o speedlab-client client.c $protocols $wl
      $CC $flags -o speedlab-capture capture.c $protocols $wl
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      install -Dm755 libspeedlab.so $out/lib/libspeedlab.so
      install -Dm755 libspeedlab-census.so $out/lib/libspeedlab-census.so
      for tool in speedlab-count speedlab-input speedlab-client speedlab-capture; do
        install -Dm755 $tool $out/bin/$tool
      done
      runHook postInstall
    '';
  };
  python = pkgs.python3;
  path = pkgs.lib.makeBinPath [
    pkgs.bash
    pkgs.coreutils
    pkgs.util-linux
    pkgs.findutils
    pkgs.gnugrep
    pkgs.gnused
    pkgs.binutils
    python
  ];
in
pkgs.runCommand "tomoe-speedlab" { } ''
  mkdir -p $out/bin $out/share/speedlab
  cp ${./driver.py} $out/share/speedlab/driver.py
  cp ${./seal.sh} $out/share/speedlab/seal.sh
  cp ${./workload} $out/share/speedlab/workload
  cp ${./verdict.py} $out/share/speedlab/verdict.py
  cp ${./noise} $out/share/speedlab/noise
  cp ${./report.py} $out/share/speedlab/report.py
  cat > $out/bin/speedlab-env <<EOF
  SPEEDLAB_TOOLS=${tools}
  SPEEDLAB_SHARE=$out/share/speedlab
  SPEEDLAB_MESA=${pkgs.mesa}
  SPEEDLAB_PATH=${path}
  SPEEDLAB_PYTHON=${python}/bin/python3
  SPEEDLAB_BASH=${pkgs.bash}/bin/bash
  SPEEDLAB_PERF=${pkgs.perf}
  EOF
''
