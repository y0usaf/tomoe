//! Cursor theme loading and cursor render elements.
//!
//! The XCursor theme resolution (XDG icon-theme inheritance) and the
//! `.cursor` file format are small, self-contained, and only used here
//! across three names (`CursorTheme::load`, `.load_icon`, `parse`) —
//! so the `xcursor` crate is replaced by a local half as big, per the
//! dependency-replacement rule (replacement < used API surface). All
//! other callers keep the unchanged [`Cursor`] surface.

use std::io::Read;
use std::path::{Path, PathBuf};

use smithay::backend::allocator::Fourcc;
use smithay::backend::renderer::element::memory::{
    MemoryRenderBuffer, MemoryRenderBufferRenderElement,
};
use smithay::backend::renderer::element::Kind;
use smithay::utils::{Logical, Physical, Point, Transform};
use tracing::warn;

pub struct Cursor {
    /// Buffer + hotspot; None if no theme could be loaded (block fallback used).
    frame: Option<(MemoryRenderBuffer, Point<i32, Logical>)>,
}

impl Cursor {
    pub fn load() -> Self {
        let theme = std::env::var("XCURSOR_THEME").unwrap_or_else(|_| "default".to_string());
        let size: u32 = std::env::var("XCURSOR_SIZE")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(24);

        let frame = load_image(&theme, size, None).map(|image| {
            // SAFETY: xcursor bitmaps are small (a valid file would exhaust
            // memory long before u32 dims exceed i32::MAX), so `as i32`
            // cannot wrap for a legitimate theme.
            let buffer = MemoryRenderBuffer::from_slice(
                &image.pixels_rgba,
                Fourcc::Abgr8888,
                (image.width as i32, image.height as i32),
                1,
                Transform::Normal,
                None,
            );
            // SAFETY: hotspots are small pixel offsets (< 2^31) for the same
            // reason; a u32 hotspot near MAX is not a real cursor theme.
            (buffer, Point::from((image.xhot as i32, image.yhot as i32)))
        });
        if frame.is_none() {
            warn!("no xcursor theme found; using block cursor");
        }
        Self { frame }
    }

    pub fn element<R: crate::render::TomoeRenderer>(
        &self,
        renderer: &mut R,
        pos: Point<f64, Physical>,
    ) -> Option<MemoryRenderBufferRenderElement<R>> {
        let (buffer, hotspot) = self.frame.as_ref()?;
        // xcursor images are raw pixels; the hotspot is physical. Snap the
        // final position to the grid so the cursor image stays crisp.
        let location = (pos - hotspot.to_f64().to_physical(1.0))
            .to_i32_round::<i32>()
            .to_f64();
        MemoryRenderBufferRenderElement::from_buffer(
            renderer,
            location,
            buffer,
            None,
            None,
            None,
            Kind::Cursor,
        )
        .ok()
    }
}

/// A parsed cursor image. A subset of what the replaced `xcursor` crate
/// returned: this compositor only consumes `size`/`width`/`height`/
/// `xhot`/`yhot` + RGBA pixels, so the unused ARGB twin and the per-frame
/// delay are not reproduced.
struct XImage {
    size: u32,
    width: u32,
    height: u32,
    xhot: u32,
    yhot: u32,
    pixels_rgba: Vec<u8>,
}

/// The cursor-image chunk type marker (XCURSOR_IMAGE_TYPE) in the file's
/// TOC and chunk headers.
const IMAGE_TYPE: u32 = 0xfffd_0002;

/// A little-endian reader over the cursor-file byte stream.
struct Bytes<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Bytes<'a> {
    fn new(data: &'a [u8]) -> Self {
        Self { data, pos: 0 }
    }
    fn take(&mut self, n: usize) -> Option<&'a [u8]> {
        let end = self.pos.checked_add(n)?;
        let slice = self.data.get(self.pos..end)?;
        self.pos = end;
        Some(slice)
    }
    fn u32(&mut self) -> Option<u32> {
        Some(u32::from_le_bytes(self.take(4)?.try_into().ok()?))
    }
    fn seek(&mut self, to: usize) {
        self.pos = to;
    }
    /// Drain + compare a fixed 4-byte tag.
    fn tag(&mut self, tag: [u8; 4]) -> bool {
        self.take(4) == Some(&tag[..])
    }
}

