-- Default screencast source picker for tomoe's xdg-desktop-portal backend,
-- implemented entirely on the public Lua API: portal source policy is
-- config policy (the portal asks the compositor over IPC, the compositor
-- asks this hook). Don't require this module (or register your own
-- tomoe.on_screencast_request) to replace the policy; without any hook the
-- portal falls back to its TOMOE_SCREENCAST_OUTPUT / TOMOE_PORTAL_CHOOSER
-- environment-variable heuristics.

---Compositor-drawn screencast source picker: answers ScreenCast portal
---requests with a tomoe.ui.menu of the allowed sources (single candidates
---resolve without asking). Preloaded as module "screencast"; requiring it
---installs its tomoe.on_screencast_request hook. Per-app policy composes
---with window rules: when a mapped window of the requesting app matches a
---rule carrying a `screencast` property, `screencast = false` denies the
---request and `screencast = "DP-1"` casts that output without asking.
---@class screencast
local M = {}

-- A rule-driven answer for the requesting app, if any. Rules match windows,
-- so the app's screencast policy rides on any mapped window with the same
-- app id (e.g. tomoe.rule { app_id = "^obs$", screencast = "DP-1" }).
local function rule_answer(req)
  for _, win in ipairs(req.windows) do
    if win:app_id() == req.app_id then
      local sc = tomoe.rules_for(win).screencast
      if sc == false then
        return { deny = true }
      elseif type(sc) == "string" then
        return { output = sc }
      end
    end
  end
  return nil
end

tomoe.on_screencast_request(function(req)
  local ruled = rule_answer(req)
  if ruled then
    if ruled.deny then
      return false
    end
    return ruled
  end

  local items, picks = {}, {}
  if req.types.monitor then
    for _, o in ipairs(req.outputs) do
      items[#items + 1] = ("output %s (%dx%d)"):format(o.name, o.w, o.h)
      picks[#picks + 1] = { output = o.name }
    end
  end
  if req.types.window then
    for _, win in ipairs(req.windows) do
      local title = win:title()
      if title == "" then
        title = win:app_id()
      end
      items[#items + 1] = ("window %s"):format(title)
      picks[#picks + 1] = { window = win }
    end
  end
  if #items == 0 then
    return false
  end
  if #items == 1 then
    return picks[1]
  end

  -- Multiple candidates: defer to an interactive compositor-drawn menu.
  -- The portal waits (bounded by its own timeout); the compositor doesn't.
  req:defer()
  local who = req.app_id ~= "" and req.app_id or "an application"
  tomoe.ui.menu {
    title = "Share screen with " .. who,
    items = items,
    on_select = function(i)
      req:resolve(picks[i])
    end,
    on_cancel = function()
      req:deny()
    end,
  }
end)

return M
