{ pkgs }:
pkgs.stdenv.mkDerivation {
  name = "tomoe-sound-pipewire-check";
  src = pkgs.lib.fileset.toSource {
    root = ./..;
    fileset = pkgs.lib.fileset.unions [
      ../lib/pipewire
      ../lib/wayland/include
      ../lib/wayland/scanner
      ../lib/xkb/include
      ../native
      ./e2e-sound.c
    ];
  };
  strictDeps = true;
  nativeBuildInputs = [
    pkgs.pkg-config
    pkgs.wayland-scanner
  ];
  buildInputs = [
    pkgs.libdrm
    pkgs.wayland-protocols
    pkgs.wlr-protocols
  ];
  buildPhase = ''
    runHook preBuild
    mkdir -p gen
    WLR=${pkgs.wlr-protocols}/share/wlr-protocols/unstable
    WP=${pkgs.wayland-protocols}/share/wayland-protocols
    WAYLAND_XML=${pkgs.wayland-scanner}/share/wayland/wayland.xml
    wayland-scanner -c server-header "$WAYLAND_XML" gen/wayland-server-protocol.h
    wayland-scanner server-header "$WLR/wlr-layer-shell-unstable-v1.xml" gen/wlr-layer-shell-unstable-v1-protocol.h
    wayland-scanner server-header "$WP/stable/xdg-shell/xdg-shell.xml" gen/xdg-shell-protocol.h
    wayland-scanner server-header "$WP/staging/xdg-system-bell/xdg-system-bell-v1.xml" gen/xdg-system-bell-v1-protocol.h
    wayland-scanner private-code "$WP/staging/xdg-system-bell/xdg-system-bell-v1.xml" gen/xdg-system-bell-v1-protocol.c
    $CC -std=c11 -O2 -Wall -Wextra -Werror -Ilib/pipewire/include -c lib/pipewire/pwwire.c -o gen/pwwire.o
    $CC -std=c11 -O2 -Wall -Wextra -Werror -Ilib/pipewire/include -c lib/pipewire/pipewire.c -o gen/pipewire.o
    $CC -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
      -Igen -Inative -Ilib/pipewire/include -Ilib/wayland/include -Ilib/wayland/scanner -Ilib/xkb/include \
      $(pkg-config --cflags libdrm) \
      -c native/sound.c -o gen/sound.o
    $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
      -Igen -Inative -Ilib/pipewire/include -Ilib/wayland/include -Ilib/wayland/scanner -Ilib/xkb/include \
      $(pkg-config --cflags libdrm) \
      -c bench/e2e-sound.c -o gen/e2e-sound.o
    $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
      -Igen -Ilib/wayland/include -Ilib/wayland/scanner \
      -c gen/xdg-system-bell-v1-protocol.c -o gen/bell-proto.o
    $CC gen/e2e-sound.o gen/sound.o gen/pipewire.o gen/pwwire.o gen/bell-proto.o \
      -lpthread -lm -o e2e-sound
    runHook postBuild
  '';
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    ./e2e-sound
    runHook postCheck
  '';
  installPhase = ''
    runHook preInstall
    install -Dm755 e2e-sound $out/bin/tomoe-e2e-sound
    runHook postInstall
  '';
}