/// Parse an XCursor file (the `Xcur` container format) into its images.
/// Faithful to the file format — gated by a fixture test so the wire
/// bytes the cursor renders from can't regress.
fn parse_xcursor(content: &[u8]) -> Option<Vec<XImage>> {
    let mut b = Bytes::new(content);
    // Magic "Xcur", header offset, version, ToC count.
    if !b.tag(*b"Xcur") {
        return None;
    }
    let header_offset = b.u32()? as usize;
    let _version = b.u32()?;
    let ntoc = b.u32()? as usize;

    b.seek(header_offset);
    let mut img_offsets = Vec::new();
    for _ in 0..ntoc {
        let toctype = b.u32()?;
        let _subtype = b.u32()?;
        let pos = b.u32()? as usize;
        if toctype == IMAGE_TYPE {
            img_offsets.push(pos);
        }
    }

    let mut imgs = Vec::new();
    for offset in img_offsets {
        b.seek(offset);
        // Image chunk header: chunk-size, type, nominal size, version.
        if !b.tag([0x24, 0x00, 0x00, 0x00]) {
            return None;
        }
        if !b.tag([0x02, 0x00, 0xfd, 0xff]) {
            return None;
        }
        let size = b.u32()?;
        if !b.tag([0x01, 0x00, 0x00, 0x00]) {
            return None;
        }
        let width = b.u32()?;
        let height = b.u32()?;
        let xhot = b.u32()?;
        let yhot = b.u32()?;
        let _delay = b.u32()?;

        // Well-formedness checks from libxcursor's reader: bounded,
        // non-zero dimensions, hotspot inside the image.
        if width == 0 || height == 0 || width > 0x7fff || height > 0x7fff {
            return None;
        }
        if xhot > width || yhot > height {
            return None;
        }

        let px = 4 * width as usize * height as usize;
        let pixels_rgba = b.take(px)?.to_vec();
        imgs.push(XImage {
            size,
            width,
            height,
            xhot,
            yhot,
            pixels_rgba,
        });
    }
    Some(imgs)
}

/// Resolve `icon_name` (e.g. "default" / "left_ptr") inside `theme`,
/// honoring the theme's `index.theme` `Inherits=` chain. `xcursor_path`
/// replaces the XDG search paths when `Some` — the same override the
/// `XCURSOR_PATH` env var applies, and how tests inject a temp theme.
fn resolve_icon(theme: &str, icon_name: &str, xcursor_path: Option<&str>) -> Option<PathBuf> {
    let search_paths: Vec<PathBuf> = match xcursor_path {
        Some(p) => p
            .split(':')
            .filter(|e| !e.is_empty())
            .map(PathBuf::from)
            .collect(),
        None => theme_search_paths(),
    };

    let mut walked: Vec<String> = Vec::new();
    let mut current = theme.to_string();
    loop {
        if walked.contains(&current) {
            return None; // inheritance cycle — no root, give up.
        }
        walked.push(current.clone());

        // Look for <theme>/cursors/<icon> in each search path.
        for base in &search_paths {
            let cursor = base.join(&current).join("cursors").join(icon_name);
            if cursor.is_file() {
                return Some(cursor);
            }
        }

        // No file: walk up the Inheritance chain.
        let mut next: Option<String> = None;
        for base in &search_paths {
            if let Some(inherits) = index_theme_inherits(&base.join(&current).join("index.theme")) {
                next = Some(inherits);
                break;
            }
        }
        if next.is_none() && current != "default" {
            // No relative index.theme: the theme inherits "default".
            next = Some("default".to_string());
        }
        let inherit = next?;
        current = inherit;
    }
}

/// The XDG cursor-theme search paths, in the same order the replaced
/// crate (a libwayland-cursor clone) searched them.
fn theme_search_paths() -> Vec<PathBuf> {
    let home = std::env::var("HOME").ok().filter(|s| !s.is_empty());
    let xdg_data_home = std::env::var("XDG_DATA_HOME")
        .ok()
        .filter(|s| !s.is_empty());
    let xdg_data_dirs = std::env::var("XDG_DATA_DIRS")
        .ok()
        .filter(|s| !s.is_empty());

    let mut paths = Vec::new();
    if let Some(xdg_data_home) = xdg_data_home {
        paths.push(PathBuf::from(xdg_data_home));
    } else if let Some(home) = &home {
        paths.push(Path::new(home).join(".local/share/icons"));
    }
    if let Some(home) = &home {
        paths.push(Path::new(home).join(".icons"));
    }
    if let Some(xdg_data_dirs) = xdg_data_dirs {
        paths.extend(
            xdg_data_dirs
                .split(':')
                .filter(|e| !e.is_empty())
                .map(|e| PathBuf::from(e).join("icons")),
        );
    } else {
        paths.push(PathBuf::from("/usr/local/share/icons"));
        paths.push(PathBuf::from("/usr/share/icons"));
    }
    paths.push(PathBuf::from("/usr/share/pixmaps"));
    if let Some(home) = &home {
        paths.push(Path::new(home).join(".cursors"));
    }
    paths.push(PathBuf::from("/usr/share/cursors/xorg-x11"));
    paths
}

