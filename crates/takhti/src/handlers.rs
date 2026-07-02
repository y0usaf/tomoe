use smithay::backend::allocator::dmabuf::Dmabuf;
use smithay::backend::renderer::utils::{on_commit_buffer_handler, with_renderer_surface_state};
use smithay::backend::renderer::ImportDma;
use smithay::desktop::{layer_map_for_output, LayerSurface, PopupKind, Window, WindowSurfaceType};
use smithay::output::Output;
use smithay::input::pointer::CursorImageStatus;
use smithay::input::{Seat, SeatHandler, SeatState};
use smithay::reexports::wayland_server::protocol::wl_buffer::WlBuffer;
use smithay::reexports::wayland_server::protocol::wl_output::WlOutput;
use smithay::reexports::wayland_server::protocol::wl_seat::WlSeat;
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::reexports::wayland_server::{Client, Resource};
use smithay::utils::{Serial, SERIAL_COUNTER};
use smithay::wayland::buffer::BufferHandler;
use smithay::reexports::calloop::Interest;
use smithay::wayland::compositor::{
    add_blocker, add_pre_commit_hook, get_parent, is_sync_subsurface, with_states,
    BufferAssignment, CompositorClientState, CompositorHandler, CompositorState,
    SurfaceAttributes,
};
use smithay::wayland::dmabuf::{
    get_dmabuf, DmabufGlobal, DmabufHandler, DmabufState, ImportNotifier,
};
use smithay::wayland::drm_syncobj::{DrmSyncobjCachedState, DrmSyncobjHandler, DrmSyncobjState};
use smithay::wayland::output::OutputHandler;
use smithay::wayland::selection::data_device::{
    set_data_device_focus, DataDeviceHandler, DataDeviceState, WaylandDndGrabHandler,
};
use smithay::wayland::selection::SelectionHandler;
use smithay::wayland::shell::wlr_layer::{
    KeyboardInteractivity, Layer, LayerSurface as WlrLayerSurface, LayerSurfaceCachedState,
    LayerSurfaceData, WlrLayerShellHandler, WlrLayerShellState,
};
use smithay::reexports::wayland_protocols::xdg::decoration::zv1::server::zxdg_toplevel_decoration_v1;
use smithay::reexports::wayland_protocols_misc::server_decoration::server::org_kde_kwin_server_decoration::{
    self, OrgKdeKwinServerDecoration,
};
use smithay::reexports::wayland_server::WEnum;
use smithay::wayland::shell::kde::decoration::{KdeDecorationHandler, KdeDecorationState};
use smithay::wayland::shell::xdg::decoration::XdgDecorationHandler;
use smithay::wayland::shell::xdg::{
    PopupSurface, PositionerState, ToplevelSurface, XdgShellHandler, XdgShellState,
    XdgToplevelSurfaceData,
};
use smithay::wayland::shm::{ShmHandler, ShmState};
use smithay::input::dnd::DndGrabHandler;
use smithay::wayland::fractional_scale::FractionalScaleHandler;
use smithay::{
    delegate_compositor, delegate_data_device, delegate_dmabuf, delegate_drm_syncobj,
    delegate_fractional_scale, delegate_kde_decoration, delegate_layer_shell, delegate_output,
    delegate_seat, delegate_shm, delegate_viewporter, delegate_xdg_decoration,
    delegate_xdg_shell,
};
use tracing::{debug, trace, warn};

use crate::state::{ClientState, Takhti};

// ─── wl_compositor ────────────────────────────────────────────────────────────

impl CompositorHandler for Takhti {
    fn compositor_state(&mut self) -> &mut CompositorState {
        &mut self.compositor_state
    }

