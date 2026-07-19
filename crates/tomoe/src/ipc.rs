//! IPC socket server — the compositor side of doctrine 03 (state-owning
//! daemon, thin client).
//!
//! The wire contract lives in `tomoe-ipc`; this module hosts it on the
//! calloop event loop. Every connection is its own calloop source, reads are
//! non-blocking line-buffered, and writes go through a per-client outgoing
//! buffer flushed opportunistically — a client that stops reading is dropped
//! at [`MAX_OUTGOING`] instead of ever blocking the compositor.
//!
//! Method dispatch is two-layered (doctrine 01): a handful of builtins the
//! bare core answers itself (`version`, `windows`, `outputs`, `view`,
//! `subscribe`, `quit`), and everything else goes to Lua handlers registered
//! with `tomoe.ipc.serve` — user endpoints ride the exact same wire as the
//! builtins. Lua handlers run as normal Lua entries: snapshot refreshed
//! before, queued ops applied after (doctrine 02).

use std::collections::HashMap;
use std::io::{ErrorKind, Read, Write};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::PathBuf;

use anyhow::Context;
use serde_json::{json, Value};
use smithay::reexports::calloop::generic::Generic;
use smithay::reexports::calloop::{Interest, Mode, PostAction, RegistrationToken};
use tracing::{info, warn};

use crate::lua::{OutputProps, ScreencastHookOutcome, ScreencastReply, WinProps};
use crate::state::Tomoe;

/// Outgoing-buffer cap per client: a reader this far behind is dropped —
/// never buffer unboundedly for a stuck peer.
const MAX_OUTGOING: usize = 1024 * 1024;

/// One request line may not exceed this (a client can't balloon our memory
/// by never sending a newline).
const MAX_REQUEST: usize = 1024 * 1024;

#[derive(Default)]
pub struct IpcState {
    clients: HashMap<u64, Client>,
    next_client: u64,
    socket_path: Option<PathBuf>,
    /// Last focus broadcast, to emit `focus_change` only on real changes
    /// (focus_window runs on every click, mostly with the same window).
    last_focus: Option<u64>,
    /// Portal `screencast_select` requests awaiting a Lua answer:
    /// token → (client id, request id).
    pending_screencast: HashMap<u64, (u64, u64)>,
    next_screencast_token: u64,
}

struct Client {
    /// Write handle (dup of the fd owned by the calloop source).
    stream: UnixStream,
    token: RegistrationToken,
    /// Partial input line, accumulated across reads.
    inbuf: Vec<u8>,
    /// Pending output the socket wasn't ready to take yet.
    outbuf: Vec<u8>,
    /// Subscribed to the event stream; `filter` empty means all events.
    subscribed: bool,
    filter: Vec<String>,
    dead: bool,
}

impl Client {
    fn wants(&self, event: &str) -> bool {
        self.subscribed && (self.filter.is_empty() || self.filter.iter().any(|f| f == event))
    }

    /// Queue a frame and opportunistically flush. Marks the client dead on
    /// write errors or when it stops reading; the caller sweeps.
    fn send(&mut self, frame: &Value) {
        if self.dead {
            return;
        }
        self.outbuf.extend_from_slice(frame.to_string().as_bytes());
        self.outbuf.push(b'\n');
        self.flush();
        if self.outbuf.len() > MAX_OUTGOING {
            warn!("dropping IPC client: not reading its socket");
            self.dead = true;
        }
    }

    fn flush(&mut self) {
        while !self.outbuf.is_empty() {
            match (&self.stream).write(&self.outbuf) {
                Ok(0) => {
                    self.dead = true;
                    return;
                }
                Ok(n) => {
                    self.outbuf.drain(..n);
                }
                Err(err) if err.kind() == ErrorKind::WouldBlock => return,
                Err(err) if err.kind() == ErrorKind::Interrupted => continue,
                Err(_) => {
                    self.dead = true;
                    return;
                }
            }
        }
    }
}