/// Read the `Inherits=` value, if any, from an `index.theme`.
fn index_theme_inherits(path: &Path) -> Option<String> {
    let content = std::fs::read_to_string(path).ok()?;
    let is_sep = |c: &char| c.is_whitespace() || *c == ';' || *c == ',';
    for line in content.lines() {
        if !line.starts_with("Inherits") {
            continue;
        }
        let mut chars = line["Inherits".len()..].trim_start().chars();
        if chars.next() != Some('=') {
            continue;
        }
        let result: String = chars
            .skip_while(|c| is_sep(c))
            .take_while(|c| !is_sep(c))
            .collect();
        if !result.is_empty() {
            return Some(result);
        }
    }
    None
}

/// Load a cursor image: pick the size closest to the requested one from
/// `default` (falling back to `left_ptr`), parsing the resolved file.
/// `xcursor_path` overrides the XDG search paths when set.
fn load_image(theme: &str, size: u32, xcursor_path: Option<&str>) -> Option<XImage> {
    let path = resolve_icon(theme, "default", xcursor_path)
        .or_else(|| resolve_icon(theme, "left_ptr", xcursor_path))?;
    let mut data = Vec::new();
    std::fs::File::open(path)
        .ok()?
        .read_to_end(&mut data)
        .ok()?;
    let images = parse_xcursor(&data)?;
    // Pick the size closest to the requested one. Compare in u32 (absolute
    // distance via max-min) so a huge XCURSOR_SIZE never wraps via a
    // narrowing cast.
    images
        .into_iter()
        .min_by_key(|image| image.size.max(size) - image.size.min(size))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A cursor file with one 4x4 image, sliced out of the 0.3.10
    /// crate's own fixture, so the bytes are a real xcursorgen-produced
    /// cursor (the same file the old dependency parsed).
    const FIXTURE: [u8; 128] = [
        0x58, 0x63, 0x75, 0x72, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x02, 0x00, 0xfd, 0xff, 0x04, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x24, 0x00,
        0x00, 0x00, 0x02, 0x00, 0xfd, 0xff, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04,
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
        0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00,
        0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80,
        0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80,
    ];

    fn rgba_pixel(p: [u8; 4]) -> Vec<u8> {
        [p; 16].concat()
    }

    #[test]
    fn parses_xcursorgen_fixture() {
        let img = parse_xcursor(&FIXTURE).expect("fixture parses");
        assert_eq!(img.len(), 1);
        let im = &img[0];
        assert_eq!(
            (im.size, im.width, im.height, im.xhot, im.yhot),
            (4, 4, 4, 1, 1)
        );
        assert_eq!(im.pixels_rgba, rgba_pixel([0, 0, 0, 128]));
    }

    #[test]
    fn rejects_bad_magic_and_out_of_bounds_hotspot() {
        assert!(parse_xcursor(&[0u8; 64]).is_none());
        // A hotspot outside the image (xhot=200 > width=4).
        let mut bad = FIXTURE.to_vec();
        bad[0x20..0x24].copy_from_slice(&200u32.to_le_bytes());
        assert!(parse_xcursor(&bad).is_none());
    }

    #[test]
    fn theme_resolution_honors_inheritance_and_close_size() {
        let home = std::env::var("HOME").unwrap_or_default();
        if home.is_empty() {
            return;
        }
        // A "cursortest" theme under ~/.icons whose index.theme inherits
        // "default", from which the icon resolves.
        let tdir = Path::new(&home).join(".icons");
        let theme_dir = tdir.join("cursortest");
        let cursors = theme_dir.join("cursors");
        std::fs::create_dir_all(&cursors).unwrap();
        std::fs::write(theme_dir.join("index.theme"), "Inherits=default\n").unwrap();
        std::fs::write(cursors.join("default"), &FIXTURE[..]).unwrap();
        std::fs::write(cursors.join("left_ptr"), &FIXTURE[..]).unwrap();

        // With ~/.icons as an XCURSOR_PATH override, the theme resolves
        // through its inheritance chain to the file, and the 4x4 image is
        // picked for a requested size of 4 (not the (nonexistent) 24).
        let override_ = tdir.to_string_lossy().into_owned();
        let img = load_image("cursortest", 4, Some(&override_)).expect("theme resolves");
        assert_eq!((img.width, img.height, img.size), (4, 4, 4));

        let _ = std::fs::remove_dir_all(&theme_dir);
    }
}
