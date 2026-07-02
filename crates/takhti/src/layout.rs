//! Native layout mechanisms. Layout *policy* lives in Lua (see
//! resources/wm.lua); this module only keeps rendering-adjacent bookkeeping.

use crate::state::Takhti;

impl Takhti {
    /// Update per-window border buffers (size + focus color) for mapped windows.
    pub fn refresh_borders(&mut self) {
        let settings = self.lua.settings();
        let width = settings.border_width;
        let focused = self.focused_window();
        let windows: Vec<_> = self.space.elements().cloned().collect();
        for window in windows {
            let Some(geo) = self.space.element_geometry(&window) else {
                continue;
            };
            let color = if Some(&window) == focused.as_ref() {
                settings.border_focused
            } else {
                settings.border_unfocused
            };
            let buffers = self
                .border_buffers
                .entry(window.clone())
                .or_insert_with(Default::default);
            // Top, bottom, left, right slabs — a hollow frame rather than one
            // full-size rect, so transparent windows don't tint all over.
            buffers[0].update((geo.size.w + 2 * width, width), color);
            buffers[1].update((geo.size.w + 2 * width, width), color);
            buffers[2].update((width, geo.size.h), color);
            buffers[3].update((width, geo.size.h), color);
        }
    }
}