/// Bind the socket and register the listener on the event loop. Returns the
/// socket path (exported as `$TOMOE_SOCKET`).
pub fn start(tomoe: &mut Tomoe, wayland_display: &str) -> anyhow::Result<PathBuf> {
    let path = tomoe_ipc::socket_path(wayland_display);
    // A previous instance that crashed leaves a stale file; the name is
    // namespaced by the (fresh) Wayland display, so unlinking is safe.
    let _ = std::fs::remove_file(&path);
    let listener = UnixListener::bind(&path)
        .with_context(|| format!("error binding IPC socket {}", path.display()))?;
    listener
        .set_nonblocking(true)
        .context("error setting IPC socket non-blocking")?;

    tomoe
        .loop_handle
        .insert_source(
            Generic::new(listener, Interest::READ, Mode::Level),
            |_, listener, tomoe| {
                loop {
                    match listener.accept() {
                        Ok((stream, _)) => accept_client(tomoe, stream),
                        Err(err) if err.kind() == ErrorKind::WouldBlock => break,
                        Err(err) => {
                            warn!("error accepting IPC client: {err}");
                            break;
                        }
                    }
                }
                Ok(PostAction::Continue)
            },
        )
        .map_err(|err| anyhow::anyhow!("error inserting IPC listener source: {err}"))?;

    tomoe.ipc.socket_path = Some(path.clone());
    info!("IPC socket at {}", path.display());
    Ok(path)
}

/// Unlink the socket on compositor exit.
pub fn shutdown(tomoe: &mut Tomoe) {
    if let Some(path) = tomoe.ipc.socket_path.take() {
        let _ = std::fs::remove_file(path);
    }
}

fn accept_client(tomoe: &mut Tomoe, stream: UnixStream) {
    if stream.set_nonblocking(true).is_err() {
        return;
    }
    // The calloop source owns one fd, the client entry a dup for writes; both
    // refer to the same socket.
    let Ok(write_half) = stream.try_clone() else {
        return;
    };
    let id = tomoe.ipc.next_client;
    tomoe.ipc.next_client += 1;

    let token = match tomoe.loop_handle.insert_source(
        Generic::new(stream, Interest::READ, Mode::Level),
        move |_, stream, tomoe| Ok(read_client(tomoe, id, stream)),
    ) {
        Ok(token) => token,
        Err(err) => {
            warn!("error inserting IPC client source: {err}");
            return;
        }
    };

    tomoe.ipc.clients.insert(
        id,
        Client {
            stream: write_half,
            token,
            inbuf: Vec::new(),
            outbuf: Vec::new(),
            subscribed: false,
            filter: Vec::new(),
            dead: false,
        },
    );
}

fn read_client(tomoe: &mut Tomoe, id: u64, stream: &UnixStream) -> PostAction {
    // Readable also means writable progress is likely; drain the backlog.
    if let Some(client) = tomoe.ipc.clients.get_mut(&id) {
        client.flush();
    }

    let mut lines = Vec::new();
    let mut disconnect = false;
    let mut buf = [0u8; 4096];
    // `impl Read for &UnixStream`: reads through a shared reference, which is
    // all the calloop source hands out.
    let mut reader = stream;
    loop {
        let Some(client) = tomoe.ipc.clients.get_mut(&id) else {
            return PostAction::Remove;
        };
        match reader.read(&mut buf) {
            Ok(0) => {
                disconnect = true;
                break;
            }
            Ok(n) => {
                client.inbuf.extend_from_slice(&buf[..n]);
                while let Some(pos) = client.inbuf.iter().position(|&b| b == b'\n') {
                    let line: Vec<u8> = client.inbuf.drain(..=pos).collect();
                    lines.push(line);
                }
                if client.inbuf.len() > MAX_REQUEST {
                    warn!("dropping IPC client: oversized request");
                    disconnect = true;
                    break;
                }
            }
            Err(err) if err.kind() == ErrorKind::WouldBlock => break,
            Err(err) if err.kind() == ErrorKind::Interrupted => continue,
            Err(_) => {
                disconnect = true;
                break;
            }
        }
    }

    for line in lines {
        if !line.iter().all(|b| b.is_ascii_whitespace()) {
            dispatch(tomoe, id, &line);
        }
    }

    let dead = disconnect || tomoe.ipc.clients.get(&id).is_none_or(|client| client.dead);
    if dead {
        tomoe.ipc.clients.remove(&id);
        tomoe
            .ipc
            .pending_screencast
            .retain(|_, (cid, _)| *cid != id);
        return PostAction::Remove;
    }
    PostAction::Continue
}

