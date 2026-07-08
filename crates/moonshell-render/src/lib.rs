//! CPU rendering into wl_shm ARGB8888 buffers: tiny-skia for geometry,
//! cosmic-text for shaping and glyph rasterization.
//!
//! Coordinate doctrine: everything here works in integer physical
//! (buffer) pixels. Logical→physical scaling happens in the caller;
//! fonts are sized in physical pixels directly so glyphs are never
//! resampled.
//!
//! Pixel format: wl_shm ARGB8888 is little-endian, i.e. bytes
//! [B, G, R, A], premultiplied alpha. tiny-skia's pixmaps are byte-order
//! [R, G, B, A] premultiplied, so colors are swizzled (R<->B) exactly
//! once, inside [`Rgba::to_skia`], and tiny-skia then writes correct
//! ARGB8888 memory without any post-pass.

use cosmic_text::{Attrs, Buffer, FontSystem, Metrics, Shaping, SwashCache};
use tiny_skia::{Paint, PixmapMut, Rect, Transform};

/// Straight-alpha color as the caller thinks of it (CSS-style RGBA).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rgba {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

impl Rgba {
    pub const fn new(r: u8, g: u8, b: u8, a: u8) -> Self {
        Self { r, g, b, a }
    }

    /// R and B swapped: tiny-skia writes RGBA byte order, the buffer is
    /// interpreted as ARGB8888 little-endian (BGRA bytes).
    fn to_skia(self) -> tiny_skia::Color {
        tiny_skia::Color::from_rgba8(self.b, self.g, self.r, self.a)
    }
}

/// Owns the font system and glyph caches — the dominant allocation in
/// the process (budgeted in PLAN.md). Create one and keep it alive.
pub struct Renderer {
    font_system: FontSystem,
    swash_cache: SwashCache,
}

impl Renderer {
    /// Discovers system fonts via fontconfig configuration (parsed in
    /// pure Rust by fontdb).
    pub fn new() -> Self {
        Self {
            font_system: FontSystem::new(),
            swash_cache: SwashCache::new(),
        }
    }

    /// Fill the whole canvas with `color`.
    pub fn clear(&mut self, canvas: &mut [u8], width: u32, height: u32, color: Rgba) {
        let Some(mut pixmap) = pixmap(canvas, width, height) else {
            return;
        };
        pixmap.fill(color.to_skia());
    }

    /// Fill an axis-aligned rect, clipped to the canvas.
    #[allow(clippy::too_many_arguments)]
    pub fn fill_rect(
        &mut self,
        canvas: &mut [u8],
        width: u32,
        height: u32,
        x: f32,
        y: f32,
        w: f32,
        h: f32,
        color: Rgba,
    ) {
        let Some(rect) = Rect::from_xywh(x, y, w, h) else {
            return;
        };
        let mut paint = Paint::default();
        paint.set_color(color.to_skia());
        let Some(mut pixmap) = pixmap(canvas, width, height) else {
            return;
        };
        pixmap.fill_rect(rect, &paint, Transform::identity(), None);
    }

    /// Shape and draw a single line of text with its top-left corner at
    /// `(x, y)` in buffer pixels. `font_px` is the physical font size.
    /// Returns the advance width of the drawn line in buffer pixels.
    #[allow(clippy::too_many_arguments)]
    pub fn text_line(
        &mut self,
        canvas: &mut [u8],
        width: u32,
        height: u32,
        x: i32,
        y: i32,
        text: &str,
        font_px: f32,
        line_px: f32,
        color: Rgba,
    ) -> f32 {
        // cosmic-text panics ("no default font found") when the font
        // database is empty; a fontless system gets a bar without text,
        // not a crash — library crates don't panic.
        if self.font_system.db().faces().next().is_none() {
            tracing::warn!("no fonts available; skipping text draw");
            return 0.0;
        }
        let mut buffer = Buffer::new(&mut self.font_system, Metrics::new(font_px, line_px));
        buffer.set_size(None, Some(line_px));
        buffer.set_text(text, &Attrs::new(), Shaping::Advanced, None);
        // set_text only marks dirty in cosmic-text 0.19; shaping is explicit.
        buffer.shape_until_scroll(&mut self.font_system, true);

        let advance = buffer
            .layout_runs()
            .map(|run| run.line_w)
            .fold(0.0_f32, f32::max);

        let src = cosmic_text::Color::rgba(color.r, color.g, color.b, color.a);
        buffer.draw(
            &mut self.font_system,
            &mut self.swash_cache,
            src,
            |gx, gy, gw, gh, c| {
                blend_rect(canvas, width, height, x + gx, y + gy, gw, gh, c);
            },
        );
        advance
    }
}

