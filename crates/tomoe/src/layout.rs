//! Native layout mechanisms. Layout *policy* lives in Lua (see
//! resources/wm.lua); this module only keeps rendering-adjacent bookkeeping.

use crate::state::Tomoe;

impl Tomoe {
    /// Update persistent per-window shader borders for mapped windows. Runs
    /// on every render path so size, focus color, width, and radius match the
    /// latest committed geometry. Unchanged parameters preserve the commit.
    pub fn refresh_borders(&mut self) {
        let settings = self.lua.settings();
        let width = settings.border_width.max(0);
        let radius = settings.corner_radius.max(0);
        let focused = self.focused_window();
        // Corner radius is a shader uniform — invisible to damage tracking —
        // so a changed setting bumps every window's damage-injection element
        // exactly once (rendered by scene_elements when rounding is on).
        if radius != self.applied_corner_radius {
            self.applied_corner_radius = radius;
            for damage in self.corner_damage.values_mut() {
                damage.damage_all();
            }
        }
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
            self.corner_damage.entry(window.clone()).or_default();
            let size = (geo.size.w + 2 * width, geo.size.h + 2 * width).into();
            self.borders.entry(window.clone()).or_default().update(
                size,
                color,
                width,
                radius + width,
            );
            self.shadows.entry(window.clone()).or_default().update(
                geo.size,
                settings.shadow_color,
                settings.shadow_range,
                radius,
                settings.shadow_power,
            );
        }
    }
}