fn dispatch(tomoe: &mut Tomoe, client_id: u64, line: &[u8]) {
    let request: tomoe_ipc::Request = match serde_json::from_slice(line) {
        Ok(request) => request,
        Err(err) => {
            // No id to address a response to; log and drop.
            warn!("invalid IPC request: {err}");
            return;
        }
    };

    let result = match request.method.as_str() {
        "version" => Ok(json!({
            "wire": tomoe_ipc::WIRE_VERSION,
            "version": env!("CARGO_PKG_VERSION"),
        })),
        "windows" => {
            let mut windows: Vec<_> = tomoe.collect_win_props().into_iter().collect();
            windows.sort_unstable_by_key(|(id, _)| *id);
            Ok(json!(windows
                .into_iter()
                .map(|(id, props)| window_json(id, &props))
                .collect::<Vec<_>>()))
        }
        "outputs" => Ok(json!(tomoe
            .collect_output_props()
            .iter()
            .map(output_json)
            .collect::<Vec<_>>())),
        "view" => {
            let offset = tomoe.space.view_offset();
            Ok(json!({
                "x": offset.x,
                "y": offset.y,
                "zoom": tomoe.space.view_zoom(),
            }))
        }
        "subscribe" => {
            let filter: Vec<String> = request
                .params
                .as_ref()
                .and_then(|p| p.get("events"))
                .and_then(|e| serde_json::from_value(e.clone()).ok())
                .unwrap_or_default();
            match tomoe.ipc.clients.get_mut(&client_id) {
                Some(client) => {
                    client.subscribed = true;
                    client.filter = filter.clone();
                    Ok(json!({
                        "events": if filter.is_empty() { json!("all") } else { json!(filter) },
                    }))
                }
                None => return,
            }
        }
        "quit" => {
            // Scripted exit: immediate, no confirm dialog (that UI guards
            // interactive keypresses, not deliberate automation).
            tomoe.loop_signal.stop();
            Ok(json!(true))
        }
        "screencast_select" => {
            // May answer later (deferred to an interactive pick), so it
            // manages its own response instead of returning a result here.
            handle_screencast_select(tomoe, client_id, &request);
            return;
        }
        method if tomoe.lua.has_ipc_handler(method) => {
            let params = request.params.clone().unwrap_or(Value::Null);
            tomoe.sync_snapshot();
            let was_in_lua = tomoe.in_lua;
            tomoe.in_lua = true;
            let result = tomoe.lua.call_ipc_handler(method, params);
            tomoe.in_lua = was_in_lua;
            tomoe.after_lua();
            result
        }
        method => Err(format!("unknown method: {method}")),
    };

    let Some(request_id) = request.id else {
        // Fire-and-forget: surface handler failures in the log at least.
        if let Err(err) = result {
            warn!("IPC {}: {err}", request.method);
        }
        return;
    };
    let frame = match result {
        Ok(result) => json!({ "id": request_id, "result": result }),
        Err(error) => json!({ "id": request_id, "error": error }),
    };
    if let Some(client) = tomoe.ipc.clients.get_mut(&client_id) {
        client.send(&frame);
    }
    sweep_dead(tomoe);
}

/// True if any connected client subscribed to `event` — lets emitters skip
/// building payloads nobody wants.
pub fn has_subscribers(tomoe: &Tomoe, event: &str) -> bool {
    tomoe.ipc.clients.values().any(|c| c.wants(event))
}

/// Push an event frame to every subscribed client whose filter matches.
pub fn broadcast(tomoe: &mut Tomoe, event: &str, payload: Value) {
    let frame = json!({ "event": event, "payload": payload });
    for client in tomoe.ipc.clients.values_mut() {
        if client.wants(event) {
            client.send(&frame);
        }
    }
    sweep_dead(tomoe);
}

