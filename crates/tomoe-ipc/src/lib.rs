//! Wire protocol for tomoe's IPC socket (doctrine 03: state-owning daemon,
//! thin client).
//!
//! This crate is the *wire contract* — framing, socket discovery, message
//! shapes — and is kept deliberately small and stable: any change here bumps
//! [`WIRE_VERSION`]. The *event vocabulary* (which methods and events exist)
//! lives in the compositor and the user's config and grows freely without
//! touching this layer.
//!
//! The wire format is newline-delimited JSON over a Unix socket
//! (ShojiWM-shape):
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
pub const WIRE_VERSION: u32 = 1;

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

/// A server → client event frame (only sent after `subscribe`).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Event {
    pub event: String,
    #[serde(default)]
    pub payload: Value,
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
            let frame = self.read_frame()?;
            if frame.get("id").and_then(Value::as_u64) != Some(id) {
                continue;
            }
            if let Some(err) = frame.get("error").and_then(Value::as_str) {
                return Ok(Err(err.to_string()));
            }
            return Ok(Ok(frame.get("result").cloned().unwrap_or(Value::Null)));
        }
    }

    /// Block until the next event frame (call after a `subscribe` request).
    pub fn next_event(&mut self) -> io::Result<Event> {
        loop {
            let frame = self.read_frame()?;
            if frame.get("event").is_some() {
                return serde_json::from_value(frame).map_err(io::Error::other);
            }
        }
    }

    fn read_frame(&mut self) -> io::Result<Value> {
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
