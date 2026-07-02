//! org.freedesktop.impl.portal.ScreenCast backend implementation.
//!
//! Monitor sources only for now — tomoe has no foreign-toplevel protocol yet
//! (PLAN M5), so there is nothing to hang WINDOW capture off.
//!
//! See: https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.impl.portal.ScreenCast.html

use std::collections::HashMap;
use std::io::Write;
use std::process::{Command, Stdio};
use std::sync::Mutex;

use zbus::zvariant::{ObjectPath, OwnedValue, Value};

use crate::outputs::{self, OutputInfo};
use crate::pipewire_stream::{self, StreamHandle, StreamSpec};

/// SourceTypes bitmask values from the portal spec.
mod source_types {
    pub const MONITOR: u32 = 1 << 0;
}

/// CursorMode bitmask values from the portal spec.
mod cursor_modes {
    pub const HIDDEN: u32 = 1 << 0;
    pub const EMBEDDED: u32 = 1 << 1;
}

#[derive(Default)]
pub struct ScreenCast {
    /// What SelectSources picked, consumed by Start.
    sessions: Mutex<HashMap<String, OutputInfo>>,
    /// Live streaming pipelines; dropping a handle stops its thread.
    streams: Mutex<HashMap<String, StreamHandle>>,
    /// Per-session `cursor_mode & EMBEDDED != 0`. Filled by SelectSources,
    /// consumed by Start to configure the streaming pipeline.
    cursor_visibility: Mutex<HashMap<String, bool>>,
}

impl ScreenCast {
    pub fn new() -> Self {
        Self::default()
    }
}

/// Pick the output to cast without a GUI:
/// `TOMOE_SCREENCAST_OUTPUT` names one directly; a single connected output is
/// unambiguous; `TOMOE_PORTAL_CHOOSER` runs a dmenu-style command (output
/// names on stdin, chosen name on stdout, non-zero exit = cancel); otherwise
/// fall back to the first output.
fn choose_output(outputs: &[OutputInfo]) -> Option<OutputInfo> {
    if outputs.is_empty() {
        return None;
    }
    if let Ok(want) = std::env::var("TOMOE_SCREENCAST_OUTPUT") {
        match outputs.iter().find(|o| o.name == want) {
            Some(o) => return Some(o.clone()),
            None => {
                tracing::warn!(want, "TOMOE_SCREENCAST_OUTPUT names no connected output");
            }
        }
    }
    if outputs.len() == 1 {
        return Some(outputs[0].clone());
    }
    if let Ok(chooser) = std::env::var("TOMOE_PORTAL_CHOOSER") {
        match run_chooser(&chooser, outputs) {
            Ok(pick) => return pick,
            Err(e) => tracing::warn!("portal chooser {chooser:?} failed: {e}"),
        }
    }
    tracing::warn!(
        chosen = outputs[0].name,
        "multiple outputs and no chooser configured; casting the first \
         (set TOMOE_SCREENCAST_OUTPUT or TOMOE_PORTAL_CHOOSER)"
    );
    Some(outputs[0].clone())
}

fn run_chooser(
    chooser: &str,
    outputs: &[OutputInfo],
) -> Result<Option<OutputInfo>, Box<dyn std::error::Error + Send + Sync>> {
    let mut child = Command::new("/bin/sh")
        .args(["-c", chooser])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()?;
    {
        let stdin = child.stdin.as_mut().ok_or("chooser stdin unavailable")?;
        for o in outputs {
            writeln!(stdin, "{}", o.name)?;
        }
    }
    let out = child.wait_with_output()?;
    if !out.status.success() {
        // dmenu-style tools exit non-zero on escape — treat as user cancel.
        return Ok(None);
    }
    let choice = String::from_utf8_lossy(&out.stdout);
    let choice = choice.lines().next().unwrap_or("").trim();
    match outputs.iter().find(|o| o.name == choice) {
        Some(o) => Ok(Some(o.clone())),
        None => {
            tracing::warn!(choice, "chooser printed an unknown output; cancelling");
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
        source_types::MONITOR
    }

    /// Both HIDDEN and EMBEDDED — the OBS "Show cursor" checkbox toggles
    /// between these; wlr-screencopy's overlay_cursor honors it per session.
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
        tracing::info!(%handle, %session_handle, %app_id, cursor_mode, "SelectSources");

        let outputs = tokio::task::spawn_blocking(outputs::enumerate)
            .await
            .map_err(|e| zbus::fdo::Error::Failed(format!("output enumeration join: {e}")))?
            .map_err(|e| zbus::fdo::Error::Failed(format!("output enumeration: {e}")))?;
        tracing::info!(
            outputs = ?outputs.iter().map(|o| o.name.as_str()).collect::<Vec<_>>(),
            "enumerated outputs"
        );

        let pick = tokio::task::spawn_blocking(move || choose_output(&outputs))
            .await
            .map_err(|e| zbus::fdo::Error::Failed(format!("chooser join: {e}")))?;
        match pick {
            Some(out) => {
                tracing::info!(?out, %session_handle, "selected output");
                self.sessions
                    .lock()
                    .unwrap()
                    .insert(session_handle.to_string(), out);
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

        let Some(out) = selection else {
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
        self.streams
            .lock()
            .unwrap()
            .insert(session_key, stream_handle);

        let mut stream_props: HashMap<String, Value> = HashMap::new();
        stream_props.insert(
            "size".to_string(),
            Value::from((spec.width as i32, spec.height as i32)),
        );
        stream_props.insert(
            "source_type".to_string(),
            Value::from(source_types::MONITOR),
        );
        let streams: Vec<(u32, HashMap<String, Value>)> = vec![(node_id, stream_props)];
        let mut results = HashMap::new();
        results.insert(
            "streams".to_string(),
            OwnedValue::try_from(Value::from(streams)).unwrap(),
        );
        Ok((0, results))
    }
}
