-- Special workspaces (scratchpads) for tomoe, implemented entirely on the
-- public Lua API — the same surface user configs get. A "special" is a named
-- Hyprland-style overlay workspace: windows parked in one stay hidden until
-- toggled, then appear centered over whatever the active WM shows. Closing
-- the special hides its windows again instead of dropping them back into
-- tiling.
--
-- The core mechanism is deliberately unchanged (PLAN.md mechanism audit):
-- Show/Hide ops, focus, and geometry are all a special workspace needs. The
-- state that would survive a reload is just window ids, which on_reload
-- already carries.
--
--   local wm = require("wm")
--   local special = require("special")
--   tomoe.bind("Mod+grave", function() special.toggle("term") end)
--   tomoe.bind("Mod+Shift+grave", function() special.move_focused("term") end)
--
-- Named specials are independent: each is just a window list + shown flag, so
-- configs can stack as many as they want ("term", "mixer", "chat", …).
--
---Scratchpad/special-workspace policy, layered on top of any WM module that
---exposes `arrange()` (wm.lua, or a user WM with the same contract). All
---geometry is physical pixels. Preloaded as module "special"; requiring it
---installs its hooks.
---@class special
---@field specials table<string, special.Space> # per-name state: { windows = Window[], shown = boolean }
local M = {
  -- specials[name] = { windows = ordered list of window objects, shown = bool }
  specials = {},
}

---@class special.Space
---@field windows Window[]
---@field shown boolean

local function space(name)
  local s = M.specials[name]
  if not s then
    s = { windows = {}, shown = false }
    M.specials[name] = s
  end
  return s
end

local function find(list, win)
  for i, w in ipairs(list) do
    if w:id() == win:id() then
      return i
    end
  end
  return nil
end

-- The WM module, if one is loaded. `require` is safe to call repeatedly:
-- package.loaded caches. A config running without a tiling WM (bare core or
-- a fully custom policy) gets nil, and specials fall back to centering on the
-- usable area without touching tiling state.
local function tiling_wm()
  local ok, wm = pcall(require, "wm")
  if ok and type(wm) == "table" and type(wm.arrange) == "function" then
    return wm
  end
  return nil
end

local function in_tiling(win)
  local wm = tiling_wm()
  if not wm or not wm.workspaces then
    return false
  end
  for _, list in pairs(wm.workspaces) do
    for _, w in ipairs(list) do
      if w:id() == win:id() then
        return true
      end
    end
  end
  return false
end

local function remove_from_tiling(win)
  local wm = tiling_wm()
  if not wm or not wm.workspaces then
    return
  end
  for _, list in pairs(wm.workspaces) do
    local i = find(list, win)
    if i then
      table.remove(list, i)
      return
    end
  end
end

-- Centered, 3/4 of the usable area in each dimension — the classic
-- scratchpad presentation. Physical pixels, integer-rounded.
local function overlay_geometry()
  local a = tomoe.usable_area()
  local w = math.floor(a.w * 3 / 4)
  local h = math.floor(a.h * 3 / 4)
  local x = a.x + math.floor((a.w - w) / 2)
  local y = a.y + math.floor((a.h - h) / 2)
  return x, y, w, h
end

