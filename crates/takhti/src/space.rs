//! Window/output bookkeeping in integer physical pixels.
//!
//! Replaces `smithay::desktop::Space`, which stores integer *logical*
//! coordinates. Logical integers land on fractional physical positions at
//! fractional scales, and independently-rounded elements drift a pixel apart
//! (blurry buffers, border seams). Here everything is `Point<i32, Physical>`,
//! so grid alignment is guaranteed by the type, not by rounding discipline.
//! Conversions to protocol-logical happen in `crate::coords` only.

use smithay::desktop::space::SpaceElement;
use smithay::desktop::Window;
use smithay::output::Output;
use smithay::utils::{IsAlive, Physical, Point, Rectangle};

use crate::coords;

#[derive(Default)]
pub struct PhysicalSpace {
    /// Uniform output scale (snapped to N/120). Uniform-for-now: per-output
    /// scales only require storing this per entry in `outputs`.
    scale: f64,
    outputs: Vec<(Output, Point<i32, Physical>)>,
    /// Mapped windows with their geometry origin, bottom → top.
    windows: Vec<(Window, Point<i32, Physical>)>,
}

impl PhysicalSpace {
    pub fn new() -> Self {
        Self {
            scale: 1.0,
            outputs: Vec::new(),
            windows: Vec::new(),
        }
    }

    pub fn scale(&self) -> f64 {
        self.scale
    }

    pub fn set_scale(&mut self, scale: f64) {
        self.scale = coords::snap_scale(scale);
    }

    // ── Outputs ──

    pub fn map_output(&mut self, output: &Output, loc: impl Into<Point<i32, Physical>>) {
        let loc = loc.into();
        if let Some(entry) = self.outputs.iter_mut().find(|(o, _)| o == output) {
            entry.1 = loc;
        } else {
            self.outputs.push((output.clone(), loc));
        }
    }

    pub fn outputs(&self) -> impl DoubleEndedIterator<Item = &Output> {
        self.outputs.iter().map(|(o, _)| o)
    }

    /// Output rect in physical pixels: mapped position + transformed mode size.
    pub fn output_geometry(&self, output: &Output) -> Option<Rectangle<i32, Physical>> {
        let loc = self
            .outputs
            .iter()
            .find(|(o, _)| o == output)
            .map(|(_, loc)| *loc)?;
        let mode = output.current_mode()?;
        let size = output.current_transform().transform_size(mode.size);
        Some(Rectangle::new(loc, size))
    }

    pub fn output_under(&self, pos: Point<f64, Physical>) -> Option<&Output> {
        self.outputs.iter().map(|(o, _)| o).find(|output| {
            self.output_geometry(output)
                .is_some_and(|geo| geo.to_f64().contains(pos))
        })
    }

    // ── Windows ──

    /// Map a window at `loc`, or move it if already mapped. New windows go on
    /// top; moving preserves stacking order.
    pub fn map_element(&mut self, window: Window, loc: impl Into<Point<i32, Physical>>) {
        let loc = loc.into();
        if let Some(entry) = self.windows.iter_mut().find(|(w, _)| *w == window) {
            entry.1 = loc;
        } else {
            self.windows.push((window, loc));
        }
    }

    pub fn unmap(&mut self, window: &Window) {
        if let Some(idx) = self.windows.iter().position(|(w, _)| w == window) {
            let (window, _) = self.windows.remove(idx);
            for (output, _) in &self.outputs {
                window.output_leave(output);
            }
        }
    }

    /// Mapped windows, bottom → top.
    pub fn elements(&self) -> impl DoubleEndedIterator<Item = &Window> {
        self.windows.iter().map(|(w, _)| w)
    }

    pub fn raise_element(&mut self, window: &Window) {
        if let Some(idx) = self.windows.iter().position(|(w, _)| w == window) {
            let entry = self.windows.remove(idx);
            self.windows.push(entry);
        }
    }

    pub fn element_location(&self, window: &Window) -> Option<Point<i32, Physical>> {
        self.windows
            .iter()
            .find(|(w, _)| w == window)
            .map(|(_, loc)| *loc)
    }

    /// Window rect in physical pixels. The size derives from the client's
    /// committed geometry (logical), rounded once onto the grid — the same
    /// rounding the renderer applies, so layout and sampling always agree.
    pub fn element_geometry(&self, window: &Window) -> Option<Rectangle<i32, Physical>> {
        let loc = self.element_location(window)?;
        let size = coords::logical_size_to_physical(window.geometry().size.to_f64(), self.scale);
        Some(Rectangle::new(loc, size))
    }

    /// Topmost window whose rect contains `pos`, with its location.
    pub fn element_under(&self, pos: Point<f64, Physical>) -> Option<(&Window, Point<i32, Physical>)> {
        self.windows.iter().rev().find_map(|(window, loc)| {
            self.element_geometry(window)
                .is_some_and(|geo| geo.to_f64().contains(pos))
                .then_some((window, *loc))
        })
    }

    /// Drop dead windows, refresh output enter/leave, and let windows update
    /// their internal state (mirrors `Space::refresh`).
    pub fn refresh(&mut self) {
        self.windows.retain(|(w, _)| w.alive());
        for (window, _) in &self.windows {
            let Some(geo) = self.element_geometry(window) else {
                continue;
            };
            for (output, _) in &self.outputs {
                let Some(output_geo) = self.output_geometry(output) else {
                    continue;
                };
                if let Some(mut overlap) = output_geo.intersection(geo) {
                    // output_enter expects the overlap relative to the window.
                    overlap.loc -= geo.loc;
                    window.output_enter(output, coords::rect_to_logical(overlap, self.scale));
                } else {
                    window.output_leave(output);
                }
            }
            SpaceElement::refresh(window);
        }
    }
}
