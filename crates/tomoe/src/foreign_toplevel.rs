//! ext-foreign-toplevel-list-v1: advertise mapped toplevels to clients
//! (bars, docks, and the screencast portal's window enumeration).
//!
//! Handles exist only for mapped windows: published from `add_window`,
//! retired from `window_closed`, title/app_id pushed on commit. Each handle
//! carries the compositor window id in its user data so capture sources
//! created from it resolve back to the window (`capture.rs::source_target`).

use smithay::desktop::Window;
use smithay::wayland::compositor::with_states;
use smithay::wayland::shell::xdg::XdgToplevelSurfaceData;

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
}
