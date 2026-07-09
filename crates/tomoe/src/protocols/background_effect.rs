//! ext-background-effect-v1 blur-region support.
//!
//! The protocol stores a double-buffered region on each wl_surface. Rendering
//! reads it after the owning surface commits, normalizes ordered add/subtract
//! operations into non-overlapping rectangles, and clips them to the surface.

use std::cmp::{max, min};
use std::collections::BTreeSet;

use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::utils::{Logical, Rectangle};
use smithay::wayland::background_effect::{
    self, BackgroundEffectSurfaceCachedState, ExtBackgroundEffectHandler,
};
use smithay::wayland::compositor::{with_states, RectangleKind, RegionAttributes};

use crate::state::Tomoe;

impl ExtBackgroundEffectHandler for Tomoe {
    fn capabilities(&self) -> background_effect::Capability {
        background_effect::Capability::Blur
    }
}

/// Return the committed protocol blur region, clipped to `bounds` in
/// surface-local logical coordinates. `None` means the client did not set a
/// region; `Some([])` is an explicitly empty region.
pub fn blur_region_rects(
    surface: &WlSurface,
    bounds: Rectangle<i32, Logical>,
) -> Option<Vec<Rectangle<i32, Logical>>> {
    let region = with_states(surface, |states| {
        let mut cached = states
            .cached_state
            .get::<BackgroundEffectSurfaceCachedState>();
        cached.current().blur_region.clone()
    })?;

    Some(
        region_to_non_overlapping_rects(&region)
            .into_iter()
            .filter_map(|rect| rect.intersection(bounds))
            .collect(),
    )
}

/// Evaluate an ordered Wayland region into non-overlapping rectangles.
fn region_to_non_overlapping_rects(region: &RegionAttributes) -> Vec<Rectangle<i32, Logical>> {
    let ys = BTreeSet::from_iter(
        region
            .rects
            .iter()
            .flat_map(|(_, rect)| [rect.loc.y, rect.loc.y.saturating_add(rect.size.h)]),
    );
    let mut ys = ys.into_iter();
    let Some(mut lo) = ys.next() else {
        return Vec::new();
    };

    let mut output = Vec::new();
    let mut spans = Vec::<(i32, i32)>::new();
    for hi in ys {
        spans.clear();
        'region: for (kind, rect) in &region.rects {
            if hi <= rect.loc.y || rect.loc.y.saturating_add(rect.size.h) <= lo {
                continue;
            }
            let mut x1 = rect.loc.x;
            let mut x2 = rect.loc.x.saturating_add(rect.size.w);
            if x1 == x2 {
                continue;
            }
            match kind {
                RectangleKind::Add => {
                    for i in (0..spans.len()).rev() {
                        let (start, end) = spans[i];
                        if end < x1 {
                            spans.insert(i + 1, (x1, x2));
                            continue 'region;
                        }
                        if x2 < start {
                            continue;
                        }
                        spans.remove(i);
                        x1 = min(x1, start);
                        x2 = max(x2, end);
                    }
                    spans.insert(0, (x1, x2));
                }
                RectangleKind::Subtract => {
                    for i in (0..spans.len()).rev() {
                        let (start, end) = spans[i];
                        if end <= x1 {
                            continue 'region;
                        }
                        if x2 <= start {
                            continue;
                        }
                        spans.remove(i);
                        if x2 < end {
                            spans.insert(i, (x2, end));
                        }
                        if start < x1 {
                            spans.insert(i, (start, x1));
                        }
                    }
                }
            }
        }
        for (x1, x2) in spans.drain(..) {
            output.push(Rectangle::from_extremities((x1, lo), (x2, hi)));
        }
        lo = hi;
    }
    output
}

smithay::delegate_background_effect!(Tomoe);

#[cfg(test)]
mod tests {
    use smithay::utils::{Point, Size};

    use super::*;

    #[test]
    fn ordered_subtraction_produces_non_overlapping_rects() {
        let region = RegionAttributes {
            rects: vec![
                (
                    RectangleKind::Add,
                    Rectangle::new(Point::from((0, 0)), Size::from((100, 50))),
                ),
                (
                    RectangleKind::Subtract,
                    Rectangle::new(Point::from((20, 10)), Size::from((60, 30))),
                ),
            ],
        };
        let rects = region_to_non_overlapping_rects(&region);
        assert_eq!(rects.len(), 4);
        assert_eq!(rects.iter().map(|r| r.size.w * r.size.h).sum::<i32>(), 3200);
        for (i, a) in rects.iter().enumerate() {
            for b in &rects[i + 1..] {
                assert!(!a.overlaps(*b));
            }
        }
    }
}
