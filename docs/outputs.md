# Outputs

Displays are configured by an ordinary extension, so a monitor layout is code
you can reload. This page covers modes, scale, placement, colour profiles,
virtual outputs and the pixel model underneath.

## Output resolution and pixel mapping

Startup loads `$XDG_CONFIG_HOME/tomoe/init.lisp`, falling back to
`~/.config/tomoe/init.lisp`. `--config FILE` replaces that default file;
`--bare` skips it unless you also supply `--config`. An absent file is fine.
Output policy is an ordinary extension, with the same mount/reload/unmount
behavior as window policy:

```lisp
(define-extension "displays" () (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil
          (list (configure-output "DP-4" :mode '(5120 1440)
                                        :scale 1 :position '(0 0))
                (configure-output "HDMI-A-2" :mode '(1920 1080 60)
                                             :scale 1 :position '(5120 0)))
          nil))
```

`configure-output` accepts:

- `:mode :preferred`, the monitor's advertised preferred mode, the default.
- `:mode :max`, the largest advertised pixel area at its highest refresh.
- `:mode '(WIDTH HEIGHT)`, that pixel resolution at its highest refresh.
- `:mode '(WIDTH HEIGHT HZ)`, the closest advertised refresh within 1 Hz.
  For example, 60 also matches 59.94 Hz. Unavailable advertised modes fall back
  to the preferred mode, or the first advertised mode when none is preferred.
  Headless, nested and virtual outputs accept custom dimensions.
- `:scale`, from 1/4 through 8, rounded to 1/120 increments. The default is 1.
- `:position '(X Y)`, in physical screen pixels. Omit it for automatic
  horizontal placement. Disconnected output names remain configured for hotplug.
- `:transform`, one of `:normal`, `:90`, `:180`, `:270`, `:flipped`,
  `:flipped-90`, `:flipped-180` and `:flipped-270`, the Wayland output
  transforms, which `:outputs` reports as the protocol's enum. The rotation is
  rendered, so it costs no hardware support. Omitted, the output keeps its
  baseline, normally `:normal`.
- `:disabled t`, to turn off a connected output and withdraw its Wayland global.
  It stays in `:connectors` so a policy can discover and re-enable it.
- `:mirror "OUTPUT"`, to use an active, non-mirroring output's physical origin.
  This overrides `:position` and consumes no additional automatic layout space.
  Missing, disabled, self, or mirroring targets fall back to automatic placement
  after ordinary outputs.
- `:vrr t`, to request adaptive sync when the backend advertises support.
  Adaptive sync stays off on unsupported outputs; a supported backend's rejection rolls back
  the transaction. Hardware VRR operation remains unverified.
- `:icc "/path/profile.icc"`, an RGB display profile with colorants and tone
  curves, plus an optional `vcgt` calibration. sRGB content, taken as gamma 2.2,
  is shown in the panel's colors through the CRTC's `DEGAMMA_LUT`, `CTM` and
  `GAMMA_LUT`, so it costs no rendering and holds for direct scanout. The
  degamma curve is linear below the gamma LUT's first entry, so grays round-trip
  exactly through uniformly spaced LUTs. Gamma-control clients apply on top.
  `:icc` in `:outputs` names the profile in use; when a profile cannot be read,
  the backend has no such LUTs, or the driver refuses them, the output runs
  without one and `:icc-error` says why.

These fields belong to the same output declaration. A later owner replaces the
whole declaration; removal restores the preceding owner or native baseline.
Disabling every output is supported. Output and connector consumers see the
prospective topology before commit, and failed transactions retain accepted
policy, shell resources and Wayland output identities.
Startup and hotplug policies settle before an output's first enable commit or
Wayland global advertisement. A predeclared disable rule keeps the connector
inactive throughout admission; the first enabled frame includes its settled
shell. Connectors expose a lifetime `:id`, advertised `:modes`, and `:pending`
admission status. A backend admission failure keeps the arriving connector
inactive and records its name and message in `:output-errors`, while preserving
the requested declaration and allowing other outputs and owners to settle.
Changing or removing the declaration, or reconnecting the port, permits recovery.

Backend mode, scale and transform requests also settle through the complete
presentation transaction. Multiple requests received before settlement coalesce;
accepted output geometry remains unchanged until publication. Connector
`:request-id` and `:request-pending` fields identify these proposals independently
of `:outputs`. Existing declarations retain precedence, while successful requests
update the underlying backend settings restored on owner removal. Requests only
change the fields they supply. Reducer or backend rejection retains the accepted
geometry and baseline, reports `:output-errors`, and allows a later request or
declaration to recover.

