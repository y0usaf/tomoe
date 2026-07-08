//! Network state — NetworkManager over the system D-Bus, sysfs
//! operstate polling as the fallback.
//!
//! nur polled `nmcli` every 5 s from a thread; this rides
//! NetworkManager's own signals instead (the same rustbus-as-calloop
//! shape as battery). The object chain is NM root → primary active
//! connection → access point:
//!
//! - root (`/org/freedesktop/NetworkManager`): `State` (connected?)
//!   and `PrimaryConnection` (object path);
//! - the active connection: `Type` (`802-11-wireless` ⇒ WiFi) and
//!   `SpecificObject` (the AP path);
//! - the AP: `Ssid` (bytes) and `Strength` (0–100).
//!
//! One `path_namespace` match rule covers all three. When the chain
//! re-roots (connection switch, AP roam), the model reports what to
//! re-query and the wiring sends a nonblocking `GetAll`, resolved via
//! `try_get_response` on the next fd wakeup — replies are guarded
//! against the chain having moved again in the meantime. An NM
//! restart is caught by `NameOwnerChanged` and re-seeds the root.
//!
//! Fallback (no system bus / no NM): `/sys/class/net/*/operstate` on
//! a 30 s calloop timer — link-up/down only, no SSID/strength (nur's
//! sysfs semantics; WiFi details needed nmcli there too). Same
//! recorded divergence from zero-idle-wakeups as battery's sysfs
//! path.

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
use rustbus::wire::ObjectPath;
use rustbus::{MessageType, RpcConn};

use crate::dbus::{self, DbusError, PROPS_IFACE, SETUP};

const NM: &str = "org.freedesktop.NetworkManager";
const NM_PATH: &str = "/org/freedesktop/NetworkManager";
const NM_IFACE: &str = "org.freedesktop.NetworkManager";
const CONN_IFACE: &str = "org.freedesktop.NetworkManager.Connection.Active";
const AP_IFACE: &str = "org.freedesktop.NetworkManager.AccessPoint";
const DBUS: &str = "org.freedesktop.DBus";
/// D-Bus's "no object" convention.
const NO_PATH: &str = "/";
/// `NM_STATE_CONNECTED_LOCAL` — anything at or above has a live
/// carrier, matching nur's `operstate == "up"` semantics.
const STATE_CONNECTED: u32 = 50;
const WIFI_TYPE: &str = "802-11-wireless";
/// sysfs fallback poll cadence.
const SYSFS_POLL: Duration = Duration::from_secs(30);

/// The snapshot pushed to `notify` on every change — nur's shape
/// exactly (`connected`, `ssid`, `strength`); `ssid` is `None` on
/// ethernet and when disconnected.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct NetworkState {
    pub connected: bool,
    pub ssid: Option<String>,
    pub strength: u8,
}

/// Which backend ended up feeding the service (for the boot log).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Source {
    NetworkManager,
    Sysfs,
    None,
}

impl std::fmt::Display for Source {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Source::NetworkManager => "NetworkManager",
            Source::Sysfs => "sysfs operstate (30 s poll)",
            Source::None => "none (/sys/class/net unreadable)",
        })
    }
}

type Notify<D> = Rc<RefCell<Box<dyn FnMut(&mut D, &NetworkState)>>>;

/// Start the network service. Never fails: NetworkManager trouble
/// degrades to sysfs; an unreadable `/sys/class/net` degrades to
/// nothing.
pub fn start<D: 'static>(
    handle: &LoopHandle<'static, D>,
    notify: impl FnMut(&mut D, &NetworkState) + 'static,
) -> Source {
    let notify: Notify<D> = Rc::new(RefCell::new(Box::new(notify)));
    match nm_start(handle, notify.clone()) {
        Ok(()) => Source::NetworkManager,
        Err(e) => {
            tracing::info!("NetworkManager unavailable ({e}); trying sysfs");
            if sysfs_start(handle, notify) {
                Source::Sysfs
            } else {
                Source::None
            }
        }
    }
}

/// One typed value pulled out of a D-Bus variant.
#[derive(Debug, Clone, PartialEq)]
enum Val {
    U32(u32),
    U8(u8),
    Str(String),
    Path(String),
    Bytes(Vec<u8>),
}

