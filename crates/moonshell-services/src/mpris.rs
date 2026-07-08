//! MPRIS media players over the session D-Bus, playerctld-style.
//!
//! nur polled `playerctl` every 2 s from a thread; this rides the bus
//! directly (the rustbus-as-calloop shape shared with battery and
//! network). Player discovery is `ListNames` at setup plus
//! `NameOwnerChanged` (arg0namespace `org.mpris.MediaPlayer2`) at
//! runtime; state arrives as `PropertiesChanged`/`Seeked` signals
//! matched by the well-known object path `/org/mpris/MediaPlayer2` —
//! one rule covers every player, present and future.
//!
//! Active-player selection is playerctld's heuristic, event-driven:
//! every signal bumps its player's activity counter; the snapshot
//! follows the *playing* player with the freshest activity, falling
//! back to the most recently active one. (nur's "first playing, else
//! first listed" needed polling to notice changes; this needs none.)
//!
//! `Position` is the one MPRIS property that never signals — it is
//! re-queried (nonblocking, resolved via `try_get_response`) whenever
//! a player's status or track changes, and updated by `Seeked`. So
//! `position` is exact at every transition and freezes between them;
//! a widget that wants a running progress bar interpolates in Lua
//! from its own clock — polling a property per second is exactly the
//! wakeup cost this service exists to remove.
//!
//! No fallback: no session bus (or a session bus without players)
//! means the facade keeps its defaults. Actions (play_pause, next, …)
//! stay placeholder until the service write path lands with M4.

use std::cell::RefCell;
use std::collections::HashMap;
use std::os::fd::{AsRawFd, BorrowedFd, OwnedFd};
use std::rc::Rc;

use calloop::generic::Generic;
use calloop::timer::{TimeoutAction, Timer};
use calloop::{Interest, LoopHandle, Mode, PostAction};
use rustbus::message_builder::MarshalledMessage;
use rustbus::wire::unmarshal::traits::Variant;
use rustbus::{MessageBuilder, MessageType, RpcConn};

use crate::dbus::{self, Dbl, DbusError, PROPS_IFACE, SETUP};

const PREFIX: &str = "org.mpris.MediaPlayer2.";
const MPRIS_PATH: &str = "/org/mpris/MediaPlayer2";
const PLAYER_IFACE: &str = "org.mpris.MediaPlayer2.Player";
const DBUS: &str = "org.freedesktop.DBus";
const DBUS_PATH: &str = "/org/freedesktop/DBus";

/// The snapshot pushed to `notify` on every change — nur's shape
/// exactly. `length`/`position` are seconds; `volume` is 0.0–1.0
/// (1.0 when the player doesn't report one).
#[derive(Debug, Clone, PartialEq)]
pub struct MprisState {
    /// Short player name (`"spotify"`, `"firefox.instance_1_23"`).
    /// Empty when no player is running.
    pub player_name: String,
    /// `"Playing"`, `"Paused"`, `"Stopped"`, or `""` when no player.
    pub status: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub art_url: String,
    pub length: u64,
    pub position: u64,
    pub volume: f64,
}

impl Default for MprisState {
    fn default() -> Self {
        Self {
            player_name: String::new(),
            status: String::new(),
            title: String::new(),
            artist: String::new(),
            album: String::new(),
            art_url: String::new(),
            length: 0,
            position: 0,
            volume: 1.0,
        }
    }
}

type Notify<D> = Rc<RefCell<Box<dyn FnMut(&mut D, &MprisState)>>>;

/// Start the mpris service. `Err` means no usable session bus — the
/// facade keeps its defaults, media widgets render empty.
pub fn start<D: 'static>(
    handle: &LoopHandle<'static, D>,
    notify: impl FnMut(&mut D, &MprisState) + 'static,
) -> Result<(), DbusError> {
    let notify: Notify<D> = Rc::new(RefCell::new(Box::new(notify)));
    session_start(handle, notify)
}

/// One player's tracked state (all times in seconds).
#[derive(Debug, Clone, Default, PartialEq)]
struct Player {
    /// Well-known name minus the MPRIS prefix.
    name: String,
    status: String,
    title: String,
    artist: String,
    album: String,
    art_url: String,
    length: u64,
    position: u64,
    /// 1.0 until the player reports one.
    volume: Option<f64>,
    /// Monotonic event counter value at this player's last signal.
    activity: u64,
}

/// The pure model: bus events in, snapshot out.
#[derive(Default)]
struct Model {
    /// Keyed by the owner's *unique* name (`:1.42`) — signals carry
    /// that as their sender, and it survives nothing (no stale keys).
    players: HashMap<String, Player>,
    clock: u64,
}

