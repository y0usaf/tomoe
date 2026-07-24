//! The org.freedesktop.Notifications daemon, hosted in-process
//! (FUSION F3): `notify-send` and friends talk to the compositor
//! directly, and the active set publishes as a snapshot so popups are
//! Lua policy on the `ui.*` surface (doctrine 01 — replaceable from
//! config), not native widgets.
//!
//! Same discipline as every service here: one session-bus connection,
//! blocking setup bounded by [`SETUP`], then the fd rides calloop as a
//! `Generic` source and drains nonblocking. Expiry rides calloop
//! timers. If another daemon owns the name (nested session, mako/dunst
//! configured), we degrade to nothing — the external daemon keeps
//! working; fusion removes no protocol.

use std::cell::RefCell;
use std::collections::HashMap;
use std::os::fd::{AsRawFd, BorrowedFd, OwnedFd};
use std::rc::Rc;
use std::time::Duration;

use calloop::generic::Generic;
use calloop::timer::{TimeoutAction, Timer};
use calloop::{Interest, LoopHandle, Mode, PostAction};
use rustbus::message_builder::MarshalledMessage;
use rustbus::wire::unmarshal::traits::Variant;
use rustbus::{MessageBuilder, RpcConn};

use crate::dbus::{self, DbusError, SETUP};

const NAME: &str = "org.freedesktop.Notifications";
const PATH: &str = "/org/freedesktop/Notifications";

/// Applied when a client sends `expire_timeout = -1` ("server default").
const DEFAULT_TIMEOUT: Duration = Duration::from_secs(5);

/// Close-reason codes from the spec.
const REASON_EXPIRED: u32 = 1;
const REASON_CLOSED_BY_CALL: u32 = 3;

/// One active notification, as Lua sees it.
#[derive(Debug, Clone, PartialEq)]
pub struct Notification {
    pub id: u32,
    pub app: String,
    pub summary: String,
    pub body: String,
    pub urgent: bool,
}

/// The active set, oldest first.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct NotificationsState {
    pub notifications: Vec<Notification>,
}

type Notify<D> = Rc<RefCell<Box<dyn FnMut(&mut D, &NotificationsState)>>>;

struct Daemon<D> {
    rpc: RpcConn,
    state: NotificationsState,
    next_id: u32,
    notify: Notify<D>,
}

impl<D> Daemon<D> {
    fn snapshot(&self) -> NotificationsState {
        self.state.clone()
    }

    fn signal_closed(&mut self, id: u32, reason: u32) {
        let mut sig = MessageBuilder::new()
            .signal(NAME, "NotificationClosed", PATH)
            .build();
        if sig.body.push_param2(id, reason).is_ok() {
            if let Err(e) = dbus::send(&mut self.rpc, &mut sig) {
                tracing::debug!("notifications: NotificationClosed signal: {e}");
            }
        }
    }

    /// Remove `id`; true if it existed (signal + republish then).
    fn close(&mut self, id: u32, reason: u32) -> bool {
        let before = self.state.notifications.len();
        self.state.notifications.retain(|n| n.id != id);
        if self.state.notifications.len() == before {
            return false;
        }
        self.signal_closed(id, reason);
        true
    }

