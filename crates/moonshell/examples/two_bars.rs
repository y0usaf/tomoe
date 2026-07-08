//! M2 §2 acceptance fixture: two windows (top + bottom bars) on one
//! `Shell`, plus a calloop timer that destroys the bottom bar and then
//! quits — proving create/destroy work from inside a source callback,
//! the exact shape `shell.window`/timers (M2 §3–4) will use.
//!
//! Run under any compositor: `cargo run --example two_bars`. Exits 0
//! by itself after ~1.5 s; watch the bottom bar disappear at 1 s.

use std::time::Duration;

use moonshell_render::element::{Align, Edges, Flex, Spacer, Style, Text};
use moonshell_render::{Element, Renderer, Rgba, Scene, SceneDamage};
use moonshell_surface::{Canvas, Damage, DamageRect, Edge, LayerOptions, Painter, Shell, WindowId};

const BG: Rgba = Rgba::new(0x14, 0x14, 0x1e, 0xff);
const FG: Rgba = Rgba::new(0xc8, 0xc8, 0xd8, 0xff);

struct LabelBar {
    renderer: Renderer,
    scene: Scene,
    root: Element,
}

impl LabelBar {
    fn new(label: &str) -> Self {
        let root = Element::HBox(Flex {
            style: Style {
                bg: Some(BG),
                ..Style::default()
            },
            padding: Edges {
                left: 8.0,
                right: 8.0,
                ..Edges::default()
            },
            align: Align::Center,
            children: vec![
                Element::Text(Text {
                    content: label.into(),
                    size: 13.0,
                    color: FG,
                    ..Text::default()
                }),
                Element::Spacer(Spacer::default()),
            ],
            ..Flex::default()
        });
        Self {
            renderer: Renderer::new(),
            scene: Scene::new(),
            root,
        }
    }
}

impl Painter for LabelBar {
    fn paint(&mut self, canvas: Canvas<'_>) -> Damage {
        let Canvas {
            buf,
            width,
            height,
            scale,
            fresh,
        } = canvas;
        if fresh {
            self.scene.invalidate();
        }
        match self.scene.render(
            &mut self.renderer,
            buf,
            width,
            height,
            scale as f32,
            &self.root,
        ) {
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

fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("moonshell=info")),
        )
        .init();

    let (mut shell, event_loop) = Shell::connect()?;
    let _top = shell.create_window(
        LayerOptions::bar(Edge::Top, 28, true),
        Box::new(LabelBar::new("two_bars: top (stays until quit)")),
    );
    let bottom: WindowId = shell.create_window(
        LayerOptions {
            namespace: "moonshell-two-bars-bottom".into(),
            ..LayerOptions::bar(Edge::Bottom, 28, true)
        },
        Box::new(LabelBar::new("two_bars: bottom (dies at 1s)")),
    );

    // Destroy the bottom bar from inside a source callback, then quit
    // half a second later — the §3/§4 access pattern.
    event_loop
        .handle()
        .insert_source(
            calloop::timer::Timer::from_duration(Duration::from_secs(1)),
            move |_, _, shell: &mut Shell| {
                assert!(shell.destroy_window(bottom), "bottom bar already gone?");
                assert!(
                    !shell.destroy_window(bottom),
                    "double destroy must return false"
                );
                tracing::info!("bottom bar destroyed");
                calloop::timer::TimeoutAction::Drop
            },
        )
        .map_err(|e| anyhow::anyhow!("insert timer: {e}"))?;
    event_loop
        .handle()
        .insert_source(
            calloop::timer::Timer::from_duration(Duration::from_millis(1500)),
            |_, _, shell: &mut Shell| {
                tracing::info!("quitting");
                shell.quit();
                calloop::timer::TimeoutAction::Drop
            },
        )
        .map_err(|e| anyhow::anyhow!("insert timer: {e}"))?;

    shell.run(event_loop)?;
    Ok(())
}
