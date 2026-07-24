-- Notification popups — an ordinary builtin on the same ui.*/shell.*
-- surface user configs get (doctrine 01): the default config calls
-- `require("moonshell.notifications").setup()`; restyle it by passing
-- opts, or don't call it and render `shell.services.notifications`
-- yourself.
--
-- The popup surface is intrinsic-sized (width/height 0): it hugs the
-- card stack and vanishes (1x1 transparent) when nothing is active.

local M = {}

---@param opts table? # anchor, margin, width, bg, urgent_bg, fg, body_fg, radius
function M.setup(opts)
    opts = opts or {}
    local width = opts.width or 320
    local win = shell.window({
        anchor = opts.anchor or "top-right",
        popup_width = 0,
        height = 0,
        margin_top = opts.margin or 8,
        margin_right = opts.margin or 8,
        exclusive = false,
        bg = "#00000000",
        name = "moonshell.notifications",
    })
    win:render(function()
        local snap = shell.services.notifications:get()
        local cards = {}
        for _, n in ipairs(snap.notifications or {}) do
            local lines = {
                ui.text({
                    content = (n.summary ~= "" and n.summary) or n.app,
                    size = 14,
                    color = opts.fg or "#cdd6f4",
                }),
            }
            if n.body and n.body ~= "" then
                lines[#lines + 1] = ui.text({
                    content = n.body,
                    size = 12,
                    color = opts.body_fg or "#a6adc8",
                })
            end
            cards[#cards + 1] = ui.vbox({
                width = width,
                bg = n.urgent and (opts.urgent_bg or "#45222c") or (opts.bg or "#1e1e2e"),
                border_radius = opts.radius or 8,
                padding = 10,
                gap = 4,
                children = lines,
            })
        end
        return ui.vbox({ gap = 8, children = cards })
    end)
    M.window = win
    return win
end

return M