fn val(v: &Variant) -> Option<Val> {
    v.get::<u32>()
        .ok()
        .map(Val::U32)
        .or_else(|| v.get::<u8>().ok().map(Val::U8))
        .or_else(|| v.get::<String>().ok().map(Val::Str))
        .or_else(|| {
            v.get::<ObjectPath<String>>()
                .ok()
                .map(|p| Val::Path(p.as_ref().to_string()))
        })
        .or_else(|| v.get::<Vec<u8>>().ok().map(Val::Bytes))
}

/// Which object in the NM chain a property batch belongs to.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Scope {
    Root,
    Conn,
    Ap,
}

/// A follow-up `GetAll` the wiring must send after a property landed.
#[derive(Debug, Clone, PartialEq, Eq)]
enum Query {
    Root,
    Conn(String),
    Ap(String),
}

/// The pure property model: NM's object chain in, snapshot out.
/// Chain re-roots clear everything downstream immediately, so a slow
/// or failed follow-up query degrades to "connected, details unknown"
/// instead of showing the previous network's SSID.
#[derive(Default)]
struct Model {
    state: u32,
    /// Primary active-connection path; empty or "/" = none.
    primary: String,
    conn_type: String,
    /// AP path (the connection's `SpecificObject`); empty or "/" = none.
    specific: String,
    ssid: Option<String>,
    strength: u8,
}

impl Model {
    fn snapshot(&self) -> NetworkState {
        let connected = self.state >= STATE_CONNECTED;
        let wifi = connected && self.conn_type.starts_with(WIFI_TYPE);
        NetworkState {
            connected,
            ssid: if wifi { self.ssid.clone() } else { None },
            strength: if wifi { self.strength } else { 0 },
        }
    }

    fn apply_root(&mut self, key: &str, v: Val) -> Option<Query> {
        match (key, v) {
            ("State", Val::U32(s)) => {
                self.state = s;
                None
            }
            ("PrimaryConnection", Val::Path(p)) => {
                if p == self.primary {
                    return None;
                }
                self.primary = p.clone();
                self.conn_type.clear();
                self.specific.clear();
                self.ssid = None;
                self.strength = 0;
                (p != NO_PATH).then_some(Query::Conn(p))
            }
            _ => None,
        }
    }

    fn apply_conn(&mut self, key: &str, v: Val) -> Option<Query> {
        match (key, v) {
            ("Type", Val::Str(t)) => {
                self.conn_type = t;
                None
            }
            ("SpecificObject", Val::Path(p)) => {
                if p == self.specific {
                    return None;
                }
                self.specific = p.clone();
                self.ssid = None;
                self.strength = 0;
                (p != NO_PATH).then_some(Query::Ap(p))
            }
            _ => None,
        }
    }

    fn apply_ap(&mut self, key: &str, v: Val) {
        match (key, v) {
            ("Ssid", Val::Bytes(b)) => {
                self.ssid = if b.is_empty() {
                    None // hidden SSID — nur's contract
                } else {
                    Some(String::from_utf8_lossy(&b).into_owned())
                };
            }
            ("Strength", Val::U8(s)) => self.strength = s,
            _ => {}
        }
    }

    /// Fold a property map into one scope of the chain, collecting
    /// follow-up queries.
    fn apply_map(&mut self, scope: Scope, props: &HashMap<String, Variant>) -> Vec<Query> {
        let mut queries = Vec::new();
        for (key, variant) in props {
            let Some(v) = val(variant) else { continue };
            match scope {
                Scope::Root => queries.extend(self.apply_root(key, v)),
                Scope::Conn => queries.extend(self.apply_conn(key, v)),
                Scope::Ap => self.apply_ap(key, v),
            }
        }
        queries
    }

    /// Route a `PropertiesChanged` signal: the interface argument
    /// picks the vocabulary, the object path guards against signals
    /// from objects the chain no longer points at.
    fn scope_for(&self, iface: &str, path: &str) -> Option<Scope> {
        match iface {
            NM_IFACE if path == NM_PATH => Some(Scope::Root),
            CONN_IFACE if path == self.primary => Some(Scope::Conn),
            AP_IFACE if path == self.specific => Some(Scope::Ap),
            _ => None,
        }
    }

