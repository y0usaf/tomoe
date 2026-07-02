-- Example config for zoomer, the floating/zooming/scrolling canvas WM.
-- Try it without touching your real config:
--
--   tomoe --config resources/examples/zoomer-init.lua
--
-- Mod+left-drag moves, Mod+right-drag resizes, Mod+middle-drag pans,
-- Mod+scroll zooms around the cursor, Mod+Tab switches planes.

tomoe.settings {
  mod = "alt", -- what "Mod" means everywhere below
  winit_size = { 1920, 1080 },
  border = {
    width = 2,
    focused = "#7aa2f7",
    unfocused = "#3b4261",
  },
}

require("zoomer").setup {
  planes = 4,
}

tomoe.bind("Mod+t", function() tomoe.spawn("foot") end, "terminal")
tomoe.bind("Mod+q", function()
  local win = tomoe.focused_window()
  if win then
    win:close()
  end
end, "close window")
tomoe.bind("Mod+Shift+e", "quit")