    fn client_compositor_state<'a>(&self, client: &'a Client) -> &'a CompositorClientState {
        &client.get_data::<ClientState>().unwrap().compositor_state
    }

    fn new_surface(&mut self, surface: &WlSurface) {
        // Gate dmabuf commits on buffer readiness so we never sample a buffer
        // whose GPU work hasn't finished: the client's explicit-sync acquire
        // point when it uses linux-drm-syncobj, else the dmabuf's implicit
        // fences. Without this, drivers that need explicit sync (NVIDIA) show
        // stalls/freezes in GPU-heavy clients.
        add_pre_commit_hook::<Self, _>(surface, move |takhti, _dh, surface| {
            let mut acquire_point = None;
            let maybe_dmabuf = with_states(surface, |states| {
                acquire_point.clone_from(
                    &states
                        .cached_state
                        .get::<DrmSyncobjCachedState>()
                        .pending()
                        .acquire_point,
                );
                states
                    .cached_state
                    .get::<SurfaceAttributes>()
                    .pending()
                    .buffer
                    .as_ref()
                    .and_then(|assignment| match assignment {
                        BufferAssignment::NewBuffer(buffer) => get_dmabuf(buffer).cloned().ok(),
                        _ => None,
                    })
            });
            let Some(dmabuf) = maybe_dmabuf else { return };
            let Some(client) = surface.client() else { return };
            let sid = surface.id().protocol_id();
            let unblock = move |takhti: &mut Takhti| {
                let dh = takhti.display_handle.clone();
                takhti
                    .client_compositor_state(&client)
                    .blocker_cleared(takhti, &dh);
            };
            if let Some(acquire_point) = acquire_point {
                if let Ok((blocker, source)) = acquire_point.generate_blocker() {
                    let res = takhti.loop_handle.insert_source(source, move |_, _, takhti| {
                        debug!("surface {sid}: acquire fence signaled, unblocking");
                        unblock(takhti);
                        Ok(())
                    });
                    if res.is_ok() {
                        debug!("surface {sid}: commit blocked on explicit-sync acquire point");
                        add_blocker(surface, blocker);
                    } else {
                        warn!("surface {sid}: failed to insert acquire fence source");
                    }
                    return;
                }
                warn!("surface {sid}: failed to create acquire point blocker");
            }
            if let Ok((blocker, source)) = dmabuf.generate_blocker(Interest::READ) {
                let res = takhti.loop_handle.insert_source(source, move |_, _, takhti| {
                    debug!("surface {sid}: implicit fences signaled, unblocking");
                    unblock(takhti);
                    Ok(())
                });
                if res.is_ok() {
                    debug!("surface {sid}: commit blocked on implicit dmabuf fences");
                    add_blocker(surface, blocker);
                }
            }
        });
    }

    fn commit(&mut self, surface: &WlSurface) {
        trace!("commit applied for surface {}", surface.id().protocol_id());
        on_commit_buffer_handler::<Self>(surface);

        if !is_sync_subsurface(surface) {
            let mut root = surface.clone();
            while let Some(parent) = get_parent(&root) {
                root = parent;
            }
            if let Some(window) = self.window_for_surface(&root) {
                window.on_commit();
            }
        }

        self.popups.commit(surface);
        self.xdg_shell_handle_commit(surface);
        self.layer_shell_handle_commit(surface);

        // Client content changed; the damage tracker decides what repaints.
        self.queue_redraw_all();
    }
}
delegate_compositor!(Takhti);

impl BufferHandler for Takhti {
    fn buffer_destroyed(&mut self, _buffer: &WlBuffer) {}
}

impl ShmHandler for Takhti {
    fn shm_state(&self) -> &ShmState {
        &self.shm_state
    }
}
delegate_shm!(Takhti);

// ─── xdg-shell ────────────────────────────────────────────────────────────────

impl XdgShellHandler for Takhti {
    fn xdg_shell_state(&mut self) -> &mut XdgShellState {
        &mut self.xdg_shell_state
    }

    fn new_toplevel(&mut self, surface: ToplevelSurface) {
        let window = Window::new_wayland_window(surface);
        self.unmapped_windows.push(window);
    }

    fn new_popup(&mut self, surface: PopupSurface, _positioner: PositionerState) {
        if let Err(err) = self.popups.track_popup(PopupKind::Xdg(surface)) {
            warn!("error tracking popup: {err:?}");
        }
    }

    fn grab(&mut self, _surface: PopupSurface, _seat: WlSeat, _serial: Serial) {
        // Popup grabs: Phase 3.
    }

    fn reposition_request(
        &mut self,
        surface: PopupSurface,
        positioner: PositionerState,
        token: u32,
    ) {
        surface.with_pending_state(|state| {
            state.geometry = positioner.get_geometry();
            state.positioner = positioner;
        });
        surface.send_repositioned(token);
    }

    fn move_request(&mut self, _surface: ToplevelSurface, _seat: WlSeat, _serial: Serial) {
        // Interactive move: Phase 3 (grabs).
    }

    fn maximize_request(&mut self, surface: ToplevelSurface) {
        // Tiled layout: just ack so the client doesn't hang waiting.
        surface.send_configure();
    }

    fn toplevel_destroyed(&mut self, surface: ToplevelSurface) {
        self.unmapped_windows
            .retain(|w| w.toplevel().map(|t| t.wl_surface()) != Some(surface.wl_surface()));
        let window = self
            .windows
            .values()
            .find(|w| w.toplevel().map(|t| t.wl_surface()) == Some(surface.wl_surface()))
            .cloned();
        if let Some(window) = window {
            self.window_closed(&window);
        }
    }
}
delegate_xdg_shell!(Takhti);

// ─── xdg-decoration / kde-server-decoration ───────────────────────────────────
//
// Windows are tiled and the compositor draws the borders, so we prefer
// server-side decorations: clients that honor either protocol (Firefox,
// GTK/Qt apps, ...) skip their own titlebar instead of drawing one that
// ignores the layout's uniform look.

