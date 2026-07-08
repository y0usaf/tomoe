//! Foreign-toplevel advertising: mapped windows published to bars, docks,
//! and the screencast portal's window enumeration.
//!
//! Two protocols share this glue:
//! - **ext-foreign-toplevel-list-v1** (read-only; smithay's state): handles
//!   published from `add_window`, retired from `window_closed`, title/app_id
//!   pushed on commit. Each handle carries the compositor window id in its
//!   user data so capture sources created from it resolve back to the
//!   window (`capture.rs::source_target`).
//! - **wlr-foreign-toplevel-management-unstable-v1** (read + control; our
//!   `protocols/wlr_foreign_toplevel.rs`): diff-refreshed once per
//!   event-loop iteration from [`Tomoe::refresh_wlr_foreign_toplevels`] —
//!   states (maximized/fullscreen/activated) and output enter/leave included
//!   — so focus changes, commits, Lua ops, and unmaps all converge on one
//!   refresh site. Control requests route through `on_window_request`
//!   policy (`handlers.rs`).

use smithay::desktop::Window;
use smithay::reexports::wayland_protocols::xdg::shell::server::xdg_toplevel;
use smithay::wayland::compositor::with_states;
use smithay::wayland::shell::xdg::XdgToplevelSurfaceData;

use crate::protocols::wlr_foreign_toplevel::ToplevelInfo;
use crate::state::Tomoe;

/// Compositor window id riding on a [`ForeignToplevelHandle`]'s user data.
pub struct ForeignWindowId(pub u64);

/// Current (app_id, title) of a window's xdg toplevel.
fn window_meta(window: &Window) -> (String, String) {
    window
        .toplevel()
        .map(|toplevel| {
            with_states(toplevel.wl_surface(), |states| {
                let data = states
                    .data_map
                    .get::<XdgToplevelSurfaceData>()
                    .unwrap()
                    .lock()
                    .unwrap();
                (
                    data.app_id.clone().unwrap_or_default(),
                    data.title.clone().unwrap_or_default(),
                )
            })
        })
        .unwrap_or_default()
}

impl Tomoe {
    /// A window mapped: announce it on the foreign-toplevel list.
    pub fn publish_foreign_toplevel(&mut self, id: u64, window: &Window) {
        let (app_id, title) = window_meta(window);
        let handle = self
            .foreign_toplevel_state
            .new_toplevel::<Self>(title, app_id);
        handle.user_data().insert_if_missing(|| ForeignWindowId(id));
        self.foreign_toplevels.insert(id, handle);
    }

    /// A mapped window committed: push any title/app_id change to listeners.
    pub fn refresh_foreign_toplevel(&mut self, window: &Window) {
        let Some(id) = self
            .windows
            .iter()
            .find(|(_, w)| *w == window)
            .map(|(id, _)| *id)
        else {
            return;
        };
        let Some(handle) = self.foreign_toplevels.get(&id) else {
            return;
        };
        let (app_id, title) = window_meta(window);
        if handle.title() == title && handle.app_id() == app_id {
            return;
        }
        handle.send_title(&title);
        handle.send_app_id(&app_id);
        handle.send_done();
    }

    /// The window is gone: send `closed` and drop the handle.
    pub fn retire_foreign_toplevel(&mut self, id: u64) {
        if let Some(handle) = self.foreign_toplevels.remove(&id) {
            self.foreign_toplevel_state.remove_toplevel(&handle);
        }
    }

    /// Diff every mapped window against the wlr-foreign-toplevel state and
    /// push the changes; called once per event-loop iteration (`main.rs`).
    /// The focused window refreshes last, so on a focus change listeners see
    /// the old window deactivate before the new one activates (niri-shape).
    pub fn refresh_wlr_foreign_toplevels(&mut self) {
        for id in self.wlr_foreign_toplevel_state.tracked_ids() {
            if !self.windows.contains_key(&id) {
                self.wlr_foreign_toplevel_state.retire(id);
            }
        }
        let focused_surface = self.seat.get_keyboard().and_then(|kb| kb.current_focus());
        let mut focused = None;
        for (id, window) in &self.windows {
            let is_focused = window.toplevel().map(|t| t.wl_surface().clone()) == focused_surface;
            if is_focused {
                focused = Some(*id);
                continue;
            }
            let info = wlr_toplevel_info(&self.space, window, false);
            self.wlr_foreign_toplevel_state
                .refresh_toplevel::<Self>(*id, info);
        }
        if let Some(id) = focused {
            if let Some(window) = self.windows.get(&id) {
                let info = wlr_toplevel_info(&self.space, window, true);
                self.wlr_foreign_toplevel_state
                    .refresh_toplevel::<Self>(id, info);
            }
        }
    }
}

/// Snapshot a window for the wlr-foreign-toplevel diff: committed xdg
/// states + the outputs its rendered rect overlaps.
fn wlr_toplevel_info(
    space: &crate::space::PhysicalSpace,
    window: &Window,
    activated: bool,
) -> ToplevelInfo {
    let (app_id, title) = window_meta(window);
    let (fullscreen, maximized) = window
        .toplevel()
        .map(|t| {
            t.with_committed_state(|state| {
                state
                    .map(|s| {
                        (
                            s.states.contains(xdg_toplevel::State::Fullscreen),
                            s.states.contains(xdg_toplevel::State::Maximized),
                        )
                    })
                    .unwrap_or_default()
            })
        })
        .unwrap_or_default();
    ToplevelInfo {
        title,
        app_id,
        maximized,
        fullscreen,
        activated,
        outputs: space.outputs_overlapping(window),
    }
}
