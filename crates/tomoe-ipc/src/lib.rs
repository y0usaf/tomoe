//! Wire protocol for tomoe's IPC socket (doctrine 03: state-owning daemon,
//! thin client).
//!
//! This crate is the *wire contract* — framing, socket discovery, message
//! shapes — and is kept deliberately small and stable: any change here bumps
//! [`WIRE_VERSION`]. The *event vocabulary* (which methods and events exist)
//! lives in the compositor and the user's config and grows freely without
//! touching this layer.
//!
//! The wire format is newline-delimited JSON over a Unix socket:
//!
//! ```text
//! client -> server   { "id"?: number, "method": string, "params"?: value }
//! server -> client   { "id": number, "result": value }        (response)
//!                    { "id": number, "error": string }        (error)
//!                    { "event": string, "payload": value }    (event stream)
//! ```
//!
//! Requests with an `id` receive exactly one matching response; requests
//! without one are fire-and-forget. Events only flow after a `subscribe`
//! request on the same connection.

use std::io::{self, BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;

use serde::{Deserialize, Serialize};
use serde_json::Value;

/// Bumped on any change to the framing or message shapes below.
pub const WIRE_VERSION: u32 = 2;

/// Environment variable pointing at the compositor's IPC socket; exported to
/// every child and pushed into the systemd/D-Bus activation environment.
pub const SOCKET_ENV: &str = "TOMOE_SOCKET";

/// A client → server message.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Request {
    /// Present: the server sends exactly one matching response.
    /// Absent: fire-and-forget.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub id: Option<u64>,
    /// Method to dispatch: a builtin (`version`, `windows`, `outputs`,
    /// `view`, `subscribe`, `quit`) or a user endpoint registered with
    /// `tomoe.ipc.serve`.
    pub method: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub params: Option<Value>,
}

/// Discrete params for a `subscribe` request: the event filter list.
/// Parsed once at the wire boundary — a present-but-malformed `events`
/// deserializes to an error instead of silently subscribing the client to
/// every event.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct SubscribeParams {
    /// Empty means "subscribe to every event" (the documented default).
    #[serde(default)]
    pub events: Vec<String>,
}

/// Discrete params for a `screencast_select` request: the portal source
/// pick. Parsed once at the wire boundary; a malformed `types` field errors
/// the whole request rather than silently granting full-monitor capture.
/// `types` empty/absent grants nothing — the portal falls back to its own
/// heuristics.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ScreencastParams {
    #[serde(default)]
    pub app_id: String,
    #[serde(default)]
    pub types: Vec<String>,
}

/// A server → client event frame (only sent after `subscribe`).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Event {
    pub event: String,
    #[serde(default)]
    pub payload: Value,
}

/// One server → client frame, discriminated by shape:
/// `{ "id", "result" }` for a response, `{ "id", "error" }` for an error,
/// `{ "event", "payload" }` for an event. This is the *wire boundary* type:
/// every frame is parsed once into a `Frame` here, so callers never re-probe
/// raw JSON (no more `frame.get("id").and_then(...)` at use sites).
///
/// `#[serde(untagged)]` keeps the on-wire JSON byte-identical to the shapes
/// documented in the module docs — it matches by field presence, it does not
/// add a discriminant key. Variant order mirrors the legacy read precedence:
/// `error`/response both carry `id`, and the original client checked `error`
/// before `result`, so `Error` is tried first.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(untagged)]
pub enum Frame {
    /// `{ "id": number, "error": string }` — an error reply.
    Error { id: u64, error: String },
    /// `{ "id": number, "result": value }` — a successful reply.
    Response { id: u64, result: Value },
    /// `{ "event": string, "payload": value }` — an event stream frame.
    Event {
        event: String,
        #[serde(default)]
        payload: Value,
    },
}

/// Geometry rectangle in global physical pixels (like all tomoe geometry).
#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}

/// A window as reported by the `windows` method and window events.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Window {
    pub id: u64,
    pub app_id: String,
    pub title: String,
    /// None while hidden/unmapped.
    pub geometry: Option<Rect>,
    pub mapped: bool,
    pub focused: bool,
    pub fullscreen: bool,
    pub maximized: bool,
}