impl Model {
    fn add(&mut self, unique: &str, well_known: &str) {
        self.clock += 1;
        let player = Player {
            name: well_known
                .strip_prefix(PREFIX)
                .unwrap_or(well_known)
                .to_string(),
            activity: self.clock,
            ..Player::default()
        };
        self.players.insert(unique.to_string(), player);
    }

    fn remove(&mut self, unique: &str) -> bool {
        self.players.remove(unique).is_some()
    }

    fn touch(&mut self, unique: &str) {
        self.clock += 1;
        if let Some(p) = self.players.get_mut(unique) {
            p.activity = self.clock;
        }
    }

    /// Fold a `PlaybackStatus`/`Metadata`/`Volume` property map into
    /// one player. Returns true when `Position` should be re-queried
    /// (status flip or track change — the transitions where a stale
    /// position would lie).
    fn apply_props(&mut self, unique: &str, props: &HashMap<String, Variant>) -> bool {
        self.touch(unique);
        let Some(p) = self.players.get_mut(unique) else {
            return false;
        };
        let mut position_stale = false;
        for (key, v) in props {
            match key.as_str() {
                "PlaybackStatus" => {
                    if let Ok(s) = v.get::<String>() {
                        position_stale |= p.status != s;
                        p.status = s;
                    }
                }
                "Volume" => {
                    if let Ok(d) = v.get::<Dbl>() {
                        p.volume = Some(d.0);
                    }
                }
                "Metadata" => {
                    if let Ok(meta) = v.get::<HashMap<String, Variant>>() {
                        apply_metadata(p, &meta);
                        // New track: the old offset is meaningless.
                        p.position = 0;
                        position_stale = true;
                    }
                }
                _ => {}
            }
        }
        position_stale
    }

    fn set_position(&mut self, unique: &str, us: i64) {
        self.touch(unique);
        if let Some(p) = self.players.get_mut(unique) {
            p.position = us.max(0) as u64 / 1_000_000;
        }
    }

    /// playerctld's heuristic: the freshest *playing* player, else the
    /// freshest player at all. Name is the determinism tiebreak.
    fn active(&self) -> Option<&Player> {
        self.players.values().max_by_key(|p| {
            (
                p.status == "Playing",
                p.activity,
                std::cmp::Reverse(&p.name),
            )
        })
    }

    fn snapshot(&self) -> MprisState {
        let Some(p) = self.active() else {
            return MprisState::default();
        };
        MprisState {
            player_name: p.name.clone(),
            status: p.status.clone(),
            title: p.title.clone(),
            artist: p.artist.clone(),
            album: p.album.clone(),
            art_url: p.art_url.clone(),
            length: p.length,
            position: p.position,
            volume: p.volume.unwrap_or(1.0),
        }
    }
}

/// Fold an MPRIS `Metadata` dict (`a{sv}`) into a player. Length
/// arrives as `x` per spec, but real players also send `t` and `d` —
/// all accepted (the version-skew tolerance rule).
fn apply_metadata(p: &mut Player, meta: &HashMap<String, Variant>) {
    p.title = meta
        .get("xesam:title")
        .and_then(|v| v.get::<String>().ok())
        .unwrap_or_default();
    p.artist = meta
        .get("xesam:artist")
        .and_then(|v| {
            v.get::<Vec<String>>()
                .map(|a| a.join(", "))
                .or_else(|_| v.get::<String>())
                .ok()
        })
        .unwrap_or_default();
    p.album = meta
        .get("xesam:album")
        .and_then(|v| v.get::<String>().ok())
        .unwrap_or_default();
    p.art_url = meta
        .get("mpris:artUrl")
        .and_then(|v| v.get::<String>().ok())
        .unwrap_or_default();
    p.length = meta
        .get("mpris:length")
        .and_then(|v| {
            v.get::<i64>()
                .map(|x| x.max(0) as u64)
                .or_else(|_| v.get::<u64>())
                .or_else(|_| v.get::<Dbl>().map(|d| d.0.max(0.0) as u64))
                .ok()
        })
        .map(|us| us / 1_000_000)
        .unwrap_or(0);
}

/// In-flight method calls: serial → what was asked.
enum Pending {
    /// `GetAll` seeding a newly appeared player.
    Seed { unique: String },
    /// `Get("Position")` after a status/track transition.
    Position { unique: String },
}

struct Mpris<D> {
    rpc: RpcConn,
    model: Model,
    pending: Vec<(u32, Pending)>,
    last: MprisState,
    notify: Notify<D>,
}

