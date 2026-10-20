//! Cursor telemetry for ScreenCast `METADATA` mode.
//!
//! Wayland has no global input protocol, so the portal asks the compositor for
//! the pointer over its own IPC socket. The compositor has the cursor image and
//! the pointer position in physical pixels; it answers with coordinates local
//! to the requested output or window and sends the bitmap only when its
//! generation changes.
//!
//! The frame path attaches that state as `SPA_META_Cursor` on each PipeWire
//! buffer. Consumers use it to draw an editable cursor or to place zooms on
//! clicks.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use serde::Deserialize;

const POLL_INTERVAL: Duration = Duration::from_millis(8);
const RETRY_INTERVAL: Duration = Duration::from_millis(250);
/// Cursors larger than this are passed over: the metadata slot is sized for
/// it, and anything bigger is not a pointer.
const MAX_CURSOR_EDGE: u32 = 256;

#[derive(Debug, Clone)]
pub enum Source {
    Output(String),
    Window(String),
}

impl Source {
    fn params(&self, generation: u64) -> serde_json::Value {
        match self {
            Source::Output(name) => serde_json::json!({ "output": name, "generation": generation }),
            Source::Window(identifier) => {
                serde_json::json!({ "window": identifier, "generation": generation })
            }
        }
    }
}

#[derive(Debug, Clone, Default)]
pub struct Sample {
    pub visible: bool,
    pub x: i32,
    pub y: i32,
    pub hotspot_x: i32,
    pub hotspot_y: i32,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub format: u32,
    pub generation: u64,
    pub bitmap: Option<Arc<Vec<u8>>>,
}

/// Whether the running compositor answers the `cursor` method. The portal is
/// often activated before a newly installed compositor is restarted; asking
/// this keeps `AvailableCursorModes` honest across that window.
pub fn available() -> bool {
    let Some(path) = tomoe_ipc::find_socket() else {
        return false;
    };
    let Ok(mut client) = tomoe_ipc::Client::connect(&path) else {
        return false;
    };
    let _ = client.set_timeout(Some(Duration::from_millis(300)));
    matches!(
        client.request("cursor", Some(serde_json::json!({ "generation": 0 }))),
        Ok(Ok(_))
    )
}

pub struct Poller {
    state: Arc<Mutex<Sample>>,
}

impl Poller {
    pub fn start(source: Source, stop: Arc<AtomicBool>) -> Option<Self> {
        let state = Arc::new(Mutex::new(Sample::default()));
        let shared = state.clone();
        thread::Builder::new()
            .name("portal-cursor".into())
            .spawn(move || poll(source, shared, stop))
            .ok()?;
        Some(Self { state })
    }

    pub fn sample(&self) -> Sample {
        self.state.lock().unwrap().clone()
    }
}

#[derive(Debug, Default, Deserialize)]
#[serde(default)]
struct Reply {
    visible: bool,
    hidden: bool,
    generation: u64,
    x: i32,
    y: i32,
    hotspot_x: i32,
    hotspot_y: i32,
    width: u32,
    height: u32,
    stride: u32,
    format: u32,
    image: Option<Image>,
}

#[derive(Debug, Deserialize)]
#[serde(untagged)]
enum Image {
    One(String),
    Chunks(Vec<String>),
}

impl Image {
    fn joined(&self) -> String {
        match self {
            Image::One(image) => image.clone(),
            Image::Chunks(chunks) => chunks.concat(),
        }
    }
}

