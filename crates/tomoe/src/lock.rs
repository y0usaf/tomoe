//! ext-session-lock-v1: the lock state machine and locked-scene rendering.
//!
//! The guarantee: the client's `locked` event is only sent after every
//! output has actually put a locked frame on screen, so no stale session
//! content can survive into the locked state. Progression:
//!
//! `Unlocked` → (client locks) → `WaitingForSurfaces` (grace period for the
//! client to create + commit lock surfaces, 1s deadline) → `Locking`
//! (redraw every output with the locked scene) → `Locked` (confirmation
//! sent once all outputs rendered locked).
//!
//! Dropping a [`SessionLocker`] without confirming sends `finished()` to the
//! client, so every abort path is just "drop the confirmation".

use std::collections::HashMap;
use std::mem;
use std::time::Duration;

use smithay::backend::renderer::element::solid::{SolidColorBuffer, SolidColorRenderElement};
use smithay::backend::renderer::element::surface::render_elements_from_surface_tree;
use smithay::backend::renderer::element::Kind;
use smithay::backend::renderer::utils::with_renderer_surface_state;
use smithay::input::pointer::CursorImageStatus;
use smithay::output::Output;
use smithay::reexports::calloop::timer::{TimeoutAction, Timer};
use smithay::reexports::calloop::RegistrationToken;
use smithay::reexports::wayland_protocols::ext::session_lock::v1::server::ext_session_lock_v1::ExtSessionLockV1;
use smithay::reexports::wayland_server::protocol::wl_output::WlOutput;
use smithay::reexports::wayland_server::Resource;
use smithay::utils::{Physical, Point, Size, SERIAL_COUNTER};
use smithay::wayland::session_lock::{LockSurface, SessionLocker};
use tracing::{info, warn};

use crate::coords;
use crate::render::{OutputRenderElements, TomoeRenderer};
use crate::state::Tomoe;

/// Backdrop drawn under (and instead of) the lock surface: dark red, so a
/// locked-but-unpainted screen is visibly "locked" rather than black-dead.
const LOCKED_COLOR: [f32; 4] = [0.3, 0.1, 0.1, 1.0];

/// How long lock-surface clients get to paint before we blank anyway.
const LOCK_SURFACE_DEADLINE: Duration = Duration::from_millis(1000);

#[derive(Default)]
pub enum LockState {
    #[default]
    Unlocked,
    /// A client asked to lock; outputs still show the session while we wait
    /// (bounded by the deadline) for lock surfaces to arrive, so the usual
    /// flow never flashes the backdrop.
    WaitingForSurfaces {
        confirmation: SessionLocker,
        deadline_token: RegistrationToken,
    },
    /// Rendering locked frames; confirmed once every output has one.
    Locking(SessionLocker),
    Locked(ExtSessionLockV1),
}

impl Tomoe {
    /// True from the moment locked frames start rendering. `Locking` counts:
    /// rendering, input routing, and focus must already be restricted while
    /// the confirmation is pending, or the lock could leak session content.
    pub fn is_locked(&self) -> bool {
        matches!(
            self.lock_state,
            LockState::Locking(_) | LockState::Locked(_)
        )
    }

