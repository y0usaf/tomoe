{
  stdenv,
  pkg-config,
  wayland-scanner,
  wayland,
  wayland-protocols,
  libdrm,
}:
stdenv.mkDerivation {
  pname = "tomoe-e2e-capture";
  version = "1";
  src = ./speedlab/capture.c;
  dontUnpack = true;
  nativeBuildInputs = [
    pkg-config
    wayland-scanner
  ];
  buildInputs = [
    wayland
    libdrm
  ];
  buildPhase = ''
    runHook preBuild
    protocols=""
    for xml in \
      "${wayland-protocols}/share/wayland-protocols/staging/ext-image-capture-source/ext-image-capture-source-v1.xml" \
      "${wayland-protocols}/share/wayland-protocols/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml" \
      "${wayland-protocols}/share/wayland-protocols/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml" \
      "${wayland-protocols}/share/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml"; do
      name=$(basename "$xml" .xml)
      wayland-scanner client-header "$xml" "$name-client-protocol.h"
      wayland-scanner private-code "$xml" "$name-protocol.c"
      protocols="$protocols $name-protocol.c"
    done
    sed 's/if (dmabufs && linear_xrgb)/if (0)/' $src > capture.c
    $CC -O2 -std=gnu11 -Wall -Wextra -Werror -I. $(pkg-config --cflags libdrm) \
      -o tomoe-e2e-capture capture.c $protocols $(pkg-config --cflags --libs wayland-client)
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    install -Dm755 tomoe-e2e-capture $out/bin/tomoe-e2e-capture
    runHook postInstall
  '';
}