    /// Is a `GetAll` reply for `query` still about the current chain?
    fn query_current(&self, query: &Query) -> Option<Scope> {
        match query {
            Query::Root => Some(Scope::Root),
            Query::Conn(p) if *p == self.primary => Some(Scope::Conn),
            Query::Ap(p) if *p == self.specific => Some(Scope::Ap),
            _ => None,
        }
    }
}

struct Nm<D> {
    rpc: RpcConn,
    model: Model,
    /// In-flight `GetAll`s: serial → what was asked.
    pending: Vec<(u32, Query)>,
    last: NetworkState,
    notify: Notify<D>,
    handle: LoopHandle<'static, D>,
}

impl<D> Nm<D> {
    /// Send one follow-up `GetAll` for a chain link.
    fn send_query(&mut self, query: Query) {
        let (path, iface) = match &query {
            Query::Root => (NM_PATH, NM_IFACE),
            Query::Conn(p) => (p.as_str(), CONN_IFACE),
            Query::Ap(p) => (p.as_str(), AP_IFACE),
        };
        let sent = dbus::get_all(NM, path, iface)
            .and_then(|mut call| dbus::send(&mut self.rpc, &mut call));
        match sent {
            Ok(serial) => self.pending.push((serial, query)),
            Err(e) => tracing::debug!("network: querying {query:?}: {e}"),
        }
    }

    /// Handle one incoming signal, collecting follow-up queries.
    fn on_signal(&mut self, sig: &MarshalledMessage) -> Vec<Query> {
        let path = sig.dynheader.object.as_deref().unwrap_or_default();
        match (
            sig.dynheader.interface.as_deref(),
            sig.dynheader.member.as_deref(),
        ) {
            (Some(PROPS_IFACE), Some("PropertiesChanged")) => {
                let mut parser = sig.body.parser();
                let Ok(iface) = parser.get::<&str>() else {
                    return Vec::new();
                };
                let Some(scope) = self.model.scope_for(iface, path) else {
                    return Vec::new();
                };
                let Ok(props) = parser.get::<HashMap<String, Variant>>() else {
                    return Vec::new();
                };
                self.model.apply_map(scope, &props)
            }
            // Legacy StateChanged — kept because it is three lines and
            // saves us on NM versions that skimp on the standard
            // properties interface.
            (Some(NM_IFACE), Some("StateChanged")) if path == NM_PATH => {
                if let Ok(state) = sig.body.parser().get::<u32>() {
                    self.model.state = state;
                }
                Vec::new()
            }
            // NM restarted: old owner's chain is dead; re-seed from
            // the root once a new owner exists.
            (Some(DBUS), Some("NameOwnerChanged")) => {
                let mut parser = sig.body.parser();
                let Ok((name, _old, new)) = parser.get3::<&str, &str, &str>() else {
                    return Vec::new();
                };
                if name != NM {
                    return Vec::new();
                }
                self.model = Model::default();
                if new.is_empty() {
                    Vec::new()
                } else {
                    vec![Query::Root]
                }
            }
            _ => Vec::new(),
        }
    }

    /// Resolve any pending `GetAll` replies, collecting follow-ups.
    fn drain_replies(&mut self) -> Vec<Query> {
        let mut queries = Vec::new();
        let pending = std::mem::take(&mut self.pending);
        for (serial, query) in pending {
            let Some(reply) = self.rpc.try_get_response(serial) else {
                self.pending.push((serial, query));
                continue;
            };
            if reply.typ == MessageType::Error {
                // The object vanished between signal and query — the
                // next PrimaryConnection change re-roots the chain.
                tracing::debug!("network: {query:?} query failed");
                continue;
            }
            let Some(scope) = self.model.query_current(&query) else {
                continue; // stale: the chain moved on
            };
            if let Ok(props) = reply.body.parser().get::<HashMap<String, Variant>>() {
                queries.extend(self.model.apply_map(scope, &props));
            }
        }
        queries
    }
}

