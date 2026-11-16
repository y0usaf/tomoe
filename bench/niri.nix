{ niri }:
niri.overrideAttrs (old: {
  postPatch = (old.postPatch or "") + ''
    substituteInPlace src/backend/tty.rs \
      --replace-fail '!egl_device.is_software(),' 'true,'
  '';
})
