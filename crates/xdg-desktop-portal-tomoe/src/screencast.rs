//! org.freedesktop.impl.portal.ScreenCast backend implementation.
//!
//! Monitor sources stream via wlr-screencopy (`pipewire_stream`); window
//! sources ride ext-foreign-toplevel-list + ext-image-copy-capture
//! (`toplevel_stream`).
//!
//! See: https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.impl.portal.ScreenCast.html

use std::collections::HashMap;
use std::io::Write;
use std::process::{Command, Stdio};
use std::sync::Mutex;
use std::time::Duration;

use zbus::zvariant::{ObjectPath, OwnedValue, Value};

use crate::outputs::{self, OutputInfo};
use crate::pipewire_stream::{self, StreamSpec};
use crate::toplevel_stream;
use crate::toplevels::{self, ToplevelInfo};

/// SourceTypes bitmask values from the portal spec.
mod source_types {
    pub const MONITOR: u32 = 1 << 0;
    pub const WINDOW: u32 = 1 << 1;
}

/// CursorMode bitmask values from the portal spec.
mod cursor_modes {
    pub const HIDDEN: u32 = 1 << 0;
    pub const EMBEDDED: u32 = 1 << 1;
}

/// What SelectSources picked for a session.
#[derive(Debug, Clone)]
enum Selection {
    Monitor(OutputInfo),
    Window(ToplevelInfo),
}

