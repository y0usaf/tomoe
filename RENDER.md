# Renderer and output buffers: the wlroots map

Phase 1 of replacing wlroots. This maps every `wlr_renderer`, `wlr_render_pass`,
`wlr_texture`, `wlr_allocator`, `wlr_swapchain` and `wlr_buffer` use in
`native/`, what depends on it, and the size of the cut.

## Tomoe call sites

| File | Use | Depends on it |
|---|---|---|
| backend.c | `wlr_renderer_autocreate`, `wlr_allocator_autocreate`, `wlr_renderer_init_wl_display` (wl_shm, linux-dmabuf v4 with feedback), `wlr_compositor_create(renderer)` | every client surface texture; shm and dmabuf globals |
| output.c `commit_outputs` | `wlr_output_init_render(allocator, renderer)` per output; `wlr_output_swapchain_manager` prepare/get/apply | modeset preflight: format and modifier fallback across all outputs in one backend test |
| output.c `output_frame` | `presented[2]` locks (hold one extra frame, 81d6762); direct scanout commits `surface->buffer` | G9 split-head corruption fix; scanout skips syncobj acquire/release |
| space.c `render_scene_buffer` | `wlr_renderer_begin_buffer_pass` with a signal timeline, `add_rect`, `add_texture`, `wlr_output_add_software_cursors_to_render_pass`, `submit`, sync-file export for `wait-frame` | every composited frame |
| space.c `render_leaf` | `wlr_surface_get_texture`; syncobj acquire as `wait_timeline`, release signalled with the output buffer | client buffers, explicit sync |
| space.c `render_presentation` | `wlr_output_configure_primary_swapchain`, `wlr_swapchain_acquire`, `wlr_allocator_create_buffer` for the cursorless capture copy, `wlr_output_state_set_wait_timeline` (c9b56d3) | frame fence the flip waits on; screencopy without cursor |
| space.c `render_presentation_cursors` | draws `wlr_output_cursor->texture` | software cursor (the session sets `WLR_NO_HARDWARE_CURSORS=1`) |
| effects.c | raw GL on the wlroots GLES2 context: `wlr_renderer_is_gles2`, `wlr_gles2_renderer_check_ext`, `wlr_gles2_renderer_get_buffer_fbo`, `wlr_gles2_texture_get_attribs`; non-GL fallback through `add_rect` | borders, shadows, blur, rounded windows |
| ui.c | `ui_pixel_buffer` (a `wlr_buffer` impl) only to call `wlr_texture_from_buffer`; `add_texture` | Lisp UI surfaces |
| screenshot.c | `wlr_allocator_create_buffer` from the output swapchain, `wlr_texture_from_buffer`, `wlr_texture_from_pixels`, `wlr_texture_read_pixels`, `add_rect`, `add_texture` | interactive and full screenshots, frozen frame |
| protocols.c | `renderer->features.timeline`, `wlr_renderer_get_drm_fd` | linux-drm-syncobj-v1 global |

## wlroots code that needs a renderer from Tomoe

These stay in wlroots during phase 1, so Tomoe's renderer and allocator must
serve them through `wlr/render/interface.h` and `wlr_allocator_interface`.

| wlroots consumer | Needs | Leaves in |
|---|---|---|
| `wlr_compositor` client buffers | `texture_from_buffer`, `update_from_buffer` for shm damage | phase 2 (wl_surface) |
| `wlr_renderer_init_wl_display`, linux-dmabuf feedback | texture formats, DRM fd | phase 2 |
| `wlr_cursor`, `wlr_output_cursor` | `texture_from_buffer`; hardware cursor renders into a cursor swapchain from `output->allocator` | phase 3 (cursor planes) |
| `wlr_screencopy_v1` | `texture_from_buffer`, `read_pixels`, a blit pass into client dmabufs | phase 2 |
| ext-image-capture output and scene sources | `output->swapchain` for constraints, `wlr_scene` rendering through the pass | phase 2 |
| DRM backend multi-GPU copy | its own internal renderer on the secondary GPU | phase 3 |

## The cut

Tomoe stops calling wlroots' GLES2 renderer, EGL setup, GBM allocator,
swapchain and swapchain manager (about 4,200 lines in wlroots 0.20). They stay
linked in libwlroots until phase 3 because wlroots' DRM backend uses them for
multi-GPU copies.

Added: `native/render.c`, one EGL/GLES2 renderer on a GBM device Tomoe opens
(the backend's render node, or the first render node when the backend has
none). It owns dmabuf and shm import with an EGLImage cache per buffer,
render targets, the rect and texture programs, acquire waits and render-done
fences as sync files on a DRM syncobj timeline, a GBM allocator, and the output
buffer ring with explicit-then-implicit modifier fallback across all outputs in
one backend test. The ring never hands out a buffer that the backend or
`presented[]` still holds. It serves wlroots through the interfaces above and
has no second implementation. Estimate: +650 lines. Actual: 1,065 lines,
including the 120 lines of shaders moved from effects.c.

Removed: the wlroots renderer and allocator autocreate, the swapchain manager,
`wlr_output_configure_primary_swapchain` and `wlr_swapchain_acquire` calls,
the GLES2 introspection in effects.c, the non-GL border fallback, and
`ui_pixel_buffer`. Estimate: -120 lines. Actual: the whole change is +1,201
and -342 lines, or +859 net.

Dropped: the pixman renderer that the headless backend used. Headless now
renders with GL on a render node and fails loudly when there is none.

Direct scanout gains the syncobj acquire wait and release signal that the
composited path already had.