    /// SessionLockHandler::lock — a client bound ext-session-lock and asked
    /// to lock the session.
    pub fn lock_session(&mut self, confirmation: SessionLocker) {
        if matches!(
            self.lock_state,
            LockState::WaitingForSurfaces { .. } | LockState::Locking(_)
        ) {
            info!("refusing lock: another client is currently locking");
            return;
        }
        if let LockState::Locked(lock) = &self.lock_state {
            if lock.is_alive() {
                info!("refusing lock: already locked with an active client");
                return;
            }
            // The previous locker died. Outputs are already blanked, so the
            // replacement can be confirmed immediately.
            info!("locking session (replacing a dead lock client)");
            let lock = confirmation.ext_session_lock().clone();
            confirmation.lock();
            self.lock_state = LockState::Locked(lock);
            return;
        }

        info!("locking session");
        if self.space.outputs().next().is_none() {
            // Nothing to blank; confirm right away.
            self.begin_locked_session();
            let lock = confirmation.ext_session_lock().clone();
            confirmation.lock();
            self.lock_state = LockState::Locked(lock);
            return;
        }

        let timer = Timer::from_duration(LOCK_SURFACE_DEADLINE);
        match self.loop_handle.insert_source(timer, |_, _, tomoe| {
            tomoe.continue_to_locking();
            TimeoutAction::Drop
        }) {
            Ok(deadline_token) => {
                self.lock_state = LockState::WaitingForSurfaces {
                    confirmation,
                    deadline_token,
                };
            }
            // Dropping the confirmation sends `finished()`.
            Err(err) => warn!("error scheduling lock deadline timer: {err}"),
        }
    }

    /// A lock surface was created or committed while we wait for surfaces:
    /// move on to locking once every output has a mapped one.
    pub fn maybe_continue_to_locking(&mut self) {
        if !matches!(self.lock_state, LockState::WaitingForSurfaces { .. }) {
            return;
        }
        for output in self.space.outputs() {
            let Some(surface) = self.lock_surfaces.get(output) else {
                return;
            };
            let mapped =
                with_renderer_surface_state(surface.wl_surface(), |state| state.buffer().is_some())
                    .unwrap_or(false);
            if !mapped {
                return;
            }
        }
        self.continue_to_locking();
    }

    /// Stop waiting (all surfaces arrived, or the deadline expired) and start
    /// rendering locked frames.
    pub(crate) fn continue_to_locking(&mut self) {
        match mem::take(&mut self.lock_state) {
            LockState::WaitingForSurfaces {
                confirmation,
                deadline_token,
            } => {
                self.loop_handle.remove(deadline_token);
                self.begin_locked_session();
                if self.space.outputs().next().is_none() {
                    let lock = confirmation.ext_session_lock().clone();
                    confirmation.lock();
                    self.lock_state = LockState::Locked(lock);
                } else {
                    self.lock_state = LockState::Locking(confirmation);
                    self.update_lock_focus();
                    self.queue_redraw_all();
                }
            }
            other => self.lock_state = other,
        }
    }

    /// Shared entry into the locked regime: dismiss compositor UI, end any
    /// Lua pointer grab, and reset transient input state, so nothing from
    /// the session bleeds into (or fights) the locked scene.
    fn begin_locked_session(&mut self) {
        self.ui.screenshot.close();
        // All widgets close silently (no cancel events — locking is not a
        // moment to re-enter Lua); their callbacks are dropped.
        for entry in self.ui.widgets.drain() {
            self.lua.drop_ui_callbacks(entry.id);
        }
        self.cursor_status = CursorImageStatus::default_named();
        self.hovered_window = None;
        self.lock_rendered.clear();
        if self.lua.pointer_grab_active() {
            let was_in_lua = self.in_lua;
            self.in_lua = true;
            self.lua.end_pointer_grab();
            self.in_lua = was_in_lua;
            self.after_lua();
        }
    }

    /// SessionLockHandler::unlock — the locked client called
    /// unlock_and_destroy. Also the abort path (via [`Self::fail_lock`]).
    pub fn unlock_session(&mut self) {
        info!("unlocking session");
        if let LockState::WaitingForSurfaces { deadline_token, .. } =
            mem::take(&mut self.lock_state)
        {
            self.loop_handle.remove(deadline_token);
        }
        self.lock_surfaces.clear();
        self.lock_rendered.clear();
        // Return keyboard focus to the topmost window; Lua hears about it
        // through the usual focus-change event.
        let next = self.space.elements().next_back().cloned();
        self.focus_window(next.as_ref());
        self.queue_redraw_all();
    }