/// An output as reported by the `outputs` method and `outputs_changed`.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Output {
    pub name: String,
    pub geometry: Rect,
    /// Geometry minus layer-shell exclusive zones.
    pub usable: Rect,
    /// Fractional client scale advertised on this output.
    pub scale: f64,
}

/// Socket path for a compositor whose Wayland socket is named
/// `wayland_display`: `$XDG_RUNTIME_DIR/tomoe.<display>.sock`. Predictable so
/// external clients need no side channel; namespaced by the display so
/// multiple instances don't collide.
pub fn socket_path(wayland_display: &str) -> PathBuf {
    let runtime_dir = std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(std::env::temp_dir);
    runtime_dir.join(format!("tomoe.{wayland_display}.sock"))
}

/// Resolve the socket of the running compositor: `$TOMOE_SOCKET` if set
/// (exported by the compositor to children and the activation environment),
/// else derived from `$WAYLAND_DISPLAY`.
pub fn find_socket() -> Option<PathBuf> {
    if let Some(path) = std::env::var_os(SOCKET_ENV) {
        return Some(PathBuf::from(path));
    }
    let display = std::env::var("WAYLAND_DISPLAY").ok()?;
    Some(socket_path(&display))
}

/// A blocking IPC client (the `tomoe msg` CLI; bars can use it too).
pub struct Client {
    reader: BufReader<UnixStream>,
    writer: UnixStream,
    next_id: u64,
}

impl Client {
    pub fn connect(path: &std::path::Path) -> io::Result<Self> {
        let stream = UnixStream::connect(path)?;
        let writer = stream.try_clone()?;
        Ok(Self {
            reader: BufReader::new(stream),
            writer,
            next_id: 1,
        })
    }

    /// Bound how long `request`/`next_event` block on the socket: after
    /// `timeout`, reads fail with a `WouldBlock`/`TimedOut` error instead
    /// of waiting forever. Wire-neutral — purely a client-side option.
    pub fn set_timeout(&self, timeout: Option<std::time::Duration>) -> io::Result<()> {
        self.reader.get_ref().set_read_timeout(timeout)
    }

    /// Send a request and block until its response arrives. Event frames
    /// received while waiting are dropped (subscribe last, then only read
    /// events). The outer error is transport failure; the inner is the
    /// server's error string.
    pub fn request(
        &mut self,
        method: &str,
        params: Option<Value>,
    ) -> io::Result<Result<Value, String>> {
        let id = self.next_id;
        self.next_id += 1;
        let mut line = serde_json::to_string(&Request {
            id: Some(id),
            method: method.to_string(),
            params,
        })
        .map_err(io::Error::other)?;
        line.push('\n');
        self.writer.write_all(line.as_bytes())?;

        loop {
            match self.read_frame()? {
                // Not our response (a different in-flight request, an event
                // from another subscriber slot, or a mismatched id): drop it.
                Frame::Error { id: fid, .. } if fid != id => continue,
                Frame::Response { id: fid, .. } if fid != id => continue,
                Frame::Error { id: fid, error } if fid == id => {
                    return Ok(Err(error));
                }
                Frame::Response { id: fid, result } if fid == id => {
                    return Ok(Ok(result));
                }
                // Anything else (an event frame, or a response/error whose id
                // never matches) is not ours; keep waiting.
                _ => continue,
            }
        }
    }

    /// Block until the next event frame (call after a `subscribe` request).
    pub fn next_event(&mut self) -> io::Result<Event> {
        loop {
            match self.read_frame()? {
                Frame::Event { event, payload } => return Ok(Event { event, payload }),
                // Not an event frame (response/error): not ours, keep waiting.
                _ => continue,
            }
        }
    }

    fn read_frame(&mut self) -> io::Result<Frame> {
        loop {
            let mut line = String::new();
            if self.reader.read_line(&mut line)? == 0 {
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "server closed the connection",
                ));
            }
            if line.trim().is_empty() {
                continue;
            }
            return serde_json::from_str(&line).map_err(io::Error::other);
        }
    }
}
