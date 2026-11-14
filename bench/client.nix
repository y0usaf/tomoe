{ pkgs }:
pkgs.stdenv.mkDerivation {
  name = "tomoe-bench-client";
  src = ./.;
  dontUnpack = true;
  strictDeps = true;
  nativeBuildInputs = [
    pkgs.pkg-config
    pkgs.wayland-scanner
  ];
  buildInputs = [ pkgs.wayland ];
  buildPhase = ''
    wp=${pkgs.wayland-protocols}/share/wayland-protocols
    for xml in $wp/stable/xdg-shell/xdg-shell.xml \
      $wp/stable/presentation-time/presentation-time.xml \
      ${pkgs.wlr-protocols}/share/wlr-protocols/unstable/wlr-virtual-pointer-unstable-v1.xml; do
      name=$(basename "$xml" .xml)
      wayland-scanner client-header "$xml" "$name-client-protocol.h"
      wayland-scanner private-code "$xml" "$name-protocol.c"
    done
    wayland-scanner client-header ${./virtual-keyboard-unstable-v1.xml} \
      virtual-keyboard-unstable-v1-client-protocol.h
    wayland-scanner private-code ${./virtual-keyboard-unstable-v1.xml} \
      virtual-keyboard-unstable-v1-protocol.c
    $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. \
      $src/bench-client.c *-protocol.c \
      $(pkg-config --cflags --libs wayland-client) -o bench-client
  '';
  installPhase = "install -Dm755 bench-client $out/bin/bench-client";
}
