---@meta moonshell
-- LuaLS definitions for the moonshell shell subsystem (FUSION F2) — the
-- `shell.*` API and the `ui.*` element vocabulary, first-class alongside the
-- `tomoe` core API (meta/tomoe.lua). Compiled into docs/lua-api.md by
-- src/docgen.rs; parity tests keep this file in lockstep with the registered
-- API.
--
-- Conventions the shell API shares (inherited moonshell contract):
--  * Element props are logical px; layout resolves to integer physical
--    pixels once, inside the engine.
--  * Render callbacks return plain element tables; service state arrives as
--    snapshots. Writes (windows, timers) are queued and applied when the
--    callback returns — the same snapshot/queue/watchdog contract as the
--    `tomoe` API.

shell = {}

-- ─── Shell ────────────────────────────────────────────────────────────────────

---Declare a compositor-internal shell surface (bar or popup), rendered
---in-process from element trees — no Wayland client involved. Bar mode:
---`position` = "top"|"bottom"|"left"|"right" stretches across that edge
---(`height`, or `width` for side bars, is the thickness; `exclusive`
---reserves space in the same usable-area computation layer-shell clients
---use). Popup mode: `anchor` = "top-left"|"top"|…|"bottom-right" anchors a
---fixed `popup_width` x `height` surface offset by `margin_*`. The surface
---exists once per output and survives hotplug.
---@param opts ShellWindowOpts
---@return ShellWindow
function shell.window(opts) end

---A reactive value: `get`/`set`; setting marks the shell dirty so render
---callbacks re-run (the scene diff makes unchanged trees free).
---@param initial any
---@return ShellState
function shell.state(initial) end

---Call `fn` every `ms` milliseconds (calloop timer, watchdog-guarded).
---@param ms integer
---@param fn fun()
function shell.interval(ms, fn) end

---Call `fn` once after `ms` milliseconds.
---@param ms integer
---@param fn fun()
function shell.once(ms, fn) end

---Run a command through `sh -c`, blocking, returning its stdout (trailing
---newline stripped). For steady-state data prefer `shell.services.*` — the
---native services exist to eliminate subprocess polling.
---@param cmd string
---@return string
function shell.exec(cmd) end

---Run a command through `sh -c` without blocking; `fn` receives stdout when
---it exits.
---@param cmd string
---@param fn fun(output: string)
function shell.exec_async(cmd, fn) end

---Watch a file for changes; `fn` fires on modification. (In-process wiring
---is a recorded F2 TODO: the watch is currently accepted and dropped with a
---warning.)
---@param path string
---@param fn fun()
function shell.watch_file(path, fn) end

---Request a config reload (same contract as editing the file: fresh VM,
---surfaces re-created).
function shell.reload() end

---Quit. In-process this is a recorded no-op — the shell shares the
---compositor's lifetime; use `tomoe.quit()`.
function shell.quit() end

---Look up a named `shell.window` handle (`opts.name`).
---@param name string
---@return ShellWindow?
function shell.get_window(name) end

---Snapshot of connected outputs: array of `{ name, x, y, width, height,
---scale }`.
---@return table[]
function shell.displays() end

---Service facades (`shell.services.battery/network/mpris/…`), populated
---natively at FUSION F3; until then placeholders that render as empty
---state. Each exposes `:get()` returning a snapshot table.
shell.services = {}

---@class ShellWindowOpts
---@field position string? # bar mode: "top"|"bottom"|"left"|"right"
---@field anchor string? # popup mode: "top-left"|"top"|"top-right"|…
---@field width number? # side-bar thickness / popup_width fallback
---@field height number? # bar thickness / popup height (default 32)
---@field popup_width number? # popup mode width
---@field exclusive boolean? # reserve screen space (default true)
---@field layer string? # "background"|"bottom"|"top"|"overlay" (default "top")
---@field keyboard string? # "none"|"on_demand"|"exclusive" (default "none")
---@field margin_top number? # popup-mode margins, logical px
---@field margin_right number?
---@field margin_bottom number?
---@field margin_left number?
---@field name string? # registers the handle for shell.get_window
---@field bg string? # window background color ("#rrggbb[aa]")
---@field fg string? # default text color
---@field font_size number? # default text size, logical px

