-- takhti default configuration.
-- Copy to ~/.config/takhti/init.lua and customise.
--
-- The compositor core is mechanism-only; ALL window management below comes
-- from the `wm` module — Lua code built on the same public API your config
-- uses. Don't require it and write your own to replace the WM wholesale.

local wm = require("wm")
wm.gaps = 8

takhti.settings {
  -- Per-output display settings, keyed by output name. `resolution` is
  -- "<preferred|max|WxH>" optionally followed by "@<Hz|max>"; unlisted
  -- outputs use their EDID-preferred mode. Some monitors advertise a low
  -- compatibility mode as preferred — "max@max" gets their best.
  -- displays = {
  --   ["DP-1"] = { resolution = "max@max" },
  --   ["HDMI-A-1"] = { resolution = "1920x1080@60" },
  -- },
  border = {
    width = 2,
    focused = "#7aa2f7",
    unfocused = "#3b4261",
  },
}

-- ─── Launch / close ──────────────────────────────────────────────────────────
-- The optional third argument labels the bind in the hotkey overlay
-- (Super+Shift+/); Lua-function binds without one are omitted from it.
takhti.bind("Super+Return", function() takhti.spawn("foot") end, "Spawn foot")
takhti.bind("Super+d", function() takhti.spawn("fuzzel") end, "Run an Application")
takhti.bind("Super+q", wm.close_focused, "Close Window")
takhti.bind("Super+Shift+e", "quit")
takhti.bind("Super+Shift+slash", "show-hotkey-overlay")

-- ─── Focus ───────────────────────────────────────────────────────────────────
takhti.bind("Super+j", wm.focus_next, "Focus Next Window")
takhti.bind("Super+k", wm.focus_prev, "Focus Previous Window")

-- ─── Workspaces ──────────────────────────────────────────────────────────────
for i = 1, wm.workspace_count do
  takhti.bind("Super+" .. i, function() wm.switch(i) end,
    i == 1 and "Switch to Workspace 1-9" or nil)
  takhti.bind("Super+Shift+" .. i, function() wm.move_focused(i) end,
    i == 1 and "Move Window to Workspace 1-9" or nil)
end
