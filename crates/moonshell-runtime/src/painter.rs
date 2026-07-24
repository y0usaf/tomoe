//! [`LuaPainter`] — the `surface::Painter` whose content is a Lua
//! render callback.
//!
//! Paint pass: call the stored render function (registry key →
//! function), parse the returned table via [`crate::element::from_table`]
//! with the window's [`TextDefaults`], wrap it in a full-canvas stack
//! carrying the window background, and hand the tree to the shared
//! [`Scene`] for layout/damage/draw. Lua runs only to *describe* the
//! frame; drawing is pure Rust afterwards (nur's contract).
//!
//! The [`Renderer`] is shared (`Rc<RefCell>`) across every window of a
//! config: the font system and glyph caches are the dominant allocation
//! in the process — one copy, not one per window.
//!
//! A failing render callback is logged and the window paints its bare
//! background: a config error must stay visible (and keep the surface
//! mapped), not wedge the shell.

use std::cell::RefCell;
use std::rc::Rc;

use mlua::prelude::*;
use moonshell_render::element::{Stack, Style};
use moonshell_render::{Element, Renderer, Scene, SceneDamage};
use moonshell_surface::{Canvas, Damage, DamageRect, Painter};

use crate::element::from_table;
use crate::window::WindowShared;

pub struct LuaPainter {
    lua: Lua,
    shared: Rc<RefCell<WindowShared>>,
    renderer: Rc<RefCell<Renderer>>,
    scene: Scene,
}

impl LuaPainter {
    pub fn new(
        lua: Lua,
        shared: Rc<RefCell<WindowShared>>,
        renderer: Rc<RefCell<Renderer>>,
    ) -> Self {
        Self {
            lua,
            shared,
            renderer,
            scene: Scene::new(),
        }
    }

    /// Run the render callback and wrap its tree in the window shell
    /// (full-canvas stack painting the window bg; stack children fill
    /// the parent rect, so the Lua root spans the window).
    fn build_root(&self) -> LuaResult<Element> {
        // Fetch everything out of the RefCell before calling into Lua —
        // the callback may touch this very window's handle.
        let (render_fn, bg, text) = {
            let s = self.shared.borrow();
            let f = match &s.render_key {
                Some(key) => Some(self.lua.registry_value::<LuaFunction>(key)?),
                None => None,
            };
            (f, s.bg, s.text)
        };
        let mut children = Vec::new();
        if let Some(f) = render_fn {
            let table: LuaTable = f.call(())?;
            children.push(from_table(&table, text)?);
        }
        Ok(Element::Stack(Stack {
            style: Style {
                bg: Some(bg),
                ..Style::default()
            },
            children,
        }))
    }
}