Window placement uses integer physical world pixels.
Output positions and layer geometry use physical screen pixels. An output at
3840x2160 still occupies that many pixels at scale 2; its protocol description
and client configure sizes use logical units at the conversion boundary.
Requested window sizes round to integer logical sizes, then back to achievable
physical sizes in `:layout`. Exact halves round away from zero. Positions are
never rounded through a logical scene position.

`:window-geometry` reports committed client sizes at the candidate output scale
and world location. A client can acknowledge a configure later or choose another
size, so this rectangle can differ from `:layout`. Geometry readers see policy
placement, visibility, camera and output changes during settlement; client size
changes arrive when the surface commits. Size-only commits leave `:windows`
readers idle.

Viewporter, fractional-scale-v1, and xdg-output let compatible clients render
buffers at the requested scale while keeping logical window sizes and input
coordinates consistent. The renderer projects physical destination edges, and
hit testing inverts those same rectangles, including rounded edges. Clients without fractional-scale support may render
at an integer scale and be resampled.

The pointer keeps a physical screen position independently of the logical output
layout. Each output's cursor is positioned from that value, so overlapping
logical rectangles at different scales cannot redirect input or duplicate the
cursor. Camera and scene changes update pointer focus even without mouse motion.
Relative device deltas use logical compositor units and convert at the starting
output's scale; crossing an output changes the next event's scale.
The wlr virtual-pointer protocol follows the same seat path as backend devices;
absolute virtual pointers can select an output, and nested backend pointers use
their named output.

Later-mounted output policies win per output. Unmount restores the previous
owner or the output's initial mode, scale, and automatic placement. The backend
validates the full configuration before committing and attempts to restore the
previous hardware state if a commit fails. A failed hardware rollback stops the
compositor. During policy settlement, `:outputs` consumers see the candidate
mode, scale, and physical layout before native commit. Their placement sizes
are quantized against those candidate scales. Matching native confirmations
do not rerun consumers; output revisions discard older queued confirmations
after a newer commit. External output changes still notify consumers.
Layer geometry and usable output areas resolve in the same dependency rounds.
`output-power` turns a display off without leaving the layout: the CRTC shuts
down so the monitor sleeps, the output stops rendering and sending frame
callbacks, and windows keep their places. Power on renders a fresh frame through
a full modeset. Connectors report `:power`, and wlr-output-power-management
clients such as `wlopm` drive the same state. A session lock does not wait on a
dark output.
The native ABI is 38; the additive inspect fields keep control wire version 1.

## Virtual outputs

`virtual-output` declares a display that exists only inside the compositor. It
renders offscreen on the session's GPU and renderer, and capture is the only
way to see it, which suits a headset streamer or a remote viewer:

```lisp
(define-extension "headset" () (snapshot state event)
  (declare (ignore snapshot event))
  (values state
          (list (virtual-output "VIRTUAL-1" :mode '(2560 1440 144))
                (virtual-output "VIRTUAL-2" :mode '(3840 2160 60))
                (configure-output "VIRTUAL-2" :scale 2 :position '(2560 0))
                (configure-output "DP-4" :disabled t))
          nil))
```

`:mode` is `'(WIDTH HEIGHT)` or `'(WIDTH HEIGHT HZ)`: any size up to 16384 and
any refresh from 1 to 1000 Hz, 60 Hz when omitted. Without `:mode` the output
is 1920x1080 at 60 Hz. The name is any string no other output uses; a taken
name is refused with an error in `inspect`'s `:native-errors`.

A declared output arrives like a monitor being plugged in. Once the
transaction that declares it commits, it joins `:connectors`, settles through
the same admission as a hotplugged connector, then appears in `:outputs` with
wl_output and xdg-output globals under its name. `configure-output` on that
name sets its position, scale, transform and mode or disables it, as for any
connector, and the declared mode is the baseline that removing such an owner
restores. Changing the declared mode resizes the same output in place. When
several owners declare one name, the latest mounted wins and removal restores
the earlier declaration. Removing the declaration, or unmounting its extension,
unplugs the output the way a hot-unplug does: it leaves `:outputs`, and
policies place their windows on what remains.

Each virtual output has its own frame clock, which ticks on a fixed grid at
its refresh rate like a vblank. A frame is drawn only when something changed,
and presentation feedback reports the tick it was shown on. The cursor is
drawn in software, so wlr-screencopy and ext-image-copy-capture with cursors
include it. Virtual outputs sit beside DRM, nested and headless ones, and with
every connector disabled they can be a session's only outputs.
`HEADLESS-1` is the same kind of output, created by the headless backend
instead of a declaration. `examples/headset.lisp` declares two and turns
every other connector off while it is mounted.