fn poll(source: Source, state: Arc<Mutex<Sample>>, stop: Arc<AtomicBool>) {
    let mut client: Option<tomoe_ipc::Client> = None;
    let mut generation = 0u64;
    let mut warned = false;
    let mut logged = false;
    while !stop.load(Ordering::SeqCst) {
        if client.is_none() {
            let Some(path) = tomoe_ipc::find_socket() else {
                thread::sleep(RETRY_INTERVAL);
                continue;
            };
            match tomoe_ipc::Client::connect(&path) {
                Ok(connected) => {
                    let _ = connected.set_timeout(Some(Duration::from_millis(500)));
                    client = Some(connected);
                    generation = 0;
                    warned = false;
                }
                Err(error) => {
                    if !warned {
                        tracing::warn!(path = %path.display(), "cursor: {error}");
                        warned = true;
                    }
                    thread::sleep(RETRY_INTERVAL);
                    continue;
                }
            }
        }
        let Some(connected) = client.as_mut() else {
            continue;
        };
        match connected.request("cursor", Some(source.params(generation))) {
            Ok(Ok(value)) => {
                let reply: Reply = match serde_json::from_value(value) {
                    Ok(reply) => reply,
                    Err(error) => {
                        tracing::warn!("cursor: malformed reply: {error}");
                        thread::sleep(RETRY_INTERVAL);
                        continue;
                    }
                };
                let image = reply.image.as_ref().map(Image::joined);
                let bitmap = image
                    .as_deref()
                    .and_then(decode_base64)
                    .filter(|pixels| {
                        reply.width <= MAX_CURSOR_EDGE
                            && reply.height <= MAX_CURSOR_EDGE
                            && reply.stride >= reply.width.saturating_mul(4)
                            && pixels.len()
                                >= reply.stride as usize * reply.height as usize
                    })
                    .map(Arc::new);
                let mut sample = state.lock().unwrap();
                sample.visible = reply.visible && !reply.hidden;
                sample.x = reply.x;
                sample.y = reply.y;
                sample.hotspot_x = reply.hotspot_x;
                sample.hotspot_y = reply.hotspot_y;
                sample.width = reply.width;
                sample.height = reply.height;
                sample.stride = if reply.stride == 0 {
                    reply.width.saturating_mul(4)
                } else {
                    reply.stride
                };
                sample.generation = reply.generation;
                if let Some(bitmap) = bitmap {
                    sample.format = reply.format;
                    sample.bitmap = Some(bitmap);
                }
                generation = reply.generation;
                if !logged {
                    tracing::info!(
                        visible = sample.visible,
                        generation = sample.generation,
                        width = sample.width,
                        height = sample.height,
                        bitmap = sample.bitmap.as_ref().map(|pixels| pixels.len()).unwrap_or(0),
                        image = reply.image.as_ref().map(Image::joined).unwrap_or_default().len(),
                        "cursor: first sample"
                    );
                    logged = true;
                }
            }
            Ok(Err(error)) => {
                tracing::debug!("cursor: compositor error: {error}");
                thread::sleep(RETRY_INTERVAL);
            }
            Err(error) => {
                tracing::warn!("cursor: {error}");
                client = None;
                thread::sleep(RETRY_INTERVAL);
                continue;
            }
        }
        thread::sleep(POLL_INTERVAL);
    }
}

/// Metadata slot size to request from PipeWire, in bytes. The bitmap sits
/// directly after the cursor and its descriptor, and the pixel data after
/// that.
pub fn meta_size() -> usize {
    std::mem::size_of::<pipewire::spa::sys::spa_meta_cursor>()
        + std::mem::size_of::<pipewire::spa::sys::spa_meta_bitmap>()
        + (MAX_CURSOR_EDGE as usize) * (MAX_CURSOR_EDGE as usize) * 4
}