impl Painter for LuaPainter {
    fn paint(&mut self, canvas: Canvas<'_>) -> Damage {
        let Canvas {
            buf,
            width,
            height,
            scale,
            fresh,
        } = canvas;
        if fresh {
            // No committed content at this size — the diff baseline is gone.
            self.scene.invalidate();
        }
        let root = self.build_root().unwrap_or_else(|e| {
            tracing::error!("render callback failed: {e}");
            let bg = self.shared.borrow().bg;
            Element::Stack(Stack {
                style: Style {
                    bg: Some(bg),
                    ..Style::default()
                },
                children: Vec::new(),
            })
        });
        let mut renderer = self.renderer.borrow_mut();
        match self
            .scene
            .render(&mut renderer, buf, width, height, scale as f32, &root)
        {
            SceneDamage::None => Damage::None,
            SceneDamage::Full => Damage::Full,
            SceneDamage::Rects(rects) => Damage::Rects(
                rects
                    .into_iter()
                    .map(|r| DamageRect {
                        x: r.x,
                        y: r.y,
                        width: r.w,
                        height: r.h,
                    })
                    .collect(),
            ),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::api::ShellCtx;
    use crate::Vm;

    /// Drive the full §3 path without a compositor: config exec →
    /// pending window → painter → pixels.
    fn painter_for(config: &str) -> (Vm, LuaPainter) {
        let vm = Vm::new().unwrap();
        let ctx = ShellCtx::new();
        vm.install_shell(&ctx).unwrap();
        vm.exec(config, "test.lua").unwrap();
        let mut pending = ctx.take_pending();
        assert_eq!(pending.len(), 1, "config must create exactly one window");
        let p = pending.remove(0);
        let painter = LuaPainter::new(
            vm.lua().clone(),
            p.shared,
            Rc::new(RefCell::new(Renderer::new())),
        );
        (vm, painter)
    }

    fn paint(painter: &mut LuaPainter, buf: &mut [u8], w: u32, h: u32, fresh: bool) -> Damage {
        painter.paint(Canvas {
            buf,
            width: w,
            height: h,
            scale: 1,
            fresh,
        })
    }

    #[test]
    fn window_bg_fills_the_canvas() {
        let (_vm, mut painter) = painter_for(
            r##"
            local w = shell.window({ height = 16, bg = "#ff0000" })
            w:render(function() return ui.hbox({}) end)
            "##,
        );
        let (w, h) = (8u32, 4u32);
        let mut buf = vec![0u8; (w * h * 4) as usize];
        let damage = paint(&mut painter, &mut buf, w, h, true);
        assert!(matches!(damage, Damage::Full), "{damage:?}");
        // ARGB8888 little-endian bytes: [B, G, R, A]; opaque red.
        for px in buf.chunks_exact(4) {
            assert_eq!(px, [0x00, 0x00, 0xff, 0xff]);
        }
    }

    #[test]
    fn unchanged_tree_is_zero_damage() {
        let (_vm, mut painter) = painter_for(
            r##"
            local w = shell.window({ bg = "#101010" })
            w:render(function() return ui.hbox({ children = { ui.text("hi") } }) end)
            "##,
        );
        let (w, h) = (32u32, 8u32);
        let mut buf = vec![0u8; (w * h * 4) as usize];
        assert!(matches!(
            paint(&mut painter, &mut buf, w, h, true),
            Damage::Full
        ));
        // Same tree next pass — the steady state does no work.
        assert!(matches!(
            paint(&mut painter, &mut buf, w, h, false),
            Damage::None
        ));
    }

    #[test]
    fn state_change_reaches_the_pixels() {
        let (vm, mut painter) = painter_for(
            r##"
            s = shell.state("#ff0000")
            local w = shell.window({ bg = "#000000" })
            w:render(function()
                return ui.hbox({ bg = s:get(), fill = true, height = 4 })
            end)
            "##,
        );
        let (w, h) = (4u32, 4u32);
        let mut buf = vec![0u8; (w * h * 4) as usize];
        paint(&mut painter, &mut buf, w, h, true);
        assert_eq!(&buf[0..4], [0x00, 0x00, 0xff, 0xff]); // red
        vm.exec(r##"s:set("#00ff00")"##, "test.lua").unwrap();
        let damage = paint(&mut painter, &mut buf, w, h, false);
        assert!(!matches!(damage, Damage::None), "{damage:?}");
        assert_eq!(&buf[0..4], [0x00, 0xff, 0x00, 0xff]); // green
    }

    #[test]
    fn no_render_fn_paints_bare_bg() {
        let (_vm, mut painter) = painter_for(r##"shell.window({ bg = "#0000ff" })"##);
        let (w, h) = (4u32, 2u32);
        let mut buf = vec![0u8; (w * h * 4) as usize];
        assert!(matches!(
            paint(&mut painter, &mut buf, w, h, true),
            Damage::Full
        ));
        assert_eq!(&buf[0..4], [0xff, 0x00, 0x00, 0xff]); // blue in BGRA
    }

    #[test]
    fn failing_render_fn_falls_back_to_bg() {
        let (_vm, mut painter) = painter_for(
            r##"
            local w = shell.window({ bg = "#00ff00" })
            w:render(function() error("boom") end)
            "##,
        );
        let (w, h) = (4u32, 2u32);
        let mut buf = vec![0u8; (w * h * 4) as usize];
        // Must still produce a frame (a remapped surface needs a buffer).
        assert!(matches!(
            paint(&mut painter, &mut buf, w, h, true),
            Damage::Full
        ));
        assert_eq!(&buf[0..4], [0x00, 0xff, 0x00, 0xff]); // green
    }
}