impl Default for Renderer {
    fn default() -> Self {
        Self::new()
    }
}

/// `None` (drawing silently skipped) if the byte length doesn't match
/// `width * height * 4` — a caller bug, but library crates don't panic.
fn pixmap<'a>(canvas: &'a mut [u8], width: u32, height: u32) -> Option<PixmapMut<'a>> {
    let pm = PixmapMut::from_bytes(canvas, width, height);
    debug_assert!(pm.is_some(), "canvas size mismatch");
    pm
}

/// Src-over blend of a solid straight-alpha color rect into the
/// ARGB8888 canvas. cosmic-text emits mostly 1x1 rects (per-pixel
/// coverage baked into the alpha channel).
#[allow(clippy::too_many_arguments)]
fn blend_rect(
    canvas: &mut [u8],
    width: u32,
    height: u32,
    x: i32,
    y: i32,
    w: u32,
    h: u32,
    c: cosmic_text::Color,
) {
    let a = c.a() as u32;
    if a == 0 {
        return;
    }
    // Premultiplied source, in buffer byte order [B, G, R, A].
    let src = [
        (c.b() as u32 * a + 127) / 255,
        (c.g() as u32 * a + 127) / 255,
        (c.r() as u32 * a + 127) / 255,
        a,
    ];
    let x0 = x.max(0) as u32;
    let y0 = y.max(0) as u32;
    let x1 = (x + w as i32).clamp(0, width as i32) as u32;
    let y1 = (y + h as i32).clamp(0, height as i32) as u32;
    let inv = 255 - a;
    for py in y0..y1 {
        let row = (py * width) as usize * 4;
        for px in x0..x1 {
            let i = row + px as usize * 4;
            let dst = &mut canvas[i..i + 4];
            for ch in 0..4 {
                dst[ch] = (src[ch] + (dst[ch] as u32 * inv + 127) / 255) as u8;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The R<->B swizzle: an opaque red clear must produce ARGB8888
    /// little-endian bytes [B, G, R, A] = [0, 0, 255, 255].
    #[test]
    fn clear_writes_argb8888_le() {
        let mut renderer = Renderer::new();
        let mut buf = vec![0u8; 2 * 2 * 4];
        renderer.clear(&mut buf, 2, 2, Rgba::new(255, 0, 0, 255));
        assert_eq!(&buf[0..4], &[0, 0, 255, 255]);
    }

    /// Text rasterization touches pixels — skipped when the environment
    /// has no fonts (e.g. the nix sandbox), which is a config problem,
    /// not a renderer bug.
    #[test]
    fn text_line_draws_glyphs() {
        let mut renderer = Renderer::new();
        if renderer.font_system.db().faces().next().is_none() {
            return;
        }
        let (w, h) = (128u32, 32u32);
        let mut buf = vec![0u8; (w * h * 4) as usize];
        let advance = renderer.text_line(
            &mut buf,
            w,
            h,
            0,
            0,
            "moonshell",
            16.0,
            20.0,
            Rgba::new(255, 255, 255, 255),
        );
        assert!(advance > 0.0, "no advance — shaping produced nothing");
        assert!(buf.iter().any(|&b| b != 0), "no pixels touched");
    }
}
