# Changelog

Each release lists what changed since the one before it. `nix run .#release`
cuts a candidate only from a commit whose `flake.nix` version has a section here.

## 0.1.0

The first release.

- A Wayland compositor with its own C core: it speaks the wire protocol and runs
  its event loop itself, drives KMS through the kernel's ioctls, renders with EGL
  and GLES2 on GBM, and runs on DRM from a TTY, nested in another compositor, or
  headless. No wlroots.
- Images and icons are decoded and rendered in-tree: PNG and JPEG without
  libpng, libjpeg or cairo, and SVG icons and assets without resvg, with the
  same antialiasing, gradients, patterns, markers, images, masks, clips and
  filters.
- The window manager, bar and wallpaper are Lisp extensions, in a Common Lisp
  dialect the C core carries, loaded from `~/.config/tomoe/init.lisp`, reloaded
  on save and live-patchable with `tomoe command patch`. A config that fails to
  load is refused and rolled back; the running one keeps going.
- One command table for key bindings, the `tomoe` command line and the JSON
  socket.
- Built-in X11 bridge (Xwayland), notification server, screencast portal, session
  services (media, battery, backlight, sounds, network, tray) and a session bus
  that is replaced when it dies; services die with the compositor.
- The screencast portal speaks PipeWire's native protocol itself: it creates and
  drives its own video node, allocates the buffers it hands out and writes the
  cursor metadata the client reads, so the portal links no PipeWire library.
- A broken extension, theme or config loses only itself: one that hangs or runs
  the heap out twice in a row is unmounted, one whose surfaces pass the canvas
  keeps its last answer while the rest commit, a surface that cannot be staged keeps its last frame, and a
  refused load gives its memory back. A service that crashes or stops never
  stalls the compositor.
- The region screenshot's selector is drawn by the theme: the core keeps the
  frozen frame, the selection and the capture, and hands the selection to a
  shader on the `:screenshot` layer as `iSelection`, the live corners beside a
  spring-carried copy (`:screenshot-motion`) for looks that trail the drag. A
  theme publishes its look as `:screenshot-look`; without one the selector is
  the dimmed output, accent frame and key hint it always was. Shell shaders can
  also be given as text with `(:glsl SOURCE ...)`, and a UI text element with
  `:markup t` sets Pango markup at its layout's own height.
- The region screenshot's marks are modal: `A`, `T` and `F` put the pointer in
  an arrow, label or freehand mode and it stays there mark after mark until `R`
  goes back to selecting, and the hint's chip for the tool in hand takes the
  hint's ink so the mode shows in the chrome. Each mode owns the keys that shape
  its mark: `[` and `]` held change the brush by half in log size a second,
  easing in and out over 0.1 s, the bind and the step Mado sizes a panel with,
  which grows a label while it is being typed and thins or thickens the stroke
  the arrow and the freehand share and the arrow's head; a label's size is its
  own. A hold ends when its own bracket is released, whatever else is typed
  meanwhile. `H` switches the arrow head between a filled triangle and a thin open
  chevron. A mark keeps the size it was drawn at, so the capture and undo see
  the same marks. `U`, `R`, Enter, Space and Esc keep their meaning.
- The region screenshot takes marks before it is captured: `A`, `T` and `F`
  pick an arrow, a text label and a freehand stroke, the pointer draws them
  inside the selection, `U` (or Ctrl+Z) undoes the last mark, `R` goes back to
  selecting a region, and Enter or Space captures the region with the marks in
  the image. The core rasterises the marks into a coverage mask on the
  `:screenshot` layer, a look's shader reads it with `marked(p)` over its own
  ink, and the look's `:mark` plist sets that ink, the stroke width, the arrow
  head, the label size and font, and the tool keys (`:screenshot-mark`
  settings). A look that does not read `marked` draws the selector without
  them.
- Every session logs to `~/.local/state/tomoe/SOCKET.log`: why it stopped, every
  refused load, stopped extension, withdrawn surface and exited service, and a
  backtrace if the compositor itself crashes.
- An animated shader background holds the picture it is drawing once the machine
  has had no input for `:shader-hold` seconds, 3 by default, and carries on from
  that same frame on the next input, even one that changes nothing else on
  screen, so a moving wallpaper stops drawing when nobody is there instead of
  skipping the time it was idle.
- A blur behind a window that is opaque but rounded is rebuilt only at the
  corners its rounding shows, and only what those corners read beneath it is
  drawn, so a moving wallpaper under such windows redraws their corners rather
  than every window it frosts, with the same pixels.
- A shader background declared `:opaque t` hides what lies beneath it, so a
  full-screen animated wallpaper no longer repaints the clear under it every
  frame.
- A window moved again while it is still moving, a column sent to and back
  before the last move finished, carries on from the pixels it is drawn at and
  the speed it has there, instead of standing still for the rest of the move it
  replaced and then jumping to its new place.
