//! Compositor-internal shell surfaces (FUSION.md F2): `shell.window{}`
//! declarations from the config become element-tree textures anchored
//! to output edges — no Wayland client, no IPC, one VM.
//!
//! Contract (snapshot in, actions out, render loop never waits on
//! config code): render callbacks run in the *dirty-drain* step
//! ([`ShellSurfaces::refresh`], called from `after_lua` and on output
//! changes), never during frame assembly. A frame only composites the
//! cached textures ([`ShellSurfaces::render_elements`]).
//!
//! Geometry follows the layer-shell vocabulary moonshell inherited:
//! anchors + logical margins/size, stretch on a fully-anchored axis,
//! exclusive zones that join the layer-shell usable-area computation
//! ([`ShellSurfaces::shrink_zone`]) so a native bar and an external
//! waybar reserve space through one mechanism. Surfaces are
//! per-output; output hotplug re-resolves rects on the next refresh
//! and drops textures of departed outputs.

use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

use moonshell_runtime::window::WindowShared;
use moonshell_runtime::PendingWindow;
use moonshell_surface::{Anchors, LayerOptions, Margins};
use smithay::backend::renderer::element::memory::MemoryRenderBufferRenderElement;
use smithay::backend::renderer::element::Kind;
use smithay::utils::{Logical, Physical, Rectangle, Size};

use crate::lua::LuaRuntime;
use crate::render::{OutputRenderElements, TomoeRenderer};
use crate::ui::element_tree::{Engine, TreeTexture};

/// One `shell.window{}` declaration, textured per output.
pub struct ShellSurface {
    pub options: LayerOptions,
    shared: Rc<RefCell<WindowShared>>,
    /// Keyed by output name; a bar exists once per output.
    per_output: HashMap<String, SurfaceTexture>,
}

struct SurfaceTexture {
    tex: TreeTexture,
    /// Output-local physical rect the texture composites at.
    rect: Rectangle<i32, Physical>,
    /// Cleared when the tree must re-render (state change, resize).
    fresh: bool,
}

/// All native shell surfaces, in declaration order.
#[derive(Default)]
pub struct ShellSurfaces {
    surfaces: Vec<ShellSurface>,
}

/// Logical→physical, rounding like the rest of the compositor.
fn px(v: f64, scale: f64) -> i32 {
    (v * scale).round() as i32
}

/// Resolve a surface's output-local physical rect from its layer-shell
/// vocabulary (anchors, logical size/margins) — integer physical pixels
/// before raster, per the coordinate doctrine.
fn resolve_rect(
    opts: &LayerOptions,
    output: Size<i32, Physical>,
    scale: f64,
) -> Rectangle<i32, Physical> {
    let Anchors {
        top,
        bottom,
        left,
        right,
    } = opts.anchors;
    let Margins {
        top: mt,
        right: mr,
        bottom: mb,
        left: ml,
    } = opts.margins;
    let (ml, mr, mt, mb) = (
        px(ml as f64, scale),
        px(mr as f64, scale),
        px(mt as f64, scale),
        px(mb as f64, scale),
    );
    let w = if left && right {
        (output.w - ml - mr).max(1)
    } else {
        px(opts.width as f64, scale).max(1)
    };
    let h = if top && bottom {
        (output.h - mt - mb).max(1)
    } else {
        px(opts.height as f64, scale).max(1)
    };
    let x = if left {
        ml
    } else if right {
        output.w - w - mr
    } else {
        (output.w - w) / 2
    };
    let y = if top {
        mt
    } else if bottom {
        output.h - h - mb
    } else {
        (output.h - h) / 2
    };
    Rectangle::new((x, y).into(), (w, h).into())
}

impl ShellSurfaces {
    /// Adopt freshly declared windows (`ShellCtx::take_pending`).
    /// Returns true when anything was adopted (zones may have changed).
    pub fn adopt(&mut self, pending: Vec<PendingWindow>) -> bool {
        let any = !pending.is_empty();
        for p in pending {
            self.surfaces.push(ShellSurface {
                options: p.options,
                shared: p.shared,
                per_output: HashMap::new(),
            });
        }
        any
    }

    /// Drop everything — config reload; the fresh VM re-declares.
    pub fn clear(&mut self) -> bool {
        let any = !self.surfaces.is_empty();
        self.surfaces.clear();
        any
    }

    pub fn is_empty(&self) -> bool {
        self.surfaces.is_empty()
    }

    /// Force re-render of every tree on the next [`refresh`]
    /// (`ShellCtx` dirty flag, output layout change).
    ///
    /// [`refresh`]: ShellSurfaces::refresh
    pub fn mark_dirty(&mut self) {
        for s in &mut self.surfaces {
            for t in s.per_output.values_mut() {
                t.fresh = false;
            }
        }
    }