    /// SessionLockHandler::new_surface — the locking client created a lock
    /// surface for `wl_output`.
    pub fn new_lock_surface(&mut self, surface: LockSurface, wl_output: &WlOutput) {
        let lock = match &self.lock_state {
            LockState::Unlocked => {
                warn!("ignoring lock surface on an unlocked session");
                return;
            }
            LockState::WaitingForSurfaces { confirmation, .. } => confirmation.ext_session_lock(),
            LockState::Locking(confirmation) => confirmation.ext_session_lock(),
            LockState::Locked(lock) => lock,
        };
        if lock.client() != surface.wl_surface().client() {
            warn!("ignoring lock surface from an unrelated client");
            return;
        }
        let Some(output) = Output::from_resource(wl_output) else {
            warn!("no Output matching the lock surface's WlOutput");
            return;
        };
        let Some(geo) = self.space.output_geometry(&output) else {
            // Output is on its way out; the client will get surfaces for the
            // survivors and the lock can complete without this one.
            return;
        };
        configure_lock_surface(&surface, geo.size, self.space.scale());
        self.lock_surfaces.insert(output, surface);
        self.update_lock_focus();
        self.maybe_continue_to_locking();
    }

    /// An output finished (or skipped) a render. While locked, track which
    /// outputs have a locked frame on screen and confirm the lock once all
    /// of them do; the flag is cleared again on unlock renders.
    pub fn lock_frame_rendered(&mut self, output: &Output) {
        if self.is_locked() {
            self.lock_rendered.insert(output.clone());
            self.confirm_lock_if_ready();
        } else {
            self.lock_rendered.remove(output);
        }
    }

    /// An output failed to render. If that output still needed its first
    /// locked frame for a pending confirmation, the lock can't be satisfied
    /// honestly — fail it (`finished()`) rather than confirming a lie.
    pub fn lock_render_failed(&mut self, output: &Output) {
        if matches!(self.lock_state, LockState::Locking(_)) && !self.lock_rendered.contains(output)
        {
            warn!("failing session lock: output failed to render a locked frame");
            self.fail_lock();
        }
    }

    /// Drop a pending confirmation (sends `finished()`) and undo lock state.
    fn fail_lock(&mut self) {
        self.lock_state = LockState::Unlocked;
        self.unlock_session();
    }

    fn confirm_lock_if_ready(&mut self) {
        match mem::take(&mut self.lock_state) {
            LockState::Locking(confirmation) => {
                let all_locked = self
                    .space
                    .outputs()
                    .all(|output| self.lock_rendered.contains(output));
                if all_locked {
                    info!("session locked");
                    let lock = confirmation.ext_session_lock().clone();
                    confirmation.lock();
                    self.lock_state = LockState::Locked(lock);
                } else {
                    self.lock_state = LockState::Locking(confirmation);
                }
            }
            other => self.lock_state = other,
        }
    }

    /// Re-sync lock bookkeeping with the current output set: prune state for
    /// gone outputs, re-configure lock surfaces to new sizes, and re-check
    /// whether a pending lock can now progress (fewer outputs may satisfy
    /// it). Called from `outputs_changed`.
    pub fn refresh_lock_state(&mut self) {
        if matches!(self.lock_state, LockState::Unlocked) {
            self.lock_surfaces.clear();
            self.lock_backdrops.clear();
            self.lock_rendered.clear();
            return;
        }
        let scale = self.space.scale();
        let live: Vec<(Output, Size<i32, Physical>)> = self
            .space
            .outputs()
            .filter_map(|o| {
                self.space
                    .output_geometry(o)
                    .map(|geo| (o.clone(), geo.size))
            })
            .collect();
        self.lock_surfaces
            .retain(|output, _| live.iter().any(|(o, _)| o == output));
        self.lock_backdrops
            .retain(|output, _| live.iter().any(|(o, _)| o == output));
        self.lock_rendered
            .retain(|output| live.iter().any(|(o, _)| o == output));
        for (output, size) in &live {
            if let Some(surface) = self.lock_surfaces.get(output) {
                configure_lock_surface(surface, *size, scale);
            }
        }
        match &self.lock_state {
            LockState::Locking(_) => self.confirm_lock_if_ready(),
            LockState::WaitingForSurfaces { .. } => self.maybe_continue_to_locking(),
            _ => {}
        }
        if self.is_locked() {
            self.update_lock_focus();
        }
    }

