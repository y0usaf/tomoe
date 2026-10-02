# Shell surfaces

Bars, panels, menus and wallpapers are drawn by Tomoe itself from a small
declarative UI tree that an extension returns: no toolkit, no separate bar
process.

## Retained shell surfaces

`(ui KIND &rest PROPERTIES)` constructs copied declarative UI data. Return
`(shell-surface NAME TREE &key width height anchors margin layer exclusive-zone
visible output background color font-size click-through)` as an owned effect. Names are keywords
scoped to the declaring extension. [examples/shell.lisp](../examples/shell.lisp)
is a working bar with a private click counter; its state survives reload.

The supported kinds are `:row`, `:column`, `:stack`, `:button`, `:text`, `:rect`,
`:spacer`, `:separator`, `:progress`, `:circular-progress`, `:image`, and `:icon`.
Containers take `:children`; rows, columns, and buttons also take `:gap`, `:padding`, `:align`,
and `:justify`. Common properties include `:width`, `:height`, `:grow`,
`:background`, `:border-color`, `:border`, `:radius`, `:key`, and `:on-click`.
Text takes `:text`, `:font`, `:size`, `:line-height`, and `:color`.
Progress values range from zero to one, with `:color` and `:track`; circular
progress also takes `:size` and `:thickness`. Separators take `:orientation`,
`:thickness`, and `:color`. Colors accept `#rgb`, `#rrggbb`, `#rrggbbaa`, or an
integer in RRGGBBAA order. Unsupported kinds and properties reject the proposal.

Images take `:src` for a PNG or JPEG file. Their intrinsic dimensions are file
pixels; explicit `:width` and `:height` are logical units scaled per output.
The image stretches to its allocated rectangle. Icons take `:name`, optional
SVG `:path`, logical `:size` (default 16), and optional `:color` tint. They draw
in a centered square inside their allocated rectangle. An omitted or empty
path searches `XDG_DATA_DIRS` for scalable SVGs in hicolor, Adwaita, then breeze;
an explicit path does not fall back to a theme. A missing image is transparent,
and a missing icon displays its name. Relative file paths resolve beside the
extension source, so separately mounted policies can use the same filenames.

```lisp
(ui :row :children
  (list (ui :image :src "assets/avatar.png" :width 32 :height 32)
        (ui :icon :name "audio-volume-high" :size 20 :color "#cdd6f4")))
```

Decoded assets and failed lookups belong to the extension's source generation.
Redraws reuse the captured files; successful source reload refreshes them.
Rejected proposals preserve accepted assets, and removing the last surface
using an asset releases it. Files are read only during preparation. The loader
accepts regular files up to 16 MiB for PNG/JPEG or 1 MiB for SVG; decoded rasters
and SVG paint targets are limited to 16384 pixels per axis and 64 MiB each.
The pool permits 256 assets and 128 MiB across live and candidate generations,
counting decoded raster bytes and encoded SVG bytes, not SVG library overhead.
Limit or allocation failures reject the proposal. Missing or malformed files
are cached fallbacks. An SVG draws images it embeds as data URLs or names by
absolute path, never by relative path or URL.

A surface `:background` is a color, `(:image PATH :fit FIT)`, or
`(:shader PATH :fps FPS)`. An image paints a PNG or JPEG beneath the tree. FIT
is `:cover` (the default; fills and crops), `:contain` (letterboxes), or `:fill`
(stretches). A worker thread decodes the file and scales it once to the
surface's physical size, so the compositor thread never decodes and only a
surface-sized texture stays resident. Until a new image is ready, and when it is
missing or malformed, the surface keeps showing its previous background and the
failure is logged. Files may reach 128 MiB and decode to 32767 pixels per axis
and 512 MiB; the scaled result counts toward the asset pool. A surface whose
tree draws nothing keeps no canvas texture.
The shipped `wallpaper` extension does this for a directory: publish
`(publish-state :wallpaper-settings '(:directory "/home/me/Pictures/walls"
:bind ((:super) "w" :next :description "Next wallpaper")))` from any owner and
it shows a random PNG or JPEG from anywhere under the directory, and another on
the optional binding, given as `bind-key`'s arguments; `:fit` defaults to
`:cover`. Without a directory it
draws nothing. By hand, a background is one surface:

