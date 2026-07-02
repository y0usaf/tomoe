-- Default window manager for takhti, implemented entirely on the public Lua API.
-- This is the same surface user configs use: replace this module wholesale by
-- not requiring it, or extend it by wrapping its functions.
--
-- All geometry (window/output coordinates, gaps) is in *physical pixels*:
-- integers stay integers at any output scale, so layouts can never cause
-- blurry, misaligned windows. Set the scale with takhti.settings{ scale = n }.
--
--   local wm = require("wm")
--   wm.gaps = 4
--   takhti.bind("Alt+1", function() wm.switch(1) end)

local M = {
  gaps = 8,
  workspace_count = 9,
  active = 1,
  -- workspaces[i] = ordered list of window objects
  workspaces = {},
}

for i = 1, M.workspace_count do
  M.workspaces[i] = {}
end

local function find(list, win)
  for i, w in ipairs(list) do
    if w:id() == win:id() then
      return i
    end
  end
  return nil
end

local function remove(list, win)
  local i = find(list, win)
  if i then
    table.remove(list, i)
  end
end

-- Classic dwindle: split the remaining area along its longer side.
function M.arrange()
  local area = takhti.usable_area()
  local wins = M.workspaces[M.active]
  local n = #wins
  if n == 0 then
    return
  end
  local g = M.gaps
  local x, y = area.x + g, area.y + g
  local w, h = area.w - 2 * g, area.h - 2 * g
  for i, win in ipairs(wins) do
    if i == n then
      win:set_geometry(x, y, w, h)
    elseif w >= h then
      local half = math.floor((w - g) / 2)
      win:set_geometry(x, y, half, h)
      x = x + half + g
      w = w - half - g
    else
      local half = math.floor((h - g) / 2)
      win:set_geometry(x, y, w, half)
      y = y + half + g
      h = h - half - g
    end
    win:show()
  end
end

function M.switch(n)
  if n == M.active or n < 1 or n > M.workspace_count then
    return
  end
  for _, win in ipairs(M.workspaces[M.active]) do
    win:hide()
  end
  M.active = n
  M.arrange()
  local last = M.workspaces[n][#M.workspaces[n]]
  if last then
    last:focus()
  else
    -- Don't leave a hidden window from the old workspace holding the keyboard.
    takhti.clear_focus()
  end
end

function M.move_focused(n)
  if n == M.active or n < 1 or n > M.workspace_count then
    return
  end
  local win = takhti.focused_window()
  if not win then
    return
  end
  remove(M.workspaces[M.active], win)
  win:hide()
  table.insert(M.workspaces[n], win)
  M.arrange()
  local last = M.workspaces[M.active][#M.workspaces[M.active]]
  if last then
    last:focus()
  else
    takhti.clear_focus()
  end
end

local function cycle(dir)
  local wins = M.workspaces[M.active]
  if #wins == 0 then
    return
  end
  local focused = takhti.focused_window()
  local idx = focused and find(wins, focused) or 1
  idx = ((idx - 1 + dir) % #wins) + 1
  wins[idx]:focus()
end

function M.focus_next()
  cycle(1)
end

function M.focus_prev()
  cycle(-1)
end

function M.close_focused()
  local win = takhti.focused_window()
  if win then
    win:close()
  end
end

takhti.on_window_open(function(win)
  table.insert(M.workspaces[M.active], win)
  M.arrange()
  win:focus()
end)

takhti.on_window_close(function(win)
  for i = 1, M.workspace_count do
    remove(M.workspaces[i], win)
  end
  M.arrange()
  local last = M.workspaces[M.active][#M.workspaces[M.active]]
  if last then
    last:focus()
  end
end)

takhti.on_outputs_changed(function()
  M.arrange()
end)

return M
