//! Native screenshots: render the current scene offscreen, then encode and
//! save a PNG (plus best-effort `wl-copy` clipboard) off the main thread.

use std::fs::File;
use std::io::BufWriter;
use std::path::PathBuf;
use std::process::{Command, Stdio};

use anyhow::{Context, Result};
use smithay::output::Output;
use smithay::utils::{Physical, Rectangle, Size};
use tracing::{debug, info, warn};

use crate::state::Tomoe;

/// Capture `output` (cropped to `region` in output-local physical coordinates
/// when given, the whole output otherwise), then encode and write the PNG on
/// a detached thread so the compositor never blocks on disk I/O.
pub fn screenshot(
    tomoe: &mut Tomoe,
    output: &Output,
    region: Option<Rectangle<i32, Physical>>,
) -> Result<()> {
    let (size, pixels) = crate::capture::capture_rgba(tomoe, output, region)
        .context("error capturing screenshot pixels")?;

    std::thread::spawn(move || match save_png(size, &pixels) {
        Ok(path) => {
            info!("screenshot saved to {}", path.display());
            copy_to_clipboard(&path);
        }
        Err(err) => warn!("error saving screenshot: {err:#}"),
    });

    Ok(())
}

/// Encode tightly packed RGBA8 `pixels` as a PNG under the screenshots
/// directory; returns the written path.
fn save_png(size: Size<i32, Physical>, pixels: &[u8]) -> Result<PathBuf> {
    let dir = screenshots_dir();
    std::fs::create_dir_all(&dir).with_context(|| format!("error creating {}", dir.display()))?;

    let timestamp = jiff::Zoned::now().strftime("%Y-%m-%d-%H%M%S").to_string();
    let path = dir.join(format!("Screenshot-{timestamp}.png"));

    let file = File::create(&path).with_context(|| format!("error creating {}", path.display()))?;
    let mut encoder = png::Encoder::new(BufWriter::new(file), size.w as u32, size.h as u32);
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    let mut writer = encoder.write_header().context("error writing PNG header")?;
    writer
        .write_image_data(pixels)
        .context("error writing PNG data")?;
    writer.finish().context("error finishing PNG")?;

    Ok(path)
}

/// `$XDG_PICTURES_DIR/Screenshots`, falling back to `~/Pictures/Screenshots`.
fn screenshots_dir() -> PathBuf {
    let pictures = std::env::var_os("XDG_PICTURES_DIR")
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .unwrap_or_else(|| {
            let home = std::env::var_os("HOME").unwrap_or_default();
            PathBuf::from(home).join("Pictures")
        });
    pictures.join("Screenshots")
}

/// Best-effort clipboard copy: pipe the written file into `wl-copy`. Missing
/// wl-copy just logs a warning.
fn copy_to_clipboard(path: &std::path::Path) {
    let file = match File::open(path) {
        Ok(file) => file,
        Err(err) => {
            warn!("error reopening screenshot for clipboard: {err}");
            return;
        }
    };
    match Command::new("wl-copy")
        .args(["-t", "image/png"])
        .stdin(Stdio::from(file))
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
    {
        Ok(_child) => debug!("screenshot handed to wl-copy"),
        Err(err) => warn!("wl-copy unavailable, screenshot not copied to clipboard: {err}"),
    }
}