impl<D> Mpris<D> {
    fn send_get_all(&mut self, unique: &str) {
        let sent = dbus::get_all(unique, MPRIS_PATH, PLAYER_IFACE)
            .and_then(|mut call| dbus::send(&mut self.rpc, &mut call));
        match sent {
            Ok(serial) => self.pending.push((
                serial,
                Pending::Seed {
                    unique: unique.to_string(),
                },
            )),
            Err(e) => tracing::debug!("mpris: seeding {unique}: {e}"),
        }
    }

    fn send_get_position(&mut self, unique: &str) {
        let sent = dbus::get_one(unique, MPRIS_PATH, PLAYER_IFACE, "Position")
            .and_then(|mut call| dbus::send(&mut self.rpc, &mut call));
        match sent {
            Ok(serial) => self.pending.push((
                serial,
                Pending::Position {
                    unique: unique.to_string(),
                },
            )),
            Err(e) => tracing::debug!("mpris: querying position of {unique}: {e}"),
        }
    }

    fn on_signal(&mut self, sig: &MarshalledMessage) {
        let sender = sig.dynheader.sender.as_deref().unwrap_or_default();
        let path = sig.dynheader.object.as_deref().unwrap_or_default();
        match (
            sig.dynheader.interface.as_deref(),
            sig.dynheader.member.as_deref(),
        ) {
            (Some(PROPS_IFACE), Some("PropertiesChanged")) if path == MPRIS_PATH => {
                let mut parser = sig.body.parser();
                match parser.get::<&str>() {
                    Ok(PLAYER_IFACE) => {}
                    _ => return,
                }
                let Ok(props) = parser.get::<HashMap<String, Variant>>() else {
                    return;
                };
                if !self.model.players.contains_key(sender) {
                    // A player we never saw appear (it beat our match
                    // registration or claimed its name late): signals
                    // from /org/mpris/MediaPlayer2 are proof enough.
                    self.model.add(sender, "");
                    self.send_get_all(sender);
                }
                if self.model.apply_props(sender, &props) {
                    self.send_get_position(sender);
                }
            }
            (Some(PLAYER_IFACE), Some("Seeked")) if path == MPRIS_PATH => {
                if let Ok(us) = sig.body.parser().get::<i64>() {
                    self.model.set_position(sender, us);
                }
            }
            (Some(DBUS), Some("NameOwnerChanged")) => {
                let Ok((name, old, new)) = sig.body.parser().get3::<&str, &str, &str>() else {
                    return;
                };
                if !name.starts_with(PREFIX) {
                    return;
                }
                if !old.is_empty() {
                    self.model.remove(old);
                }
                if !new.is_empty() {
                    self.model.add(new, name);
                    self.send_get_all(new);
                }
            }
            _ => {}
        }
    }

    fn drain_replies(&mut self) {
        let mut followups: Vec<String> = Vec::new();
        let pending = std::mem::take(&mut self.pending);
        for (serial, what) in pending {
            let Some(reply) = self.rpc.try_get_response(serial) else {
                self.pending.push((serial, what));
                continue;
            };
            if reply.typ == MessageType::Error {
                continue; // player quit before answering
            }
            match what {
                Pending::Seed { unique } => {
                    if let Ok(props) = reply.body.parser().get::<HashMap<String, Variant>>() {
                        if self.model.players.contains_key(&unique)
                            && self.model.apply_props(&unique, &props)
                        {
                            followups.push(unique);
                        }
                    }
                }
                Pending::Position { unique } => {
                    if let Ok(v) = reply.body.parser().get::<Variant>() {
                        if let Ok(us) = v.get::<i64>() {
                            self.model.set_position(&unique, us);
                        }
                    }
                }
            }
        }
        for unique in followups {
            self.send_get_position(&unique);
        }
    }
}

