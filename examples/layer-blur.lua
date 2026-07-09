-- Enable blur behind selected rectangular layer-shell surfaces.
-- Use the namespace reported/configured by your bar or notification center.
tomoe.settings {
  blur = {
    enabled = true,
    passes = 3,
    offset = 1.0,
    layer_namespaces = { "waybar", "swaync-control-center" },
  },
}