    /// Shrink a layer-shell usable-area zone (output-local logical) by
    /// this shell's exclusive zones — the native side of the one
    /// usable-area computation.
    pub fn shrink_zone(&self, mut zone: Rectangle<i32, Logical>) -> Rectangle<i32, Logical> {
        for s in &self.surfaces {
            let ez = s.options.exclusive_zone;
            if ez <= 0 {
                continue;
            }
            let a = &s.options.anchors;
            // A zone reserves from the single anchored edge (a bar:
            // anchored across one edge, stretching along it).
            match (a.top, a.bottom, a.left, a.right) {
                (true, false, _, _) if a.left == a.right => {
                    let ez = ez.min(zone.size.h);
                    zone.loc.y += ez;
                    zone.size.h -= ez;
                }
                (false, true, _, _) if a.left == a.right => {
                    zone.size.h -= ez.min(zone.size.h);
                }
                (_, _, true, false) if a.top == a.bottom => {
                    let ez = ez.min(zone.size.w);
                    zone.loc.x += ez;
                    zone.size.w -= ez;
                }
                (_, _, false, true) if a.top == a.bottom => {
                    zone.size.w -= ez.min(zone.size.w);
                }
                _ => {}
            }
        }
        zone
    }

    /// Re-run render callbacks and re-raster where needed. Runs in the
    /// action-drain step (a Lua entry boundary), never during frame
    /// assembly. `outputs` is the live output list (name, physical
    /// size, fractional scale). Returns true when any texture changed
    /// (the caller queues redraws).
    pub fn refresh(
        &mut self,
        lua: &mut LuaRuntime,
        engine: &mut Engine,
        outputs: &[(String, Size<i32, Physical>, f64)],
    ) -> bool {
        let mut changed = false;
        for s in &mut self.surfaces {
            // Drop textures for outputs that left.
            s.per_output
                .retain(|name, _| outputs.iter().any(|(n, _, _)| n == name));
            for (name, size, scale) in outputs {
                let rect = resolve_rect(&s.options, *size, *scale);
                let entry = s
                    .per_output
                    .entry(name.clone())
                    .or_insert_with(|| SurfaceTexture {
                        tex: TreeTexture::new(),
                        rect,
                        fresh: false,
                    });
                let resized = entry.rect != rect;
                entry.rect = rect;
                if entry.fresh && !resized {
                    continue;
                }
                let Some(root) = lua.render_shell_root(&s.shared) else {
                    // Callback errored: keep the previous texture, stay
                    // un-fresh so a later state change retries.
                    continue;
                };
                // A `None` update was throttled (slow tree): stay
                // un-fresh and retry on the next refresh.
                if let Some(damage) = entry.tex.update(engine, &root, rect.size, *scale as f32) {
                    entry.fresh = true;
                    changed |= !damage.is_empty() || resized;
                }
            }
        }
        changed
    }

    /// Composite the cached textures for one output (topmost first, to
    /// match the callers' prepend order). Pure texture work — no Lua.
    pub fn render_elements<R: TomoeRenderer>(
        &self,
        renderer: &mut R,
        output_name: &str,
        elements: &mut Vec<OutputRenderElements<R>>,
    ) {
        for s in &self.surfaces {
            let Some(t) = s.per_output.get(output_name) else {
                continue;
            };
            if let Ok(element) = MemoryRenderBufferRenderElement::from_buffer(
                renderer,
                (t.rect.loc.x as f64, t.rect.loc.y as f64),
                t.tex.buffer(),
                None,
                None,
                None,
                Kind::Unspecified,
            ) {
                elements.push(OutputRenderElements::Memory(element));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use moonshell_surface::{Keyboard, Layer};

    fn opts(anchors: Anchors, w: u32, h: u32, ez: i32) -> LayerOptions {
        LayerOptions {
            namespace: "test".into(),
            layer: Layer::Top,
            anchors,
            width: w,
            height: h,
            exclusive_zone: ez,
            margins: Margins::default(),
            keyboard: Keyboard::None,
        }
    }

    #[test]
    fn top_bar_stretches_and_reserves() {
        let o = opts(
            Anchors {
                top: true,
                bottom: false,
                left: true,
                right: true,
            },
            0,
            32,
            32,
        );
        // 2x scale: the 32-logical bar is 64 physical tall, full width.
        let rect = resolve_rect(&o, Size::from((2560, 1440)), 2.0);
        assert_eq!(rect, Rectangle::new((0, 0).into(), (2560, 64).into()));

        let mut shell = ShellSurfaces::default();
        shell.adopt(vec![PendingWindow {
            options: o,
            shared: Rc::new(RefCell::new(WindowShared {
                render_key: None,
                bg: moonshell_render::Rgba::new(0, 0, 0, 255),
                text: Default::default(),
            })),
        }]);
        // Usable area loses the 32 logical px at the top.
        let zone = shell.shrink_zone(Rectangle::new((0, 0).into(), (1280, 720).into()));
        assert_eq!(zone, Rectangle::new((0, 32).into(), (1280, 688).into()));
    }

    #[test]
    fn bottom_right_popup_anchors() {
        let o = opts(
            Anchors {
                top: false,
                bottom: true,
                left: false,
                right: true,
            },
            300,
            100,
            0,
        );
        let rect = resolve_rect(&o, Size::from((1920, 1080)), 1.0);
        assert_eq!(rect, Rectangle::new((1620, 980).into(), (300, 100).into()));
    }
}