    /// Keyboard focus while locked: the lock surface on the pointer's output,
    /// any lock surface as a fallback, or None (input swallowed compositor-
    /// side) until the client provides one.
    pub fn update_lock_focus(&mut self) {
        if !self.is_locked() {
            return;
        }
        let output = self
            .seat
            .get_pointer()
            .map(|p| coords::point_to_physical(p.current_location(), self.space.scale()))
            .and_then(|pos| self.space.output_under(pos))
            .or_else(|| self.space.outputs().next())
            .cloned();
        let focus = output
            .and_then(|o| self.lock_surfaces.get(&o))
            .or_else(|| self.lock_surfaces.values().next())
            .map(|s| s.wl_surface().clone());
        if let Some(keyboard) = self.seat.get_keyboard() {
            keyboard.set_focus(self, focus, SERIAL_COUNTER.next_serial());
        }
    }

    /// Whether `surface` is (part of) some output's lock surface.
    pub fn is_lock_surface(
        &self,
        surface: &smithay::reexports::wayland_server::protocol::wl_surface::WlSurface,
    ) -> bool {
        self.lock_surfaces
            .values()
            .any(|lock| lock.wl_surface() == surface)
    }
}

/// Size a lock surface to fill its output and send the configure. Physical
/// output size quantizes to protocol-logical exactly like window configures.
pub fn configure_lock_surface(surface: &LockSurface, size: Size<i32, Physical>, scale: f64) {
    let (logical, _achievable) = coords::configure_size(size, scale);
    surface.with_pending_state(|states| {
        states.size = Some(Size::from((
            logical.w.max(1) as u32,
            logical.h.max(1) as u32,
        )));
    });
    crate::state::send_scale(surface.wl_surface(), scale);
    surface.send_configure();
}

/// The locked scene for one output: the lock surface (if any) over a solid
/// backdrop covering the whole output. This *replaces* the normal scene —
/// windows, layers, borders, and compositor UI are all unreachable while
/// locked. The backdrop buffers persist per output so damage trackers see
/// stable element ids; `resize` is a no-op at an unchanged size.
// Output keys hash by their stable id despite interior mutability.
#[allow(clippy::mutable_key_type)]
pub fn lock_elements<R: TomoeRenderer>(
    renderer: &mut R,
    output: &Output,
    size: Size<i32, Physical>,
    scale: f64,
    surface: Option<&LockSurface>,
    backdrops: &mut HashMap<Output, SolidColorBuffer>,
) -> Vec<OutputRenderElements<R>> {
    let mut elements = Vec::new();
    if let Some(surface) = surface {
        elements.extend(
            render_elements_from_surface_tree(
                renderer,
                surface.wl_surface(),
                Point::<i32, Physical>::from((0, 0)),
                scale,
                1.0,
                Kind::Unspecified,
            )
            .into_iter()
            .map(OutputRenderElements::Surface),
        );
    }
    let backdrop = backdrops
        .entry(output.clone())
        .or_insert_with(|| SolidColorBuffer::new((0, 0), LOCKED_COLOR));
    backdrop.resize((size.w, size.h));
    elements.push(OutputRenderElements::Solid(
        SolidColorRenderElement::from_buffer(
            backdrop,
            Point::<i32, Physical>::from((0, 0)),
            1.0,
            1.0,
            Kind::Unspecified,
        ),
    ));
    elements
}