---@class ShellWindow
local ShellWindow = {}

---Store the render callback: called (under the watchdog) when shell state
---changes; returns an element table (`ui.*`).
---@param fn fun(): table
function ShellWindow:render(fn) end

---@class ShellState
local ShellState = {}

---@return any
function ShellState:get() end

---Set the value and mark the shell dirty (render callbacks re-run).
---@param value any
function ShellState:set(value) end

-- ─── Elements ─────────────────────────────────────────────────────────────────

---The element vocabulary: constructors returning plain tables the engine
---rasterizes (tiny-skia + cosmic-text, damage-diffed). Containers take
---`children` (a sequential table); every element accepts the shared style
---props bg, border_radius, width, height, fill (bool) / grow (number).
ui = {}

---Horizontal flex row. Props: children, gap, padding/padding_*, justify
---("start"|"center"|"end"), align, plus shared style props.
---@param props table?
---@return table
function ui.hbox(props) end

---Alias of `ui.hbox`.
---@param props table?
---@return table
function ui.hstack(props) end

---Vertical flex column; same props as `ui.hbox`.
---@param props table?
---@return table
function ui.vbox(props) end

---Alias of `ui.vbox`.
---@param props table?
---@return table
function ui.vstack(props) end

---Flexible gap that expands to fill available main-axis space.
---@return table
function ui.spacer() end

---A single line of text. String, reactive state, or a props table
---(content/text, size, line_height, color, chip-style bg).
---@param content_or_props string|table
---@return table
function ui.text(content_or_props) end

---Alias of `ui.text`.
---@param content_or_props string|table
---@return table
function ui.label(content_or_props) end

---Themed icon (SVG, pure vector). Name or props table (name, size, color).
---@param name_or_props string|table
---@return table
function ui.icon(name_or_props) end

---Clickable row (visual shell until F4 lands `on_click` routing).
---@param props table
---@return table
function ui.button(props) end

---Thin rule stretching across the container's cross axis. Props:
---orientation ("horizontal"|"vertical"), thickness, color.
---@param props table?
---@return table
function ui.separator(props) end

---Horizontal progress bar. Props: value (0..1), width, height, color, track.
---@param props table
---@return table
function ui.progress_bar(props) end

---Ring progress. Props: value (0..1), size, thickness, color, track.
---@param props table
---@return table
function ui.circular_progress(props) end

---Raster image from a file path (string or props table with src).
---@param props string|table
---@return table
function ui.image(props) end

---Slider (interactive at F4; renders as a progress bar until then).
---@param props table
---@return table
function ui.slider(props) end

---Text input (lands with the launcher work re-homed at F6).
---@param props table
---@return table
function ui.input(props) end

---Children overlaid on top of each other, each given the full rect.
---@param props table
---@return table
function ui.overlay(props) end

---Alias of `ui.overlay`.
---@param props table
---@return table
function ui.stack(props) end

---Scroll container (virtualization lands with `ui.list` at F6).
---@param props table
---@return table
function ui.scroll(props) end

---`element` when `condition` is truthy, else nothing.
---@param condition any
---@param element table
---@return table?
function ui.when(condition, element) end

---Map `list` through `fn`, collecting the returned elements.
---@param list table
---@param fn fun(item: any, i: integer): table
---@return table[]
function ui.map(list, fn) end

---Splice a list of children into a parent's children list.
---@param children table[]
---@return table
function ui.fragment(children) end

---Three-region bar scaffold (left / center / right), themed padding and
---gaps — the shape nur bars are built on.
---@param left table[]?
---@param center table[]?
---@param right table[]?
---@return table
function ui.bar_layout(left, center, right) end