local function present(s)
  local x, y, w, h = overlay_geometry()
  for _, win in ipairs(s.windows) do
    win:set_geometry(x, y, w, h)
    win:show()
    win:raise()
  end
  local last = s.windows[#s.windows]
  if last then
    last:focus()
  end
end

---Toggle special `name`: hidden → shown over the current workspace; shown →
---hidden again. The active workspace's layout is never disturbed.
---@param name string
function M.toggle(name)
  local s = space(name)
  if s.shown then
    M.hide(name)
  else
    M.show(name)
  end
end

---Show special `name` (no-op if already shown).
---@param name string
function M.show(name)
  local s = space(name)
  if s.shown then
    return
  end
  s.shown = true
  present(s)
end

---Hide special `name` (no-op if already hidden). Focus moves back to the
---active workspace's most recent window via the WM, or clears.
---@param name string
function M.hide(name)
  local s = space(name)
  if not s.shown then
    return
  end
  s.shown = false
  for _, win in ipairs(s.windows) do
    win:hide()
  end
  local wm = tiling_wm()
  if wm and wm.workspaces and wm.active then
    local list = wm.workspaces[wm.active]
    local last = list and list[#list]
    if last then
      last:focus()
      return
    end
  end
  tomoe.clear_focus()
end

---Move the focused window into special `name`, hiding it until toggled.
---@param name string
function M.move_focused(name)
  local win = tomoe.focused_window()
  if not win then
    return
  end
  local s = space(name)
  if find(s.windows, win) then
    return
  end
  remove_from_tiling(win)
  table.insert(s.windows, win)
  if s.shown then
    present(s)
  else
    win:hide()
  end
  local wm = tiling_wm()
  if wm then
    wm.arrange()
    local list = wm.workspaces and wm.workspaces[wm.active]
    local last = list and list[#list]
    if last then
      last:focus()
    else
      tomoe.clear_focus()
    end
  end
end

---Is the special currently shown?
---@param name string
---@return boolean
function M.is_shown(name)
  local s = M.specials[name]
  return s ~= nil and s.shown
end

---Window ids parked in special `name` (bar-facing, JSON-friendly).
---@param name string
---@return integer[]
function M.windows(name)
  local ids = {}
  local s = M.specials[name]
  if s then
    for _, win in ipairs(s.windows) do
      ids[#ids + 1] = win:id()
    end
  end
  return ids
end

-- Windows a rule sends to a special (`special = "name"`) arrive hidden and
-- never join tiling. Routed here instead of growing wm.lua: specials are an
-- opt-in layer, and the rule property is honored wherever this module is
-- required.
tomoe.on_window_open(function(win)
  local r = tomoe.rules_for(win)
  if type(r.special) ~= "string" then
    return
  end
  -- wm's hook ran first (registration order) and filed the window into a
  -- workspace; take it back out.
  remove_from_tiling(win)
  local s = space(r.special)
  table.insert(s.windows, win)
  if s.shown then
    present(s)
  else
    win:hide()
  end
  local wm = tiling_wm()
  if wm then
    wm.arrange()
  end
end)

tomoe.on_window_close(function(win)
  for _, s in pairs(M.specials) do
    local i = find(s.windows, win)
    if i then
      table.remove(s.windows, i)
      if s.shown then
        present(s)
      end
      return
    end
  end
end)

-- Output changes can move the usable area; re-present visible overlays.
tomoe.on_outputs_changed(function()
  for _, s in pairs(M.specials) do
    if s.shown then
      present(s)
    end
  end
end)

-- Survive config reloads: persist as { name = { ids..., shown } } and rebuild
-- handles in the fresh VM, same contract as wm's on_reload. Windows a dead
-- config had parked stay parked (they keep their ids); anything unknown falls
-- to wm's own restore replay.
tomoe.on_reload("special", function()
  local saved = {}
  for name, s in pairs(M.specials) do
    local ids = {}
    for _, win in ipairs(s.windows) do
      ids[#ids + 1] = win:id()
    end
    saved[name] = { windows = ids, shown = s.shown }
  end
  return saved
end, function(state)
  M.specials = {}
  for name, saved in pairs(state or {}) do
    local s = space(name)
    s.shown = saved.shown == true
    for _, id in ipairs(saved.windows or {}) do
      local win = tomoe.window(id)
      if win then
        table.insert(s.windows, win)
        if not in_tiling(win) then
          -- wm's restore won't show it (it never tracked it); keep hidden
          -- until toggled, or present if the special was up.
          if s.shown then
            -- presented below, once all handles exist
          else
            win:hide()
          end
        end
      end
    end
    if s.shown then
      present(s)
    end
  end
end)

return M