fn nm_start<D: 'static>(
    handle: &LoopHandle<'static, D>,
    notify: Notify<D>,
) -> Result<(), DbusError> {
    let mut rpc = RpcConn::system_conn(SETUP)?;

    // Matches first, snapshot second (changes racing the GetAll apply
    // on top). One path_namespace rule covers root, connections, APs.
    let rules = [
        format!(
            "type='signal',sender='{NM}',interface='{PROPS_IFACE}',\
             member='PropertiesChanged',path_namespace='{NM_PATH}'"
        ),
        format!(
            "type='signal',sender='{NM}',path='{NM_PATH}',\
             interface='{NM_IFACE}',member='StateChanged'"
        ),
        format!(
            "type='signal',sender='{DBUS}',interface='{DBUS}',\
             member='NameOwnerChanged',arg0='{NM}'"
        ),
    ];
    for rule in &rules {
        let mut add = rustbus::standard_messages::add_match(rule);
        let serial = dbus::send(&mut rpc, &mut add)?;
        dbus::reply_ok(&rpc.wait_response(serial, SETUP)?)?;
    }

    // Walk the chain blocking at setup — three tiny calls, bounded.
    let mut model = Model::default();
    let mut queries = vec![Query::Root];
    while let Some(query) = queries.pop() {
        let (path, iface) = match &query {
            Query::Root => (NM_PATH.to_string(), NM_IFACE),
            Query::Conn(p) => (p.clone(), CONN_IFACE),
            Query::Ap(p) => (p.clone(), AP_IFACE),
        };
        let mut call = dbus::get_all(NM, &path, iface)?;
        let serial = dbus::send(&mut rpc, &mut call)?;
        let reply = rpc.wait_response(serial, SETUP)?;
        if let Query::Root = query {
            // No NM on the bus is an error reply right here.
            dbus::reply_ok(&reply)?;
        } else if reply.typ == MessageType::Error {
            continue; // chain link vanished mid-walk — details stay unknown
        }
        let Some(scope) = model.query_current(&query) else {
            continue;
        };
        if let Ok(props) = reply.body.parser().get::<HashMap<String, Variant>>() {
            queries.extend(model.apply_map(scope, &props));
        }
    }

    // calloop watches a dup of the socket fd; rustbus keeps reading
    // through the original.
    let raw = rpc.conn().as_raw_fd();
    let fd: OwnedFd = unsafe { BorrowedFd::borrow_raw(raw) }.try_clone_to_owned()?;

    let last = model.snapshot();
    let be = Rc::new(RefCell::new(Nm {
        rpc,
        model,
        pending: Vec::new(),
        last,
        notify,
        handle: handle.clone(),
    }));

    // Initial snapshot: `notify` needs the loop's `&mut D`, which only
    // exists inside a source callback.
    let seed = be.clone();
    handle
        .insert_source(Timer::immediate(), move |_, _, data: &mut D| {
            let b = &mut *seed.borrow_mut();
            let state = b.last.clone();
            (b.notify.borrow_mut())(data, &state);
            TimeoutAction::Drop
        })
        .map_err(|e| DbusError::Loop(e.to_string()))?;

    handle
        .insert_source(
            Generic::new(fd, Interest::READ, Mode::Level),
            move |_, _, data: &mut D| {
                let b = &mut *be.borrow_mut();
                match b.rpc.refill_all() {
                    Ok(_) => {
                        let mut queries = Vec::new();
                        while let Some(sig) = b.rpc.try_get_signal() {
                            queries.extend(b.on_signal(&sig));
                        }
                        queries.extend(b.drain_replies());
                        for q in queries {
                            b.send_query(q);
                        }
                        let snap = b.model.snapshot();
                        if snap != b.last {
                            b.last = snap.clone();
                            (b.notify.borrow_mut())(data, &snap);
                        }
                        Ok(PostAction::Continue)
                    }
                    Err(e) => {
                        tracing::warn!("network: system bus lost ({e}); falling back to sysfs");
                        b.model = Model::default();
                        b.last = b.model.snapshot();
                        let state = b.last.clone();
                        (b.notify.borrow_mut())(data, &state);
                        if !sysfs_start(&b.handle, b.notify.clone()) {
                            tracing::info!("no sysfs either — network service stopped");
                        }
                        Ok(PostAction::Remove)
                    }
                }
            },
        )
        .map_err(|e| DbusError::Loop(e.to_string()))?;
    Ok(())
}

// ── sysfs fallback ──────────────────────────────────────────────────

/// Parse a sysfs `operstate` file ("up\n") — nur's contract.
fn parse_operstate(s: &str) -> bool {
    s.trim() == "up"
}