/// One live streaming pipeline; dropping it stops its thread.
enum LiveStream {
    Monitor(#[allow(dead_code)] pipewire_stream::StreamHandle),
    Window(#[allow(dead_code)] toplevel_stream::StreamHandle),
}

#[derive(Default)]
pub struct ScreenCast {
    /// What SelectSources picked, consumed by Start.
    sessions: Mutex<HashMap<String, Selection>>,
    /// Live streaming pipelines; dropping a handle stops its thread.
    streams: Mutex<HashMap<String, LiveStream>>,
    /// Per-session `cursor_mode & EMBEDDED != 0`. Filled by SelectSources,
    /// consumed by Start to configure the streaming pipeline.
    cursor_visibility: Mutex<HashMap<String, bool>>,
}

impl ScreenCast {
    pub fn new() -> Self {
        Self::default()
    }
}

/// Chooser line for a window: title, app id, and the identifier that makes
/// the line unique when two windows share both.
fn window_line(t: &ToplevelInfo) -> String {
    format!("[window] {} ({}) {}", t.title, t.app_id, t.identifier)
}

/// What the compositor answered to a `screencast_select` IPC request.
enum IpcPick {
    Output(String),
    /// Foreign-toplevel identifier.
    Window(String),
    /// The config denied (or the user cancelled) the request.
    Deny,
    /// No hook registered / error / timeout: use the env-var heuristics.
    Fallback,
}

/// How long to wait for the compositor's answer. Generous: the config's
/// hook may defer to an interactive compositor-drawn picker — but a hook
/// that never answers must not wedge the portal forever.
const IPC_SELECT_TIMEOUT: Duration = Duration::from_secs(120);

/// Ask the compositor which source to cast (`tomoe.on_screencast_request`):
/// source policy is config policy, and the portal is a thin IPC client of
/// it. Every non-answer degrades to [`IpcPick::Fallback`] so screencasting
/// keeps working on a bare core (no config hook, no compositor socket).
fn ipc_select(app_id: &str, monitor: bool, window: bool) -> IpcPick {
    let Some(path) = tomoe_ipc::find_socket() else {
        return IpcPick::Fallback;
    };
    let mut client = match tomoe_ipc::Client::connect(&path) {
        Ok(client) => client,
        Err(e) => {
            tracing::warn!("screencast_select: connecting {}: {e}", path.display());
            return IpcPick::Fallback;
        }
    };
    if let Err(e) = client.set_timeout(Some(IPC_SELECT_TIMEOUT)) {
        tracing::warn!("screencast_select: set_timeout: {e}");
    }
    let mut types = Vec::new();
    if monitor {
        types.push("monitor");
    }
    if window {
        types.push("window");
    }
    let params = serde_json::json!({ "app_id": app_id, "types": types });
    let reply = match client.request("screencast_select", Some(params)) {
        Ok(Ok(reply)) => reply,
        Ok(Err(e)) => {
            tracing::warn!("screencast_select: compositor error: {e}");
            return IpcPick::Fallback;
        }
        Err(e) => {
            tracing::warn!("screencast_select: {e}");
            return IpcPick::Fallback;
        }
    };
    match reply.get("action").and_then(|v| v.as_str()) {
        Some("resolve") => {
            if let Some(name) = reply.get("output").and_then(|v| v.as_str()) {
                return IpcPick::Output(name.to_string());
            }
            if let Some(ident) = reply.get("identifier").and_then(|v| v.as_str()) {
                return IpcPick::Window(ident.to_string());
            }
            tracing::warn!("screencast_select: resolve without output/identifier");
            IpcPick::Fallback
        }
        Some("deny") => IpcPick::Deny,
        Some("fallback") => IpcPick::Fallback,
        other => {
            tracing::warn!("screencast_select: unknown action {other:?}");
            IpcPick::Fallback
        }
    }
}

/// Pick the source to cast without a GUI:
/// `TOMOE_SCREENCAST_WINDOW` names a window (identifier, exact app id, or
/// title substring); `TOMOE_SCREENCAST_OUTPUT` names an output; a single
/// candidate is unambiguous; `TOMOE_PORTAL_CHOOSER` runs a dmenu-style
/// command (candidate lines on stdin, chosen line on stdout, non-zero exit =
/// cancel); otherwise fall back to the first output when monitors are
/// allowed.
fn choose_source(outputs: &[OutputInfo], windows: &[ToplevelInfo]) -> Option<Selection> {
    if let Ok(want) = std::env::var("TOMOE_SCREENCAST_WINDOW") {
        if !windows.is_empty() {
            let want_lower = want.to_lowercase();
            let pick = windows
                .iter()
                .find(|t| t.identifier == want)
                .or_else(|| windows.iter().find(|t| t.app_id == want))
                .or_else(|| {
                    windows
                        .iter()
                        .find(|t| t.title.to_lowercase().contains(&want_lower))
                });
            match pick {
                Some(t) => return Some(Selection::Window(t.clone())),
                None => {
                    tracing::warn!(want, "TOMOE_SCREENCAST_WINDOW matches no toplevel");
                }
            }
        }
    }
    if let Ok(want) = std::env::var("TOMOE_SCREENCAST_OUTPUT") {
        match outputs.iter().find(|o| o.name == want) {
            Some(o) => return Some(Selection::Monitor(o.clone())),
            None => {
                tracing::warn!(want, "TOMOE_SCREENCAST_OUTPUT names no connected output");
            }
        }
    }
    if outputs.len() == 1 && windows.is_empty() {
        return Some(Selection::Monitor(outputs[0].clone()));
    }
    if outputs.is_empty() && windows.len() == 1 {
        return Some(Selection::Window(windows[0].clone()));
    }
    if let Ok(chooser) = std::env::var("TOMOE_PORTAL_CHOOSER") {
        match run_chooser(&chooser, outputs, windows) {
            Ok(pick) => return pick,
            Err(e) => tracing::warn!("portal chooser {chooser:?} failed: {e}"),
        }
    }
    if let Some(out) = outputs.first() {
        tracing::warn!(
            chosen = out.name,
            "multiple sources and no chooser configured; casting the first output \
             (set TOMOE_SCREENCAST_OUTPUT / TOMOE_SCREENCAST_WINDOW or TOMOE_PORTAL_CHOOSER)"
        );
        return Some(Selection::Monitor(out.clone()));
    }
    tracing::warn!("no source candidates and no chooser pick; cancelling");
    None
}

fn run_chooser(
    chooser: &str,
    outputs: &[OutputInfo],
    windows: &[ToplevelInfo],
) -> Result<Option<Selection>, Box<dyn std::error::Error + Send + Sync>> {
    // Output lines stay bare names (as before window support); window lines
    // are prefixed. The chooser echoes one line back, matched exactly.
    let mut candidates: Vec<(String, Selection)> = Vec::new();
    for o in outputs {
        candidates.push((o.name.clone(), Selection::Monitor(o.clone())));
    }
    for t in windows {
        candidates.push((window_line(t), Selection::Window(t.clone())));
    }

    let mut child = Command::new("/bin/sh")
        .args(["-c", chooser])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()?;
    {
        let stdin = child.stdin.as_mut().ok_or("chooser stdin unavailable")?;
        for (line, _) in &candidates {
            writeln!(stdin, "{line}")?;
        }
    }
    let out = child.wait_with_output()?;
    if !out.status.success() {
        // dmenu-style tools exit non-zero on escape — treat as user cancel.
        return Ok(None);
    }
    let choice = String::from_utf8_lossy(&out.stdout);
    let choice = choice.lines().next().unwrap_or("").trim();
    match candidates.iter().find(|(line, _)| line == choice) {
        Some((_, selection)) => Ok(Some(selection.clone())),
        None => {
            tracing::warn!(choice, "chooser printed an unknown source; cancelling");
            Ok(None)
        }
    }
}

#[zbus::interface(name = "org.freedesktop.impl.portal.ScreenCast")]
impl ScreenCast {
    #[zbus(property, name = "version")]
    fn version(&self) -> u32 {
        4
    }

    #[zbus(property, name = "AvailableSourceTypes")]
    fn available_source_types(&self) -> u32 {
        source_types::MONITOR | source_types::WINDOW
    }

    /// Both HIDDEN and EMBEDDED — the OBS "Show cursor" checkbox toggles
    /// between these; wlr-screencopy's overlay_cursor (monitors) and
    /// ext-image-copy-capture's paint_cursors (windows) honor it per session.
    #[zbus(property, name = "AvailableCursorModes")]
    fn available_cursor_modes(&self) -> u32 {
        cursor_modes::HIDDEN | cursor_modes::EMBEDDED
    }

    async fn create_session(
        &self,
        handle: ObjectPath<'_>,
        session_handle: ObjectPath<'_>,
        app_id: String,
        options: HashMap<String, OwnedValue>,
    ) -> zbus::fdo::Result<(u32, HashMap<String, OwnedValue>)> {
        tracing::info!(
            %handle, %session_handle, %app_id, option_keys = ?options.keys().collect::<Vec<_>>(),
            "CreateSession"
        );
        Ok((0, HashMap::new()))
    }

    async fn select_sources(
        &self,
        handle: ObjectPath<'_>,
        session_handle: ObjectPath<'_>,
        app_id: String,
        options: HashMap<String, OwnedValue>,
    ) -> zbus::fdo::Result<(u32, HashMap<String, OwnedValue>)> {
        // OBS sends `cursor_mode` per its "Show cursor" checkbox. EMBEDDED =
        // cursor in the stream, HIDDEN = no cursor. We default to EMBEDDED.
        let cursor_mode = options
            .get("cursor_mode")
            .and_then(|v| u32::try_from(v).ok())
            .unwrap_or(cursor_modes::EMBEDDED);
        let cursor_visible = cursor_mode & cursor_modes::EMBEDDED != 0;
        self.cursor_visibility
            .lock()
            .unwrap()
            .insert(session_handle.to_string(), cursor_visible);
        let types = options
            .get("types")
            .and_then(|v| u32::try_from(v).ok())
            .unwrap_or(source_types::MONITOR);
        tracing::info!(%handle, %session_handle, %app_id, cursor_mode, types, "SelectSources");

        let outputs = if types & source_types::MONITOR != 0 {
            tokio::task::spawn_blocking(outputs::enumerate)
                .await
                .map_err(|e| zbus::fdo::Error::Failed(format!("output enumeration join: {e}")))?
                .map_err(|e| zbus::fdo::Error::Failed(format!("output enumeration: {e}")))?
        } else {
            Vec::new()
        };
        let windows = if types & source_types::WINDOW != 0 {
            tokio::task::spawn_blocking(toplevels::enumerate)
                .await
                .map_err(|e| zbus::fdo::Error::Failed(format!("toplevel enumeration join: {e}")))?
                .unwrap_or_else(|e| {
                    tracing::warn!("toplevel enumeration failed: {e}");
                    Vec::new()
                })
        } else {
            Vec::new()
        };
        tracing::info!(
            outputs = ?outputs.iter().map(|o| o.name.as_str()).collect::<Vec<_>>(),
            windows = ?windows.iter().map(|t| t.title.as_str()).collect::<Vec<_>>(),
            "enumerated sources"
        );

        let requester = app_id.clone();
        let want_monitor = types & source_types::MONITOR != 0;
        let want_window = types & source_types::WINDOW != 0;
        let pick = tokio::task::spawn_blocking(move || {
            // Source policy lives in the config (tomoe.on_screencast_request,
            // asked over IPC); the env-var heuristics are the fallback for a
            // bare core or an unanswered request.
            match ipc_select(&requester, want_monitor, want_window) {
                IpcPick::Output(name) => match outputs.iter().find(|o| o.name == name) {
                    Some(o) => Some(Selection::Monitor(o.clone())),
                    None => {
                        tracing::warn!(name, "compositor picked an unknown output; falling back");
                        choose_source(&outputs, &windows)
                    }
                },
                IpcPick::Window(ident) => match windows.iter().find(|t| t.identifier == ident) {
                    Some(t) => Some(Selection::Window(t.clone())),
                    None => {
                        tracing::warn!(ident, "compositor picked an unknown window; falling back");
                        choose_source(&outputs, &windows)
                    }
                },
                IpcPick::Deny => None,
                IpcPick::Fallback => choose_source(&outputs, &windows),
            }
        })
        .await
        .map_err(|e| zbus::fdo::Error::Failed(format!("chooser join: {e}")))?;
        match pick {
            Some(selection) => {
                tracing::info!(?selection, %session_handle, "selected source");
                self.sessions
                    .lock()
                    .unwrap()
                    .insert(session_handle.to_string(), selection);
                Ok((0, HashMap::new()))
            }
            None => {
                tracing::info!(%session_handle, "source selection cancelled");
                Ok((1, HashMap::new()))
            }
        }
    }

    async fn start(
        &self,
        handle: ObjectPath<'_>,
        session_handle: ObjectPath<'_>,
        app_id: String,
        parent_window: String,
        _options: HashMap<String, OwnedValue>,
    ) -> zbus::fdo::Result<(u32, HashMap<String, OwnedValue>)> {
        let session_key = session_handle.to_string();
        let selection = self.sessions.lock().unwrap().get(&session_key).cloned();
        tracing::info!(%handle, %session_handle, %app_id, %parent_window, ?selection, "Start");

        let Some(selection) = selection else {
            tracing::warn!(%session_handle, "Start with no selection — cancelling");
            return Ok((1, HashMap::new()));
        };
        let cursor_visible = self
            .cursor_visibility
            .lock()
            .unwrap()
            .get(&session_key)
            .copied()
            .unwrap_or(true);

        let (node_id, width, height, source_type, stream) = match selection {
            Selection::Monitor(out) => {
                let framerate = {
                    let hz = (out.refresh_mhz as f32 / 1000.0).round() as u32;
                    hz.clamp(30, 240)
                };
                let spec = StreamSpec {
                    output_name: out.name.clone(),
                    width: out.width.max(1) as u32,
                    height: out.height.max(1) as u32,
                    framerate,
                    cursor_visible,
                };
                let spec_for_task = spec.clone();
                let stream_result =
                    tokio::task::spawn_blocking(move || pipewire_stream::start(spec_for_task))
                        .await
                        .map_err(|e| zbus::fdo::Error::Failed(format!("stream task panic: {e}")))?;
                let (node_id, stream_handle) = match stream_result {
                    Ok(v) => v,
                    Err(e) => {
                        tracing::error!("pipewire stream failed: {e}");
                        return Ok((2, HashMap::new()));
                    }
                };
                (
                    node_id,
                    spec.width,
                    spec.height,
                    source_types::MONITOR,
                    LiveStream::Monitor(stream_handle),
                )
            }
            Selection::Window(win) => {
                let spec = toplevel_stream::StreamSpec {
                    toplevel_identifier: win.identifier.clone(),
                    // No refresh source of truth per window; cap at 60.
                    // Content-paced anyway (frames follow commits).
                    framerate: 60,
                    cursor_visible,
                };
                let stream_result =
                    tokio::task::spawn_blocking(move || toplevel_stream::start(spec))
                        .await
                        .map_err(|e| zbus::fdo::Error::Failed(format!("stream task panic: {e}")))?;
                let (info, stream_handle) = match stream_result {
                    Ok(v) => v,
                    Err(e) => {
                        tracing::error!("toplevel pipewire stream failed: {e}");
                        return Ok((2, HashMap::new()));
                    }
                };
                (
                    info.node_id,
                    info.width,
                    info.height,
                    source_types::WINDOW,
                    LiveStream::Window(stream_handle),
                )
            }
        };
        self.streams.lock().unwrap().insert(session_key, stream);

        let mut stream_props: HashMap<String, Value> = HashMap::new();
        stream_props.insert(
            "size".to_string(),
            Value::from((width as i32, height as i32)),
        );
        stream_props.insert("source_type".to_string(), Value::from(source_type));
        let streams: Vec<(u32, HashMap<String, Value>)> = vec![(node_id, stream_props)];
        let mut results = HashMap::new();
        results.insert(
            "streams".to_string(),
            OwnedValue::try_from(Value::from(streams)).unwrap(),
        );
        Ok((0, results))
    }
}