/// Write `sample` into the buffer's cursor metadata. `written` is the cursor
/// generation whose bitmap the consumer already has; it is updated when this
/// frame carries a new one.
pub fn write(pw_buffer: *mut pipewire::sys::pw_buffer, sample: &Sample, written: &mut u64) {
    use pipewire::spa::sys as spa;

    unsafe {
        if pw_buffer.is_null() || (*pw_buffer).buffer.is_null() {
            return;
        }
        let buffer = (*pw_buffer).buffer;
        if (*buffer).n_metas == 0 || (*buffer).metas.is_null() {
            return;
        }
        let metas = std::slice::from_raw_parts_mut((*buffer).metas, (*buffer).n_metas as usize);
        let Some(meta) = metas.iter_mut().find(|meta| meta.type_ == spa::SPA_META_Cursor) else {
            return;
        };
        let cursor_size = std::mem::size_of::<spa::spa_meta_cursor>();
        if meta.data.is_null() || meta.size < cursor_size as u32 {
            return;
        }
        let cursor = meta.data as *mut spa::spa_meta_cursor;
        std::ptr::write_bytes(cursor, 0, 1);
        (*cursor).id = if sample.visible {
            u32::try_from(sample.generation).unwrap_or(u32::MAX).max(1)
        } else {
            0
        };
        (*cursor).position = spa::spa_point {
            x: sample.x,
            y: sample.y,
        };
        (*cursor).hotspot = spa::spa_point {
            x: sample.hotspot_x,
            y: sample.hotspot_y,
        };

        if !sample.visible || sample.generation == *written {
            return;
        }
        let Some(format) = spa_format(sample.format) else {
            tracing::debug!(format = sample.format, "cursor: unsupported bitmap format");
            return;
        };
        let Some(bitmap) = sample.bitmap.as_deref() else {
            return;
        };
        let bitmap_size = std::mem::size_of::<spa::spa_meta_bitmap>();
        let offset = cursor_size + bitmap_size;
        let stride = sample.stride as usize;
        let Some(data_size) = stride.checked_mul(sample.height as usize) else {
            return;
        };
        let Some(needed) = offset.checked_add(data_size) else {
            return;
        };
        if sample.width > MAX_CURSOR_EDGE
            || sample.height > MAX_CURSOR_EDGE
            || needed > meta.size as usize
            || data_size > bitmap.len()
        {
            tracing::debug!(
                width = sample.width,
                height = sample.height,
                needed,
                meta = meta.size,
                bitmap = bitmap.len(),
                "cursor: bitmap does not fit the metadata slot"
            );
            return;
        }

        let descriptor = (meta.data as *mut u8).add(cursor_size) as *mut spa::spa_meta_bitmap;
        std::ptr::write_bytes(descriptor, 0, 1);
        (*descriptor).format = format;
        (*descriptor).size = spa::spa_rectangle {
            width: sample.width,
            height: sample.height,
        };
        (*descriptor).stride = sample.stride as i32;
        (*descriptor).offset = bitmap_size as u32;
        std::ptr::copy_nonoverlapping(bitmap.as_ptr(), (meta.data as *mut u8).add(offset), data_size);
        (*cursor).bitmap_offset = cursor_size as u32;
        *written = sample.generation;
        tracing::debug!(
            generation = sample.generation,
            format,
            width = sample.width,
            height = sample.height,
            stride = sample.stride,
            "cursor: bitmap attached"
        );
    }
}

fn spa_format(drm_format: u32) -> Option<u32> {
    use drm_fourcc::DrmFourcc;
    use pipewire::spa::sys as spa;
    match drm_format {
        x if x == DrmFourcc::Argb8888 as u32 => Some(spa::SPA_VIDEO_FORMAT_ARGB),
        x if x == DrmFourcc::Xrgb8888 as u32 => Some(spa::SPA_VIDEO_FORMAT_xRGB),
        _ => None,
    }
}

fn decode_base64(input: &str) -> Option<Vec<u8>> {
    fn value(byte: u8) -> Option<u8> {
        match byte {
            b'A'..=b'Z' => Some(byte - b'A'),
            b'a'..=b'z' => Some(byte - b'a' + 26),
            b'0'..=b'9' => Some(byte - b'0' + 52),
            b'+' => Some(62),
            b'/' => Some(63),
            _ => None,
        }
    }
    let mut output = Vec::with_capacity(input.len() / 4 * 3);
    let mut chunk = [0u8; 4];
    let mut count = 0;
    for byte in input.bytes() {
        if byte == b'=' {
            break;
        }
        chunk[count] = value(byte)?;
        count += 1;
        if count == 4 {
            output.push(chunk[0] << 2 | chunk[1] >> 4);
            output.push(chunk[1] << 4 | chunk[2] >> 2);
            output.push(chunk[2] << 6 | chunk[3]);
            count = 0;
        }
    }
    match count {
        0 => {}
        2 => output.push(chunk[0] << 2 | chunk[1] >> 4),
        3 => {
            output.push(chunk[0] << 2 | chunk[1] >> 4);
            output.push(chunk[1] << 4 | chunk[2] >> 2);
        }
        _ => return None,
    }
    Some(output)
}