    /// Handle one method call; replies are best-effort (a gone client
    /// must not wedge the daemon).
    fn handle_call(&mut self, call: &MarshalledMessage) -> Option<(u32, Duration)> {
        let member = call.dynheader.member.as_deref().unwrap_or("");
        let mut expiry = None;
        let mut reply = call.dynheader.make_response();
        match member {
            "GetServerInformation" => {
                let _ = reply.body.push_param4("tomoe", "tomoe", "0.1.0", "1.2");
            }
            "GetCapabilities" => {
                let _ = reply.body.push_param(vec!["body".to_string()]);
            }
            "Notify" => {
                let mut p = call.body.parser();
                let app: String = p.get().unwrap_or_default();
                let replaces: u32 = p.get().unwrap_or(0);
                let _icon: String = p.get::<String>().unwrap_or_default();
                let summary: String = p.get().unwrap_or_default();
                let body: String = p.get().unwrap_or_default();
                let _actions: Vec<String> = p.get().unwrap_or_default();
                let hints: HashMap<String, Variant> = p.get().unwrap_or_default();
                let timeout_ms: i32 = p.get().unwrap_or(-1);

                let urgent = hints
                    .get("urgency")
                    .and_then(|v| v.get::<u8>().ok())
                    .map(|u| u >= 2)
                    .unwrap_or(false);
                let id = if replaces != 0 {
                    self.state.notifications.retain(|n| n.id != replaces);
                    replaces
                } else {
                    self.next_id += 1;
                    self.next_id
                };
                self.state.notifications.push(Notification {
                    id,
                    app,
                    summary,
                    body,
                    urgent,
                });
                let _ = reply.body.push_param(id);
                // 0 = never expire; -1 = server default; >0 = that many ms.
                expiry = match timeout_ms {
                    0 => None,
                    t if t < 0 => Some((id, DEFAULT_TIMEOUT)),
                    t => Some((id, Duration::from_millis(t as u64))),
                };
            }
            "CloseNotification" => {
                let id: u32 = call.body.parser().get().unwrap_or(0);
                self.close(id, REASON_CLOSED_BY_CALL);
            }
            other => {
                tracing::debug!("notifications: unhandled method {other:?}");
                let _ = reply; // fall through to an empty reply
            }
        }
        if let Err(e) = dbus::send(&mut self.rpc, &mut reply) {
            tracing::debug!("notifications: reply failed: {e}");
        }
        expiry
    }
}

/// Start the daemon. Fails (gracefully, in the caller) when the bus is
/// unreachable or another daemon owns the name.
pub fn start<D: 'static>(
    handle: &LoopHandle<'static, D>,
    notify: impl FnMut(&mut D, &NotificationsState) + 'static,
) -> Result<(), DbusError> {
    let mut rpc = RpcConn::session_conn(SETUP)?;
    let mut req = rustbus::standard_messages::request_name(
        NAME,
        rustbus::standard_messages::DBUS_NAME_FLAG_DO_NOT_QUEUE,
    );
    let serial = dbus::send(&mut rpc, &mut req)?;
    let reply = rpc.wait_response(serial, SETUP)?;
    dbus::reply_ok(&reply)?;
    let code: u32 = reply.body.parser().get().unwrap_or(0);
    if code != rustbus::standard_messages::DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER {
        return Err(DbusError::Reply(format!(
            "{NAME} already owned (reply code {code}) — external daemon keeps it"
        )));
    }

    let raw = rpc.conn().as_raw_fd();
    let fd: OwnedFd = unsafe { BorrowedFd::borrow_raw(raw) }.try_clone_to_owned()?;

    let daemon = Rc::new(RefCell::new(Daemon {
        rpc,
        state: NotificationsState::default(),
        next_id: 0,
        notify: Rc::new(RefCell::new(Box::new(notify))),
    }));

    let handle_for_timers = handle.clone();
    let inserted = handle.insert_source(
        Generic::new(fd, Interest::READ, Mode::Level),
        move |_, _, data: &mut D| {
            let d = daemon.clone();
            let mut changed = false;
            let mut expirations = Vec::new();
            {
                let daemon = &mut *d.borrow_mut();
                match daemon.rpc.refill_all() {
                    Ok(_) => {
                        while let Some(call) = daemon.rpc.try_get_call() {
                            changed = true;
                            if let Some(exp) = daemon.handle_call(&call) {
                                expirations.push(exp);
                            }
                        }
                    }
                    Err(e) => {
                        tracing::warn!("notifications: bus read failed ({e}); daemon off");
                        return Ok(PostAction::Remove);
                    }
                }
            }
            for (id, after) in expirations {
                let d = d.clone();
                let timer = Timer::from_duration(after);
                if let Err(e) = handle_for_timers.insert_source(timer, move |_, _, data: &mut D| {
                    let daemon = &mut *d.borrow_mut();
                    if daemon.close(id, REASON_EXPIRED) {
                        let state = daemon.snapshot();
                        let notify = daemon.notify.clone();
                        (notify.borrow_mut())(data, &state);
                    }
                    TimeoutAction::Drop
                }) {
                    tracing::debug!("notifications: expiry timer: {e}");
                }
            }
            if changed {
                let (state, notify) = {
                    let daemon = d.borrow();
                    (daemon.snapshot(), daemon.notify.clone())
                };
                (notify.borrow_mut())(data, &state);
            }
            Ok(PostAction::Continue)
        },
    );
    inserted.map_err(|e| DbusError::Loop(e.to_string()))?;
    Ok(())
}
