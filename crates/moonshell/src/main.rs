//! The moonshell binary. With no config (there is no config yet — the
//! runtime is M2), this maps one layer surface and draws a version
//! string: the doctrine-06 bare-core artifact. `--boot-check` exits 0
//! right after the first frame is committed, which is what
//! `nix flake check` runs under a headless compositor.

use moonshell_render::{Renderer, Rgba};
use moonshell_surface::{Canvas, Damage, LayerOptions, Painter};

const BG: Rgba = Rgba::new(0x14, 0x14, 0x1e, 0xff);
const FG: Rgba = Rgba::new(0xc8, 0xc8, 0xd8, 0xff);
/// Logical font size; scaled to physical inside paint.
const FONT_LOGICAL: f32 = 13.0;
const PAD_LOGICAL: i32 = 8;

struct VersionBar {
    renderer: Renderer,
    label: String,
}

impl Painter for VersionBar {
    fn paint(&mut self, canvas: Canvas<'_>) -> Damage {
        let Canvas {
            buf,
            width,
            height,
            scale,
        } = canvas;
        self.renderer.clear(buf, width, height, BG);
        let font_px = FONT_LOGICAL * scale as f32;
        let line_px = (font_px * 1.3).ceil();
        let x = PAD_LOGICAL * scale;
        let y = ((height as f32 - line_px) / 2.0).round() as i32;
        self.renderer
            .text_line(buf, width, height, x, y, &self.label, font_px, line_px, FG);
        Damage::Full
    }
}

fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("moonshell=info")),
        )
        .init();

    let mut boot_check = false;
    for arg in std::env::args().skip(1) {
        match arg.as_str() {
            "--boot-check" => boot_check = true,
            "--version" | "-V" => {
                println!("moonshell {}", env!("CARGO_PKG_VERSION"));
                return Ok(());
            }
            other => anyhow::bail!("unknown argument: {other} (try --version, --boot-check)"),
        }
    }

    let options = LayerOptions {
        exit_after_first_draw: boot_check,
        ..LayerOptions::default()
    };
    let painter = VersionBar {
        renderer: Renderer::new(),
        label: format!("moonshell {}", env!("CARGO_PKG_VERSION")),
    };
    moonshell_surface::run(options, Box::new(painter))?;
    Ok(())
}