/// Remove clients marked dead by a failed/backed-up write, deregistering
/// their loop sources and abandoning their pending screencast requests.
fn sweep_dead(tomoe: &mut Tomoe) {
    let dead: Vec<u64> = tomoe
        .ipc
        .clients
        .iter()
        .filter(|(_, c)| c.dead)
        .map(|(id, _)| *id)
        .collect();
    for id in dead {
        if let Some(client) = tomoe.ipc.clients.remove(&id) {
            tomoe.loop_handle.remove(client.token);
        }
        tomoe
            .ipc
            .pending_screencast
            .retain(|_, (cid, _)| *cid != id);
    }
}

// ── Screencast source policy (M4 §7) ──

/// Send a `screencast_select` response frame to its waiting client.
fn send_screencast_response(tomoe: &mut Tomoe, client_id: u64, request_id: u64, result: Value) {
    let frame = json!({ "id": request_id, "result": result });
    if let Some(client) = tomoe.ipc.clients.get_mut(&client_id) {
        client.send(&frame);
    }
}

/// `screencast_select` — the portal backend asking which source to cast:
/// params `{app_id?, types?: ["monitor","window"]}`. Answered by the
/// config's `tomoe.on_screencast_request` hook — immediately, or later when
/// the hook deferred to an interactive pick (the portal waits with a
/// timeout; the compositor never does). `{"action":"fallback"}` tells the
/// backend to use its own heuristics (no hook registered / hook error).
fn handle_screencast_select(tomoe: &mut Tomoe, client_id: u64, request: &tomoe_ipc::Request) {
    let Some(request_id) = request.id else {
        warn!("screencast_select without an id cannot be answered; ignoring");
        return;
    };
    if !tomoe.lua.has_screencast_hook() {
        send_screencast_response(
            tomoe,
            client_id,
            request_id,
            json!({ "action": "fallback" }),
        );
        sweep_dead(tomoe);
        return;
    }
    let app_id = request
        .params
        .as_ref()
        .and_then(|p| p.get("app_id"))
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string();
    let types: Vec<String> = request
        .params
        .as_ref()
        .and_then(|p| p.get("types"))
        .and_then(|t| serde_json::from_value(t.clone()).ok())
        .unwrap_or_else(|| vec!["monitor".to_string()]);
    let monitor = types.iter().any(|t| t == "monitor");
    let window = types.iter().any(|t| t == "window");

    let token = tomoe.ipc.next_screencast_token;
    tomoe.ipc.next_screencast_token += 1;
    tomoe
        .ipc
        .pending_screencast
        .insert(token, (client_id, request_id));

    tomoe.sync_snapshot();
    let was_in_lua = tomoe.in_lua;
    tomoe.in_lua = true;
    let outcome = tomoe
        .lua
        .emit_screencast_request(token, app_id, monitor, window);
    tomoe.in_lua = was_in_lua;
    // Applies queued ops (the hook may open a tomoe.ui.menu) and flushes
    // any synchronous reply to the client.
    tomoe.after_lua();

    if outcome == ScreencastHookOutcome::Fallback
        && tomoe.ipc.pending_screencast.remove(&token).is_some()
    {
        send_screencast_response(
            tomoe,
            client_id,
            request_id,
            json!({ "action": "fallback" }),
        );
        sweep_dead(tomoe);
    }
}

/// Send queued `tomoe.on_screencast_request` answers to their waiting
/// portal clients. Called from `after_lua` (like the broadcast drain), so
/// deferred resolves from any later Lua entry reach the portal.
pub fn flush_screencast_replies(tomoe: &mut Tomoe) {
    let replies = tomoe.lua.take_screencast_replies();
    if replies.is_empty() {
        return;
    }
    for (token, reply) in replies {
        let Some((client_id, request_id)) = tomoe.ipc.pending_screencast.remove(&token) else {
            // Request abandoned: client gone or config reloaded.
            continue;
        };
        let result = match reply {
            ScreencastReply::Output(name) => {
                json!({ "action": "resolve", "type": "output", "output": name })
            }
            ScreencastReply::Window(id) => match tomoe.foreign_toplevels.get(&id) {
                Some(handle) => json!({
                    "action": "resolve",
                    "type": "window",
                    "identifier": handle.identifier(),
                }),
                None => {
                    warn!("screencast resolve: window {id} is gone; denying");
                    json!({ "action": "deny" })
                }
            },
            ScreencastReply::Deny => json!({ "action": "deny" }),
        };
        send_screencast_response(tomoe, client_id, request_id, result);
    }
    sweep_dead(tomoe);
}