/// Any non-loopback interface with `operstate == up`.
fn sysfs_connected() -> Option<bool> {
    let dir = std::fs::read_dir("/sys/class/net").ok()?;
    Some(dir.filter_map(|e| e.ok()).any(|entry| {
        let name = entry.file_name();
        if name.to_string_lossy() == "lo" {
            return false;
        }
        std::fs::read_to_string(entry.path().join("operstate"))
            .map(|s| parse_operstate(&s))
            .unwrap_or(false)
    }))
}

/// Start the polling fallback. Returns false (and inserts nothing —
/// zero wakeups) when `/sys/class/net` is unreadable.
fn sysfs_start<D: 'static>(handle: &LoopHandle<'static, D>, notify: Notify<D>) -> bool {
    if sysfs_connected().is_none() {
        return false;
    }
    let mut last: Option<NetworkState> = None;
    let inserted = handle.insert_source(Timer::immediate(), move |_, _, data: &mut D| {
        let now = NetworkState {
            connected: sysfs_connected().unwrap_or(false),
            ssid: None,
            strength: 0,
        };
        if last.as_ref() != Some(&now) {
            last = Some(now.clone());
            (notify.borrow_mut())(data, &now);
        }
        TimeoutAction::ToDuration(SYSFS_POLL)
    });
    match inserted {
        Ok(_) => true,
        Err(e) => {
            tracing::error!("network: arming sysfs poll timer: {e}");
            false
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dbus::test_util::props_changed;
    use rustbus::params::{Base, Container, Param};

    fn bytes(b: &[u8]) -> Param<'static, 'static> {
        Param::Container(
            Container::make_array("y", b.iter().map(|x| Param::Base(Base::Byte(*x)))).unwrap(),
        )
    }

    /// Feed one wire-level `PropertiesChanged` through the same
    /// routing the live source uses.
    fn apply_signal(
        model: &mut Model,
        path: &str,
        iface: &str,
        msg: &MarshalledMessage,
    ) -> Vec<Query> {
        let mut parser = msg.body.parser();
        let got_iface: &str = parser.get().unwrap();
        assert_eq!(got_iface, iface);
        let Some(scope) = model.scope_for(iface, path) else {
            return Vec::new();
        };
        let props: HashMap<String, Variant> = parser.get().unwrap();
        model.apply_map(scope, &props)
    }

    #[test]
    fn chain_walk_round_trip() {
        let mut m = Model::default();

        // Root: connected, primary connection appears → query it.
        let msg = props_changed(
            NM_PATH,
            NM_IFACE,
            vec![
                ("State", Param::Base(Base::Uint32(70))),
                (
                    "PrimaryConnection",
                    Param::Base(Base::ObjectPath(
                        "/org/freedesktop/NetworkManager/ActiveConnection/1".into(),
                    )),
                ),
            ],
        );
        let q = apply_signal(&mut m, NM_PATH, NM_IFACE, &msg);
        assert_eq!(
            q,
            vec![Query::Conn(
                "/org/freedesktop/NetworkManager/ActiveConnection/1".into()
            )]
        );
        assert!(m.snapshot().connected);
        assert_eq!(m.snapshot().ssid, None, "type unknown yet — not wifi");

        // The connection: wifi, has an AP → query it.
        let conn = "/org/freedesktop/NetworkManager/ActiveConnection/1";
        let msg = props_changed(
            conn,
            CONN_IFACE,
            vec![
                ("Type", Param::Base(Base::String("802-11-wireless".into()))),
                (
                    "SpecificObject",
                    Param::Base(Base::ObjectPath(
                        "/org/freedesktop/NetworkManager/AccessPoint/7".into(),
                    )),
                ),
            ],
        );
        let q = apply_signal(&mut m, conn, CONN_IFACE, &msg);
        assert_eq!(
            q,
            vec![Query::Ap(
                "/org/freedesktop/NetworkManager/AccessPoint/7".into()
            )]
        );

        // The AP: ssid + strength surface in the snapshot.
        let ap = "/org/freedesktop/NetworkManager/AccessPoint/7";
        let msg = props_changed(
            ap,
            AP_IFACE,
            vec![
                ("Ssid", bytes(b"HomeNet")),
                ("Strength", Param::Base(Base::Byte(87))),
            ],
        );
        let q = apply_signal(&mut m, ap, AP_IFACE, &msg);
        assert!(q.is_empty());
        let s = m.snapshot();
        assert!(s.connected);
        assert_eq!(s.ssid.as_deref(), Some("HomeNet"));
        assert_eq!(s.strength, 87);
    }

    #[test]
    fn reroot_clears_downstream_and_guards_stale_objects() {
        let mut m = Model {
            state: 70,
            primary: "/conn/1".into(),
            conn_type: "802-11-wireless".into(),
            specific: "/ap/1".into(),
            ssid: Some("Old".into()),
            strength: 60,
        };

        // Primary connection switches: downstream state must clear.
        let q = m.apply_root("PrimaryConnection", Val::Path("/conn/2".into()));
        assert_eq!(q, Some(Query::Conn("/conn/2".into())));
        let s = m.snapshot();
        assert_eq!(s.ssid, None);
        assert_eq!(s.strength, 0);
        assert!(s.connected, "state is still connected");

        // Signals from the old AP no longer route anywhere.
        assert_eq!(m.scope_for(AP_IFACE, "/ap/1"), None);
        // Stale query replies are dropped too.
        assert_eq!(m.query_current(&Query::Ap("/ap/1".into())), None);
        assert_eq!(
            m.query_current(&Query::Conn("/conn/2".into())),
            Some(Scope::Conn)
        );
    }

    #[test]
    fn disconnect_drops_everything() {
        let mut m = Model {
            state: 70,
            primary: "/conn/1".into(),
            conn_type: "802-11-wireless".into(),
            ssid: Some("Net".into()),
            strength: 50,
            ..Model::default()
        };
        assert!(m.snapshot().connected);

        assert_eq!(m.apply_root("State", Val::U32(20)), None);
        let q = m.apply_root("PrimaryConnection", Val::Path(NO_PATH.into()));
        assert_eq!(q, None, "no object — nothing to query");
        let s = m.snapshot();
        assert_eq!(s, NetworkState::default());
    }

    #[test]
    fn ethernet_has_no_ssid() {
        let mut m = Model {
            state: 70,
            primary: "/conn/1".into(),
            // Even if stale AP fields lingered, the type gates them out.
            ssid: Some("ghost".into()),
            strength: 40,
            ..Model::default()
        };
        m.apply_conn("Type", Val::Str("802-3-ethernet".into()));
        let s = m.snapshot();
        assert!(s.connected);
        assert_eq!(s.ssid, None);
        assert_eq!(s.strength, 0);
    }

    #[test]
    fn hidden_ssid_is_none() {
        let mut m = Model::default();
        m.apply_ap("Ssid", Val::Bytes(Vec::new()));
        assert_eq!(m.ssid, None);
        m.apply_ap("Ssid", Val::Bytes(b"x".to_vec()));
        assert_eq!(m.ssid.as_deref(), Some("x"));
    }

    #[test]
    fn state_threshold_matches_carrier_semantics() {
        let mut m = Model::default();
        for (state, connected) in [
            (0, false),
            (20, false),
            (40, false),
            (50, true),
            (60, true),
            (70, true),
        ] {
            m.apply_root("State", Val::U32(state));
            assert_eq!(m.snapshot().connected, connected, "state {state}");
        }
    }

    #[test]
    fn mistyped_props_change_nothing() {
        let mut m = Model::default();
        assert_eq!(m.apply_root("State", Val::Str("70".into())), None);
        assert_eq!(m.state, 0);
        assert_eq!(m.apply_conn("Type", Val::U32(3)), None);
        assert!(m.conn_type.is_empty());
        m.apply_ap("Strength", Val::U32(50)); // 'y' expected, not 'u'
        assert_eq!(m.strength, 0);
    }

    #[test]
    fn operstate_parses() {
        assert!(parse_operstate("up"));
        assert!(parse_operstate("up\n"));
        assert!(!parse_operstate("down\n"));
        assert!(!parse_operstate("unknown\n"));
        assert!(!parse_operstate(""));
        assert!(!parse_operstate("UP"), "sysfs is lowercase");
    }

    #[test]
    fn defaults_match_nur() {
        let s = NetworkState::default();
        assert!(!s.connected);
        assert_eq!(s.ssid, None);
        assert_eq!(s.strength, 0);
    }
}
