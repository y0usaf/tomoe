-- moonshell.widgets.battery (ported from nur.widgets.battery)
-- Displays battery percentage and charging status. Renders nothing on
-- machines without a battery (the service's `available` flag — M3 §3).
--
-- Usage:
--   local Battery = require("moonshell.widgets.battery")
--   local bat = Battery.new()
--   -- In a render function:
--   bat:render()

local M = {}

function M.new(opts)
    opts = opts or {}

    local self = {}

    function self:render()
        local bat = shell.services.battery:get()
        if bat.available == false then
            -- No battery (desktop): an empty box lays out to nothing,
            -- and stays safe inside any children list (a nil would
            -- truncate it).
            return ui.hbox({})
        end
        local pct = bat.percent or 0
        local icon = bat.charging and "battery-charging" or (
            pct > 80 and "battery-full"    or
            pct > 40 and "battery-medium"  or
            pct > 15 and "battery-low"     or
            "battery-warning"
        )
        return ui.hbox({ gap = 4, children = {
            ui.icon(icon),
            ui.text(pct .. "%"),
        }})
    end

    return self
end

return M