```lisp
(shell-surface :wallpaper (ui :stack)
  :anchors '(:top :right :bottom :left) :layer :background
  :background '(:image "/home/me/Pictures/wall.png"))
```

A shader paints a GLSL ES 3.00 fragment shader beneath the tree, in Shadertoy's
convention: the file defines `void mainImage(out vec4 fragColor, in vec2 fragCoord)`,
with `fragCoord` in physical pixels from the surface's bottom-left corner. Tomoe
declares `iResolution` (the surface size), `iTime` (seconds since the shader
compiled), `iFrame`, and an always-zero `iMouse`; shaders that read channels or
other inputs fail to compile. Output alpha is ignored. `iTime` advances in steps
of 1/FPS (default 30, from 0 to 1000; 0 draws one still frame), and the shader
runs only to repaint its area after a step, not on every frame. A fullscreen
client on direct scanout stops it entirely. The file compiles when the
declaration is prepared, on the compositor thread; a compile error logs the GLSL
message and keeps the previous background. Shader files may reach 256 KiB.

```glsl
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    fragColor = vec4(0.5 + 0.5 * cos(iTime + uv.xyx + vec3(0, 2, 4)), 1.0);
}
```

Explicit UI dimensions, margins, padding, font sizes, and reservations are logical units,
scaled once on each output. A surface defaults to every connected output,
top/left/right anchors, intrinsic unstretched dimensions, zero margins and zone,
layer `:top`, background `#1e1e2e`, text `#cdd6f4`, and font size 13.
Opposing anchors stretch across the output; other zero surface dimensions use
the tree's intrinsic size. Positive exclusive zones reduce `:workareas` after
external layer reservations, without adding margins. Surfaces remain fixed to
the screen as the camera moves. `:background` and `:bottom` surfaces render just
above external layer surfaces of the same layer, so windows cover them; `:top`
and `:overlay` surfaces render above every client, below the cursor. Hit testing
follows that same order and the drawing clips.

Set `:on-click` to a keyword command and optionally set a stable `:key` on the
element. Visible click keys must be unique within a surface; omitted keys use
`node-N` from full-tree preorder, including clipped nodes. Use explicit keys
when dynamically reordering nodes.
A left press delivers a private `:ui` event to the owning reducer with
lowercase `:command`, `:surface`, `:output`, and `:element` strings, surface-local
physical `:x`/`:y`, `:button`, and `:modifiers`. The deepest eligible element
handles the click; a parent handles uncovered descendants. On `:top` and
`:overlay` surfaces, blank regions and other buttons consume both edges without a
callback. On `:background` and `:bottom` surfaces, input outside elements with
`:on-click` or `:on-hover` passes through to bindings and the layers below.
`:click-through t` gives a surface on any layer that rule: a `:top` or
`:overlay` surface then takes the pointer only over its `:on-click` and
`:on-hover` elements, and motion, buttons, and scroll elsewhere reach whatever
lies beneath it. A held press retains only its consumed edge after unmount; stale callbacks cannot
enter a replacement source. Other reducers observe resulting context changes, without receiving the
private click. Command effects returned by this event run after publication.

Pango shapes text, and Cairo rasterizes text, vectors, images, and icons before
publication. Text is antialiased in grayscale without hinting, whatever fontconfig
asks for: a shell surface is translucent over content it cannot see, where
subpixel color fringes show, and hinting distorts pixel fonts off their grid. Unchanged plans reuse their native textures, and frame rendering
runs no extension code.
Handler tokens survive text, style, and geometry changes while the source,
surface, output, click key, and command remain the same. Already queued clicks
therefore reach the same handler even if the first click redraws or resizes it.
Removal, command changes, source replacement, and output destruction retire the
old token. Failed generations preserve accepted resources. Removing a
surface releases its textures and callback entries; native output destruction
immediately retires its textures and callbacks before Lisp drains the output
notification. Output invalidations force presentation even if a same-name
replacement has identical geometry, restoring the shell with fresh callbacks.
`inspect` includes `:native-ui` resource and rasterization counters, with
`:assets`, `:asset-bytes`, and cumulative `:asset-loads` for the asset pool;
`hit-test` adds optional `:ui` metadata. Trees, dimensions, surface counts, and
retained memory have explicit bounds. Shell surfaces do not
take keyboard focus.
