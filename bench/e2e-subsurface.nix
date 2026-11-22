{ pkgs }:
pkgs.stdenv.mkDerivation {
  name = "tomoe-e2e-subsurface";
  src = ./e2e-subsurface.c;
  dontUnpack = true;
  strictDeps = true;
  nativeBuildInputs = [
    pkgs.pkg-config
    pkgs.wayland-scanner
  ];
  buildInputs = [ pkgs.wayland ];
  buildPhase = ''
    xml=${pkgs.wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
    wayland-scanner client-header "$xml" xdg-shell-client-protocol.h
    wayland-scanner private-code "$xml" xdg-shell-protocol.c
    $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -I. \
      $src xdg-shell-protocol.c \
      $(pkg-config --cflags --libs wayland-client) -o tomoe-e2e-subsurface
  '';
  installPhase = "install -Dm755 tomoe-e2e-subsurface $out/bin/tomoe-e2e-subsurface";
}