impl XdgDecorationHandler for Takhti {
    fn new_decoration(&mut self, toplevel: ToplevelSurface) {
        toplevel.with_pending_state(|state| {
            state.decoration_mode = Some(zxdg_toplevel_decoration_v1::Mode::ServerSide);
        });
        // Usually this rides along with the initial configure; only flush it
        // ourselves when the client decorated an already-configured toplevel.
        if toplevel.is_initial_configure_sent() {
            toplevel.send_pending_configure();
        }
    }

    fn request_mode(&mut self, toplevel: ToplevelSurface, mode: zxdg_toplevel_decoration_v1::Mode) {
        // Grant whatever the client asks for rather than forcing our
        // preference — forcing a mode mid-handshake breaks older SDL2 apps
        // (https://github.com/libsdl-org/SDL/issues/8173), and a client that
        // insists on CSD will draw it regardless.
        toplevel.with_pending_state(|state| {
            state.decoration_mode = Some(mode);
        });
        if toplevel.is_initial_configure_sent() {
            toplevel.send_pending_configure();
        }
    }

    fn unset_mode(&mut self, toplevel: ToplevelSurface) {
        toplevel.with_pending_state(|state| {
            state.decoration_mode = Some(zxdg_toplevel_decoration_v1::Mode::ServerSide);
        });
        if toplevel.is_initial_configure_sent() {
            toplevel.send_pending_configure();
        }
    }
}
delegate_xdg_decoration!(Takhti);

impl KdeDecorationHandler for Takhti {
    fn kde_decoration_state(&self) -> &KdeDecorationState {
        &self.kde_decoration_state
    }

    fn request_mode(
        &mut self,
        _surface: &WlSurface,
        decoration: &OrgKdeKwinServerDecoration,
        mode: WEnum<org_kde_kwin_server_decoration::Mode>,
    ) {
        // Same policy as xdg-decoration: acknowledge the client's choice.
        if let WEnum::Value(mode) = mode {
            decoration.mode(mode);
        }
    }
}
delegate_kde_decoration!(Takhti);

impl Takhti {
    /// Handle the xdg-shell part of a surface commit: initial configure and
    /// mapping toplevels once their first buffer arrives.
    pub fn xdg_shell_handle_commit(&mut self, surface: &WlSurface) {
        if let Some(idx) = self
            .unmapped_windows
            .iter()
            .position(|w| w.toplevel().map(|t| t.wl_surface()) == Some(surface))
        {
            let toplevel = self.unmapped_windows[idx].toplevel().unwrap().clone();
            let initial_configure_sent = with_states(surface, |states| {
                states
                    .data_map
                    .get::<XdgToplevelSurfaceData>()
                    .unwrap()
                    .lock()
                    .unwrap()
                    .initial_configure_sent
            });
            if !initial_configure_sent {
                toplevel.send_configure();
                return;
            }
            let has_buffer =
                with_renderer_surface_state(surface, |state| state.buffer().is_some())
                    .unwrap_or(false);
            if has_buffer {
                let window = self.unmapped_windows.remove(idx);
                self.add_window(window);
            }
            return;
        }

        // Initial configure for popups.
        if let Some(popup) = self.popups.find_popup(surface) {
            if let PopupKind::Xdg(popup) = &popup {
                if !popup.is_initial_configure_sent() {
                    let _ = popup.send_configure();
                }
            }
        }
    }

    /// Handle the layer-shell part of a surface commit.
    pub fn layer_shell_handle_commit(&mut self, surface: &WlSurface) {
        let outputs: Vec<Output> = self.space.outputs().cloned().collect();
        for output in outputs {
            let layer = layer_map_for_output(&output)
                .layer_for_surface(surface, WindowSurfaceType::TOPLEVEL)
                .cloned();
            let Some(layer) = layer else { continue };

            let initial_configure_sent = with_states(surface, |states| {
                states
                    .data_map
                    .get::<LayerSurfaceData>()
                    .map(|data| data.lock().unwrap().initial_configure_sent)
            })
            .unwrap_or(true);

            layer_map_for_output(&output).arrange();
            if !initial_configure_sent {
                layer.layer_surface().send_configure();
            } else {
                // Give keyboard focus to layers that ask for it (launchers).
                let interactivity = with_states(surface, |states| {
                    let mut guard = states.cached_state.get::<LayerSurfaceCachedState>();
                    guard.current().keyboard_interactivity
                });
                if matches!(
                    interactivity,
                    KeyboardInteractivity::Exclusive | KeyboardInteractivity::OnDemand
                ) {
                    let serial = SERIAL_COUNTER.next_serial();
                    if let Some(keyboard) = self.seat.get_keyboard() {
                        keyboard.set_focus(self, Some(surface.clone()), serial);
                    }
                }
            }
            // Exclusive zones may have changed; Lua is notified only if so.
            self.outputs_changed(false);
            return;
        }
    }
}