/// A config reload swapped the VM: deferred screencast resolvers died with
/// it. Tell the waiting portals to use their fallback heuristics instead of
/// leaving them to time out.
pub fn abandon_pending_screencasts(tomoe: &mut Tomoe) {
    if tomoe.ipc.pending_screencast.is_empty() {
        return;
    }
    let pending: Vec<(u64, u64)> = tomoe
        .ipc
        .pending_screencast
        .drain()
        .map(|(_, v)| v)
        .collect();
    for (client_id, request_id) in pending {
        send_screencast_response(
            tomoe,
            client_id,
            request_id,
            json!({ "action": "fallback" }),
        );
    }
    sweep_dead(tomoe);
}

// ── Core event emitters (called from state.rs) ──

/// Broadcast coarse keyboard activity for visual clients; key values never
/// cross IPC.
pub fn notify_keyboard_activity(tomoe: &mut Tomoe, hand: &str) {
    if !has_subscribers(tomoe, "keyboard_activity") {
        return;
    }
    broadcast(tomoe, "keyboard_activity", json!({ "hand": hand }));
}

pub fn notify_window_open(tomoe: &mut Tomoe, id: u64) {
    if !has_subscribers(tomoe, "window_open") {
        return;
    }
    let props = tomoe.collect_win_props().remove(&id).unwrap_or_default();
    broadcast(tomoe, "window_open", json!(window_json(id, &props)));
}

pub fn notify_window_close(tomoe: &mut Tomoe, id: u64) {
    if !has_subscribers(tomoe, "window_close") {
        return;
    }
    broadcast(tomoe, "window_close", json!({ "id": id }));
}

pub fn notify_focus_change(tomoe: &mut Tomoe, id: Option<u64>) {
    if tomoe.ipc.last_focus == id {
        return;
    }
    tomoe.ipc.last_focus = id;
    if !has_subscribers(tomoe, "focus_change") {
        return;
    }
    broadcast(tomoe, "focus_change", json!({ "id": id }));
}

pub fn notify_outputs_changed(tomoe: &mut Tomoe) {
    if !has_subscribers(tomoe, "outputs_changed") {
        return;
    }
    let outputs: Vec<_> = tomoe
        .collect_output_props()
        .iter()
        .map(output_json)
        .collect();
    broadcast(tomoe, "outputs_changed", json!(outputs));
}

/// Drain `tomoe.ipc.broadcast(...)` calls queued during a Lua entry.
pub fn flush_lua_broadcasts(tomoe: &mut Tomoe) {
    for (event, payload) in tomoe.lua.take_ipc_broadcasts() {
        broadcast(tomoe, &event, payload);
    }
}

fn window_json(id: u64, props: &WinProps) -> tomoe_ipc::Window {
    tomoe_ipc::Window {
        id,
        app_id: props.app_id.clone(),
        title: props.title.clone(),
        geometry: props
            .geometry
            .map(|(x, y, w, h)| tomoe_ipc::Rect { x, y, w, h }),
        mapped: props.mapped,
        focused: props.focused,
        fullscreen: props.fullscreen,
        maximized: props.maximized,
    }
}

fn output_json(props: &OutputProps) -> tomoe_ipc::Output {
    let (x, y, w, h) = props.geometry;
    let (ux, uy, uw, uh) = props.usable;
    tomoe_ipc::Output {
        name: props.name.clone(),
        geometry: tomoe_ipc::Rect { x, y, w, h },
        usable: tomoe_ipc::Rect {
            x: ux,
            y: uy,
            w: uw,
            h: uh,
        },
    }
}
