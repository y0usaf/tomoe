{ pkgs }:
pkgs.stdenv.mkDerivation {
  name = "tomoe-e2e-pw-portal";
  src = ./e2e-pw-portal.c;
  dontUnpack = true;
  strictDeps = true;
  nativeBuildInputs = [ pkgs.pkg-config ];
  buildInputs = [
    pkgs.pipewire
    pkgs.libdrm
  ];
  buildPhase = ''
    $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
      $src $(pkg-config --cflags --libs libpipewire-0.3 libdrm) \
      -o tomoe-e2e-pw-portal
  '';
  installPhase = "install -Dm755 tomoe-e2e-pw-portal $out/bin/tomoe-e2e-pw-portal";
}