fn session_start<D: 'static>(
    handle: &LoopHandle<'static, D>,
    notify: Notify<D>,
) -> Result<(), DbusError> {
    let mut rpc = RpcConn::session_conn(SETUP)?;

    // Matches first, discovery second (a player appearing mid-setup
    // lands as a NameOwnerChanged we already receive).
    let rules = [
        format!(
            "type='signal',sender='{DBUS}',interface='{DBUS}',\
             member='NameOwnerChanged',arg0namespace='org.mpris.MediaPlayer2'"
        ),
        format!(
            "type='signal',path='{MPRIS_PATH}',interface='{PROPS_IFACE}',\
             member='PropertiesChanged'"
        ),
        format!("type='signal',path='{MPRIS_PATH}',interface='{PLAYER_IFACE}',member='Seeked'"),
    ];
    for rule in &rules {
        let mut add = rustbus::standard_messages::add_match(rule);
        let serial = dbus::send(&mut rpc, &mut add)?;
        dbus::reply_ok(&rpc.wait_response(serial, SETUP)?)?;
    }

    // Enumerate the players already running (blocking at setup; a
    // handful of tiny calls, all bounded).
    let mut list = rustbus::standard_messages::list_names();
    let serial = dbus::send(&mut rpc, &mut list)?;
    let reply = rpc.wait_response(serial, SETUP)?;
    dbus::reply_ok(&reply)?;
    let names: Vec<String> = reply.body.parser().get().unwrap_or_default();

    let mut model = Model::default();
    let mut seeds: Vec<String> = Vec::new();
    for name in names.iter().filter(|n| n.starts_with(PREFIX)) {
        let mut owner_call = MessageBuilder::new()
            .call("GetNameOwner")
            .with_interface(DBUS)
            .on(DBUS_PATH)
            .at(DBUS)
            .build();
        owner_call.body.push_param(name.as_str())?;
        let serial = dbus::send(&mut rpc, &mut owner_call)?;
        let reply = rpc.wait_response(serial, SETUP)?;
        if reply.typ == MessageType::Error {
            continue; // gone already
        }
        let Ok(unique) = reply.body.parser().get::<String>() else {
            continue;
        };
        model.add(&unique, name);
        seeds.push(unique);
    }

    let raw = rpc.conn().as_raw_fd();
    let fd: OwnedFd = unsafe { BorrowedFd::borrow_raw(raw) }.try_clone_to_owned()?;

    let last = model.snapshot();
    let be = Rc::new(RefCell::new(Mpris {
        rpc,
        model,
        pending: Vec::new(),
        last,
        notify,
    }));

    // Seed the running players' state through the nonblocking path —
    // replies resolve on the first fd wakeup.
    {
        let b = &mut *be.borrow_mut();
        for unique in seeds {
            b.send_get_all(&unique);
        }
    }

    // Initial snapshot (defaults or the empty-player state): `notify`
    // needs the loop's `&mut D`.
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
                        while let Some(sig) = b.rpc.try_get_signal() {
                            b.on_signal(&sig);
                        }
                        b.drain_replies();
                        let snap = b.model.snapshot();
                        if snap != b.last {
                            b.last = snap.clone();
                            (b.notify.borrow_mut())(data, &snap);
                        }
                        Ok(PostAction::Continue)
                    }
                    Err(e) => {
                        // The session bus died — that session is over.
                        tracing::warn!("mpris: session bus lost ({e}); service stopped");
                        b.model = Model::default();
                        b.last = MprisState::default();
                        let state = b.last.clone();
                        (b.notify.borrow_mut())(data, &state);
                        Ok(PostAction::Remove)
                    }
                }
            },
        )
        .map_err(|e| DbusError::Loop(e.to_string()))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dbus::test_util::{dict, props_changed};
    use rustbus::params::{Base, Container, Param};

    fn metadata(entries: Vec<(&str, Param<'static, 'static>)>) -> Param<'static, 'static> {
        Param::Container(dict(entries))
    }

    /// Parse a wire-level `PropertiesChanged` into a props map, the
    /// way the live source does.
    fn parse_props(msg: &MarshalledMessage) -> HashMap<String, Variant<'_, '_>> {
        let mut parser = msg.body.parser();
        let iface: &str = parser.get().unwrap();
        assert_eq!(iface, PLAYER_IFACE);
        parser.get().unwrap()
    }

    #[test]
    fn metadata_round_trip() {
        let msg = props_changed(
            MPRIS_PATH,
            PLAYER_IFACE,
            vec![
                (
                    "PlaybackStatus",
                    Param::Base(Base::String("Playing".into())),
                ),
                (
                    "Metadata",
                    metadata(vec![
                        ("xesam:title", Param::Base(Base::String("Song".into()))),
                        (
                            "xesam:artist",
                            Param::Container(
                                Container::make_array(
                                    "s",
                                    vec![
                                        Param::Base(Base::String("A".into())),
                                        Param::Base(Base::String("B".into())),
                                    ]
                                    .into_iter(),
                                )
                                .unwrap(),
                            ),
                        ),
                        ("xesam:album", Param::Base(Base::String("Album".into()))),
                        (
                            "mpris:artUrl",
                            Param::Base(Base::String("file:///art".into())),
                        ),
                        ("mpris:length", Param::Base(Base::Int64(185_000_000))),
                    ]),
                ),
                ("Volume", Param::Base(Base::Double(f64::to_bits(0.8)))),
            ],
        );
        let mut m = Model::default();
        m.add(":1.5", "org.mpris.MediaPlayer2.spotify");
        let wants_position = m.apply_props(":1.5", &parse_props(&msg));
        assert!(wants_position, "track + status changed");

        let s = m.snapshot();
        assert_eq!(s.player_name, "spotify");
        assert_eq!(s.status, "Playing");
        assert_eq!(s.title, "Song");
        assert_eq!(s.artist, "A, B");
        assert_eq!(s.album, "Album");
        assert_eq!(s.art_url, "file:///art");
        assert_eq!(s.length, 185);
        assert_eq!(s.position, 0, "track change resets position");
        assert!((s.volume - 0.8).abs() < 1e-9);
    }

    #[test]
    fn volume_only_change_needs_no_position() {
        let msg = props_changed(
            MPRIS_PATH,
            PLAYER_IFACE,
            vec![("Volume", Param::Base(Base::Double(f64::to_bits(0.5))))],
        );
        let mut m = Model::default();
        m.add(":1.5", "org.mpris.MediaPlayer2.mpv");
        assert!(!m.apply_props(":1.5", &parse_props(&msg)));
        assert!((m.snapshot().volume - 0.5).abs() < 1e-9);
    }

    #[test]
    fn playing_wins_over_fresher_paused() {
        let mut m = Model::default();
        m.add(":1.1", "org.mpris.MediaPlayer2.spotify");
        m.add(":1.2", "org.mpris.MediaPlayer2.mpv");
        m.players.get_mut(":1.1").unwrap().status = "Playing".into();
        m.touch(":1.2"); // paused player is fresher…
        m.players.get_mut(":1.2").unwrap().status = "Paused".into();
        assert_eq!(m.snapshot().player_name, "spotify", "…but Playing wins");

        // Both paused: recency decides.
        m.players.get_mut(":1.1").unwrap().status = "Paused".into();
        assert_eq!(m.snapshot().player_name, "mpv");
    }

    #[test]
    fn removal_falls_back_to_the_next_player() {
        let mut m = Model::default();
        m.add(":1.1", "org.mpris.MediaPlayer2.spotify");
        m.add(":1.2", "org.mpris.MediaPlayer2.mpv");
        m.players.get_mut(":1.2").unwrap().status = "Playing".into();
        assert_eq!(m.snapshot().player_name, "mpv");
        assert!(m.remove(":1.2"));
        assert_eq!(m.snapshot().player_name, "spotify");
        assert!(m.remove(":1.1"));
        assert_eq!(m.snapshot(), MprisState::default());
        assert!(!m.remove(":1.1"), "double remove is inert");
    }

    #[test]
    fn seeked_updates_position_in_seconds() {
        let mut m = Model::default();
        m.add(":1.1", "org.mpris.MediaPlayer2.mpv");
        m.set_position(":1.1", 42_500_000);
        assert_eq!(m.snapshot().position, 42);
        m.set_position(":1.1", -5); // garbage clamps
        assert_eq!(m.snapshot().position, 0);
    }

    #[test]
    fn artist_as_plain_string_is_tolerated() {
        let mut p = Player::default();
        let msg = props_changed(
            MPRIS_PATH,
            PLAYER_IFACE,
            vec![(
                "Metadata",
                metadata(vec![(
                    "xesam:artist",
                    Param::Base(Base::String("Solo".into())),
                )]),
            )],
        );
        let props = parse_props(&msg);
        let meta = props
            .get("Metadata")
            .unwrap()
            .get::<HashMap<String, Variant>>()
            .unwrap();
        apply_metadata(&mut p, &meta);
        assert_eq!(p.artist, "Solo");
    }

    #[test]
    fn defaults_match_nur() {
        let s = MprisState::default();
        assert!(s.player_name.is_empty());
        assert!(s.status.is_empty());
        assert_eq!(s.length, 0);
        assert_eq!(s.position, 0);
        assert!((s.volume - 1.0).abs() < 1e-9, "volume defaults to 1.0");
    }

    #[test]
    fn unknown_player_props_are_inert() {
        let mut m = Model::default();
        let msg = props_changed(
            MPRIS_PATH,
            PLAYER_IFACE,
            vec![(
                "PlaybackStatus",
                Param::Base(Base::String("Playing".into())),
            )],
        );
        assert!(!m.apply_props(":1.9", &parse_props(&msg)));
        assert_eq!(m.snapshot(), MprisState::default());
    }
}
