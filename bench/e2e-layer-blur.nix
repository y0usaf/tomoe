{ pkgs }:
pkgs.stdenv.mkDerivation {
  name = "tomoe-e2e-layer-blur";
  src = ./e2e-layer-blur.c;
  dontUnpack = true;
  strictDeps = true;
  nativeBuildInputs = [
    pkgs.pkg-config
    pkgs.wayland-scanner
  ];
  buildInputs = [ pkgs.wayland ];
  buildPhase = ''
    xdg=${pkgs.wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
    layer=${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml
    bg=${pkgs.wayland-protocols}/share/wayland-protocols/staging/ext-background-effect/ext-background-effect-v1.xml
    wayland-scanner client-header "$xdg" xdg-shell-client-protocol.h
    wayland-scanner private-code "$xdg" xdg-shell-protocol.c
    wayland-scanner client-header "$layer" wlr-layer-shell-unstable-v1-client-protocol.h
    wayland-scanner private-code "$layer" wlr-layer-shell-unstable-v1-protocol.c
    wayland-scanner client-header "$bg" ext-background-effect-v1-client-protocol.h
    wayland-scanner private-code "$bg" ext-background-effect-v1-protocol.c
    $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. \
      $src xdg-shell-protocol.c wlr-layer-shell-unstable-v1-protocol.c ext-background-effect-v1-protocol.c \
      $(pkg-config --cflags --libs wayland-client) -o tomoe-e2e-layer-blur
  '';
  installPhase = "install -Dm755 tomoe-e2e-layer-blur $out/bin/tomoe-e2e-layer-blur";
}