// ─── wlr-layer-shell ──────────────────────────────────────────────────────────

impl WlrLayerShellHandler for Takhti {
    fn shell_state(&mut self) -> &mut WlrLayerShellState {
        &mut self.layer_shell_state
    }

    fn new_layer_surface(
        &mut self,
        surface: WlrLayerSurface,
        wl_output: Option<WlOutput>,
        _layer: Layer,
        namespace: String,
    ) {
        let output = wl_output
            .as_ref()
            .and_then(Output::from_resource)
            .or_else(|| self.space.outputs().next().cloned());
        let Some(output) = output else {
            warn!("no output for new layer surface");
            return;
        };
        let layer = LayerSurface::new(surface, namespace);
        crate::state::send_scale(layer.wl_surface(), self.space.scale());
        if let Err(err) = layer_map_for_output(&output).map_layer(&layer) {
            warn!("error mapping layer surface: {err}");
        }
        self.outputs_changed(false);
    }

    fn layer_destroyed(&mut self, surface: WlrLayerSurface) {
        for output in self.space.outputs().cloned().collect::<Vec<_>>() {
            let mut map = layer_map_for_output(&output);
            let layer = map
                .layers()
                .find(|l| l.layer_surface() == &surface)
                .cloned();
            if let Some(layer) = layer {
                map.unmap_layer(&layer);
            }
        }
        // Return keyboard focus to the topmost window, then notify Lua.
        let next = self.space.elements().next_back().cloned();
        self.focus_window(next.as_ref());
        self.outputs_changed(false);
    }
}
delegate_layer_shell!(Takhti);

// ─── seat / input focus ───────────────────────────────────────────────────────

impl SeatHandler for Takhti {
    type KeyboardFocus = WlSurface;
    type PointerFocus = WlSurface;
    type TouchFocus = WlSurface;

    fn seat_state(&mut self) -> &mut SeatState<Takhti> {
        &mut self.seat_state
    }

    fn cursor_image(&mut self, _seat: &Seat<Self>, image: CursorImageStatus) {
        self.cursor_status = image;
        self.queue_redraw_all();
    }

    fn focus_changed(&mut self, seat: &Seat<Self>, focused: Option<&WlSurface>) {
        let dh = &self.display_handle;
        let client = focused.and_then(|s| dh.get_client(s.id()).ok());
        set_data_device_focus(dh, seat, client);
    }
}
delegate_seat!(Takhti);

// ─── selection / data device ──────────────────────────────────────────────────

impl SelectionHandler for Takhti {
    type SelectionUserData = ();
}

impl DataDeviceHandler for Takhti {
    fn data_device_state(&mut self) -> &mut DataDeviceState {
        &mut self.data_device_state
    }
}

impl WaylandDndGrabHandler for Takhti {}
impl DndGrabHandler for Takhti {}
delegate_data_device!(Takhti);

// ─── outputs ──────────────────────────────────────────────────────────────────

impl OutputHandler for Takhti {}
delegate_output!(Takhti);

// ─── wp-viewporter / wp-fractional-scale ──────────────────────────────────────

impl FractionalScaleHandler for Takhti {
    fn new_fractional_scale(&mut self, surface: WlSurface) {
        // Tell the client the exact scale up front so its very first buffer
        // is already at native pixel density.
        crate::state::send_scale(&surface, self.space.scale());
    }
}
delegate_fractional_scale!(Takhti);
delegate_viewporter!(Takhti);

// ─── dmabuf ───────────────────────────────────────────────────────────────────

impl DmabufHandler for Takhti {
    fn dmabuf_state(&mut self) -> &mut DmabufState {
        &mut self.dmabuf_state
    }

    fn dmabuf_imported(
        &mut self,
        _global: &DmabufGlobal,
        dmabuf: Dmabuf,
        notifier: ImportNotifier,
    ) {
        let Some(renderer) = self.backend.renderer() else {
            notifier.failed();
            return;
        };
        if renderer.import_dmabuf(&dmabuf, None).is_ok() {
            let _ = notifier.successful::<Takhti>();
        } else {
            notifier.failed();
        }
    }
}
delegate_dmabuf!(Takhti);

// ─── linux-drm-syncobj (explicit sync) ────────────────────────────────────────

impl DrmSyncobjHandler for Takhti {
    fn drm_syncobj_state(&mut self) -> Option<&mut DrmSyncobjState> {
        self.syncobj_state.as_mut()
    }
}
delegate_drm_syncobj!(Takhti);
