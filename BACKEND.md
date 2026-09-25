# Backend: the wlroots map

Phase 3 of replacing wlroots: DRM/KMS, libinput, libseat, the outputs they
feed, and the wlroots types every module still passes around. After phase 2
Tomoe serves every protocol except wl_output and xdg-output, so what remains is
the machinery under them.

## What still comes from wlroots

| Area | wlroots pieces | Tomoe users |
|---|---|---|
| Backends | `wlr_backend_autocreate`, DRM (atomic, legacy, modifiers, hotplug, cursor planes, gamma, VRR, async flips), headless, Wayland nested, multi-output `wlr_backend_test`/`commit` | backend.c, output.c `commit_outputs`, `output_frame` |
| Session | `wlr_session` (libseat, VT switch, device open) | backend.c, input.c VT keys |
| Outputs | `wlr_output`, `wlr_output_state`, modes, frame/needs_frame/present/request_state/commit events, `wlr_output_cursor`, software cursors, wl_output global, `wlr_output_layout`, xdg-output | output.c (120 uses), space.c, capture.c, gamma.c, surface.c enter/leave |
| Input devices | libinput backend, `wlr_input_device`, `wlr_keyboard` (xkb state, keymap fd, repeat, LEDs), `wlr_pointer` events | input.c (78 uses), libinput.c, virtual.c |
| Cursor | `wlr_cursor` (device mapping, absolute motion, image on every output), `wlr_xcursor_manager` | input.c, virtual.c |
| Render types | `wlr_renderer`/`wlr_texture`/`wlr_render_pass`/`wlr_allocator` interfaces render.c implements, `wlr_buffer` locks and release, `wlr_addon` | render.c, effects.c, capture.c, surface.c, buffer.c, output.c |
| Utilities | `wlr_box`, `wlr_fbox`, region scale/transform, output transform helpers, `wlr_drm_format_set`, `wlr_drm_syncobj_timeline`, `wlr_log` | everywhere |
| xdg positioner | `wlr_xdg_positioner_rules_get_geometry`, `_unconstrain_box` | xdg_shell.c |

## Order

wlroots couples these at `wlr_output`: its backends consume `wlr_buffer`,
`wlr_allocator` and `wlr_renderer`, and hand out `wlr_output` and input devices.
So the cut goes bottom-up in three steps, each leaving the DRM session usable.

1. **Input.** Done. libinput through a context whose devices open via the
   existing session, keymaps and xkb state in Tomoe's `keymap_slot` instead of
   `wlr_keyboard`, pointer events as plain structs into input.c, which already
   computed the cursor position. Virtual keyboard and pointer feed the same
   device path. The nested backend's host keyboard and pointer still arrive as
   wlroots devices; an adapter in libinput.c forwards them until step 2
   removes it. Cursor images stay on `wlr_cursor`, which no longer sees any
   input device, until outputs move.
2. **Outputs and backends.** Done in code; the DRM path awaits a hardware
   session. A Tomoe display core (output, modes, state,
   test/commit, frame scheduling, present feedback, cursors, wl_output,
   xdg-output) with three backends: DRM/KMS on libseat, headless, and nested
   Wayland. DRM does atomic commits with test commits, falls back from explicit
   modifiers to implicit and from atomic to legacy, handles hotplug through
   udev, VT switching through libseat, gamma LUTs, VRR, async flips, cursor
   planes and direct scanout, and passes the syncobj acquire point as the
   plane's in-fence. The step-1 adapter goes away here.
3. **Types.** render.c stops implementing wlroots interfaces: Tomoe buffer,
   texture and render calls, its own format sets, syncobj timelines, boxes,
   regions and positioner math. wlroots leaves flake.nix and dev.sh.

## Invariants carried forward

The output buffer ring keeps holding presented buffers one extra frame, every
composited frame keeps its render-done fence on the flip, and client syncobj
acquire and release points are honored on the composited and direct-scanout
paths. Step 2 moves those from `wlr_output_state` into the KMS commit itself:
the render fence becomes the primary plane's `IN_FENCE_FD`, a scanned-out
client buffer's acquire point becomes its in-fence, and its release point is
signalled when the page flip that replaces it completes.

## Size

wlroots' DRM backend is about 7,000 lines, its output code 3,000, libinput and
cursor code another 3,500. Tomoe needs one GPU vendor path verified on this
machine and loud failure elsewhere, not wlroots' coverage; the target is under
5,000 lines for all three steps, with each wlroots path deleted in the step
that replaces it.
