//! Battery state — UPower over the system D-Bus, sysfs polling as the
//! fallback.
//!
//! The D-Bus connection is rustbus (pure-Rust, synchronous): after a
//! blocking setup (connect + AddMatch + one `GetAll`), the socket fd
//! lives as a calloop `Generic` source and `refill_all()` drains it
//! nonblocking — no async executor, no threads, no idle wakeups. The
//! match rule is registered *before* the snapshot request, so a
//! `PropertiesChanged` racing the `GetAll` applies on top of it
//! instead of being lost (the compositor backends' discipline).
//!
//! Everything rides UPower's aggregate `DisplayDevice`, which is what
//! a bar wants (one number, multi-battery laptops pre-combined) and
//! also covers hotplug: a battery appearing/vanishing flips its
//! `IsPresent` via `PropertiesChanged`, no `DeviceAdded` tracking
//! needed.
//!
//! Fallback (no system bus / no UPower / bus lost later): read
//! `/sys/class/power_supply/BAT*/{capacity,status}` on a 30 s calloop
//! timer. sysfs attribute writes do not fire inotify, so a timer is
//! the honest mechanism — the one recorded divergence from the
//! zero-idle-wakeup rule, and it only exists on UPower-less systems
//! that actually have a battery. No battery and no UPower = no source
//! at all.

use std::cell::RefCell;
use std::collections::HashMap;
use std::os::fd::{AsRawFd, BorrowedFd, OwnedFd};
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::time::Duration;

use calloop::generic::Generic;
use calloop::timer::{TimeoutAction, Timer};
use calloop::{Interest, LoopHandle, Mode, PostAction};
use rustbus::message_builder::MarshalledMessage;
use rustbus::wire::unmarshal::traits::Variant;
use rustbus::{MessageType, RpcConn};

use crate::dbus::{self, Dbl, DbusError, PROPS_IFACE, SETUP};

const UPOWER: &str = "org.freedesktop.UPower";
const DEV_PATH: &str = "/org/freedesktop/UPower/devices/DisplayDevice";
const DEV_IFACE: &str = "org.freedesktop.UPower.Device";
/// UPower's `State` value for "charging". `PendingCharge`/`Full` count
/// as not charging — matches nur's sysfs semantics (`status ==
/// "Charging"`).
const STATE_CHARGING: u32 = 1;
/// sysfs fallback poll cadence.
const SYSFS_POLL: Duration = Duration::from_secs(30);

/// The snapshot pushed to `notify` on every change. nur's shape
/// (`percent`, `charging`) plus `available`, so desktops without a
/// battery can be told apart from a full one — the defaults keep
/// nur's no-battery rendering (100%, not charging).
#[derive(Debug, Clone, PartialEq)]
pub struct BatteryState {
    pub available: bool,
    pub percent: u8,
    pub charging: bool,
}

impl Default for BatteryState {
    fn default() -> Self {
        Self {
            available: false,
            percent: 100,
            charging: false,
        }
    }
}

/// Which backend ended up feeding the service (for the boot log).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Source {
    Upower,
    Sysfs,
    /// No UPower and no sysfs battery — nothing runs, the facade
    /// keeps its defaults.
    None,
}

impl std::fmt::Display for Source {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Source::Upower => "UPower",
            Source::Sysfs => "sysfs (30 s poll)",
            Source::None => "none (no battery)",
        })
    }
}

/// Shared so the UPower backend can hand it to the sysfs fallback if
/// the bus dies mid-flight.
type Notify<D> = Rc<RefCell<Box<dyn FnMut(&mut D, &BatteryState)>>>;

/// Start the battery service. `notify` receives a full snapshot after
/// every change. Never fails: UPower trouble degrades to sysfs, no
/// battery anywhere degrades to nothing.
pub fn start<D: 'static>(
    handle: &LoopHandle<'static, D>,
    notify: impl FnMut(&mut D, &BatteryState) + 'static,
) -> Source {
    let notify: Notify<D> = Rc::new(RefCell::new(Box::new(notify)));
    match upower_start(handle, notify.clone()) {
        Ok(()) => Source::Upower,
        Err(e) => {
            tracing::info!("UPower unavailable ({e}); trying sysfs");
            if sysfs_start(handle, notify) {
                Source::Sysfs
            } else {
                Source::None
            }
        }
    }
}

/// One typed value pulled out of a D-Bus variant — the model speaks
/// this, not wire types, so it unit-tests without a bus.
#[derive(Debug, Clone, Copy, PartialEq)]
enum Prop {
    F64(f64),
    U32(u32),
    Bool(bool),
}

fn prop(v: &Variant) -> Option<Prop> {
    v.get::<Dbl>()
        .ok()
        .map(|d| Prop::F64(d.0))
        .or_else(|| v.get::<u32>().ok().map(Prop::U32))
        .or_else(|| v.get::<bool>().ok().map(Prop::Bool))
}

/// The pure property model: D-Bus properties in, snapshot out.
///
/// Raw properties and the published snapshot are distinct: a
/// DisplayDevice with `IsPresent = false` (desktops) reports
/// `Percentage = 0`, but the snapshot must keep nur's no-battery
/// defaults (100%, not charging) — `available` is the honest flag,
/// the numbers stay render-safe for widgets that don't check it.
#[derive(Default)]
struct Model {
    raw: BatteryState,
}

impl Model {
    /// Apply one raw property. Returns true if it changed (the
    /// *snapshot* may still be unchanged — [`apply_props`] compares).
    fn apply(&mut self, key: &str, value: Prop) -> bool {
        let s = &mut self.raw;
        match (key, value) {
            ("Percentage", Prop::F64(p)) => {
                let pct = p.clamp(0.0, 100.0).round() as u8;
                let changed = s.percent != pct;
                s.percent = pct;
                changed
            }
            ("State", Prop::U32(v)) => {
                let charging = v == STATE_CHARGING;
                let changed = s.charging != charging;
                s.charging = charging;
                changed
            }
            ("IsPresent", Prop::Bool(b)) => {
                let changed = s.available != b;
                s.available = b;
                changed
            }
            _ => false,
        }
    }

    /// The published state: raw when a battery is present, nur's
    /// defaults when not.
    fn snapshot(&self) -> BatteryState {
        if self.raw.available {
            self.raw.clone()
        } else {
            BatteryState::default()
        }
    }

    /// Fold every recognized property of a `GetAll` reply or a
    /// `PropertiesChanged` signal into the model. Returns true if the
    /// published snapshot changed. Signals carry the interface name as
    /// their first argument; wrong interface (or an unrelated signal
    /// like `NameAcquired`) changes nothing.
    fn apply_props(&mut self, msg: &MarshalledMessage) -> bool {
        let mut parser = msg.body.parser();
        if msg.typ == MessageType::Signal {
            match parser.get::<&str>() {
                Ok(iface) if iface == DEV_IFACE => {}
                _ => return false,
            }
        }
        let Ok(changed) = parser.get::<HashMap<String, Variant>>() else {
            return false;
        };
        let before = self.snapshot();
        for (key, value) in &changed {
            if let Some(p) = prop(value) {
                self.apply(key, p);
            }
        }
        self.snapshot() != before
    }
}

struct Upower<D> {
    rpc: RpcConn,
    model: Model,
    notify: Notify<D>,
    handle: LoopHandle<'static, D>,
}

fn upower_start<D: 'static>(
    handle: &LoopHandle<'static, D>,
    notify: Notify<D>,
) -> Result<(), DbusError> {
    let mut rpc = RpcConn::system_conn(SETUP)?;

    // Match first, snapshot second: a change racing the GetAll is
    // applied on top, never lost.
    let rule = format!(
        "type='signal',sender='{UPOWER}',path='{DEV_PATH}',\
         interface='{PROPS_IFACE}',member='PropertiesChanged'"
    );
    let mut add = rustbus::standard_messages::add_match(&rule);
    let serial = dbus::send(&mut rpc, &mut add)?;
    dbus::reply_ok(&rpc.wait_response(serial, SETUP)?)?;

    // GetAll also auto-activates upowerd if the bus knows it; an error
    // reply (no UPower installed) falls back to sysfs in the caller.
    let mut call = dbus::get_all(UPOWER, DEV_PATH, DEV_IFACE)?;
    let serial = dbus::send(&mut rpc, &mut call)?;
    let reply = rpc.wait_response(serial, SETUP)?;
    dbus::reply_ok(&reply)?;

    let mut model = Model::default();
    model.apply_props(&reply);

    // calloop watches a dup of the socket fd; rustbus keeps reading
    // through the original (the watcher's dup'd-fd technique — source
    // teardown order can't invalidate either side).
    let raw = rpc.conn().as_raw_fd();
    let fd: OwnedFd = unsafe { BorrowedFd::borrow_raw(raw) }.try_clone_to_owned()?;

    let be = Rc::new(RefCell::new(Upower {
        rpc,
        model,
        notify,
        handle: handle.clone(),
    }));

    // Initial snapshot: `notify` needs the loop's `&mut D`, which only
    // exists inside a source callback (the M3 §2 lesson).
    let seed = be.clone();
    handle
        .insert_source(Timer::immediate(), move |_, _, data: &mut D| {
            let b = &mut *seed.borrow_mut();
            let state = b.model.snapshot();
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
                        let mut changed = false;
                        while let Some(sig) = b.rpc.try_get_signal() {
                            if sig.dynheader.member.as_deref() == Some("PropertiesChanged")
                                && sig.dynheader.object.as_deref() == Some(DEV_PATH)
                            {
                                changed |= b.model.apply_props(&sig);
                            }
                        }
                        if changed {
                            let state = b.model.snapshot();
                            (b.notify.borrow_mut())(data, &state);
                        }
                        Ok(PostAction::Continue)
                    }
                    Err(e) => {
                        // The system bus died under us (usually the
                        // system going down). Degrade to sysfs; reset
                        // first so a bar without sysfs shows defaults,
                        // not stale charge.
                        tracing::warn!("UPower bus lost ({e}); falling back to sysfs");
                        b.model = Model::default();
                        let state = b.model.snapshot();
                        (b.notify.borrow_mut())(data, &state);
                        if !sysfs_start(&b.handle, b.notify.clone()) {
                            tracing::info!("no sysfs battery either — battery service stopped");
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

/// First `/sys/class/power_supply/*` whose `type` is `Battery`
/// (sorted — deterministic when there are several; the first is the
/// primary on every multi-battery layout seen in the wild).
fn find_battery() -> Option<PathBuf> {
    let mut batteries: Vec<PathBuf> = std::fs::read_dir("/sys/class/power_supply")
        .ok()?
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| {
            std::fs::read_to_string(p.join("type"))
                .map(|t| t.trim() == "Battery")
                .unwrap_or(false)
        })
        .collect();
    batteries.sort();
    batteries.into_iter().next()
}

fn read_sysfs(dir: &Path) -> BatteryState {
    BatteryState {
        available: true,
        percent: std::fs::read_to_string(dir.join("capacity"))
            .map(|s| parse_capacity(&s))
            .unwrap_or(100),
        charging: std::fs::read_to_string(dir.join("status"))
            .map(|s| parse_status(&s))
            .unwrap_or(false),
    }
}

/// Parse a sysfs capacity string ("75\n") — nur's contract: garbage
/// falls back to 100, values over 100 (some firmwares) clamp.
fn parse_capacity(s: &str) -> u8 {
    s.trim().parse::<u8>().map(|p| p.min(100)).unwrap_or(100)
}

/// Parse a sysfs status string ("Charging\n").
fn parse_status(s: &str) -> bool {
    s.trim() == "Charging"
}

/// Start the polling fallback. Returns false (and inserts nothing —
/// zero wakeups) when no battery exists.
fn sysfs_start<D: 'static>(handle: &LoopHandle<'static, D>, notify: Notify<D>) -> bool {
    let Some(dir) = find_battery() else {
        return false;
    };
    let mut last: Option<BatteryState> = None;
    let inserted = handle.insert_source(Timer::immediate(), move |_, _, data: &mut D| {
        let now = read_sysfs(&dir);
        if last.as_ref() != Some(&now) {
            last = Some(now.clone());
            (notify.borrow_mut())(data, &now);
        }
        TimeoutAction::ToDuration(SYSFS_POLL)
    });
    match inserted {
        Ok(_) => true,
        Err(e) => {
            tracing::error!("battery: arming sysfs poll timer: {e}");
            false
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rustbus::params::{Base, Param};

    /// Wire-level `PropertiesChanged` (`s a{sv} as`) at the
    /// DisplayDevice path — real bytes through the shared marshaller.
    fn props_changed(
        iface: &str,
        props: Vec<(&str, Param<'static, 'static>)>,
    ) -> MarshalledMessage {
        crate::dbus::test_util::props_changed(DEV_PATH, iface, props)
    }

    #[test]
    fn properties_changed_round_trip() {
        let mut m = Model::default();
        let msg = props_changed(
            DEV_IFACE,
            vec![
                ("IsPresent", Param::Base(Base::Boolean(true))),
                ("Percentage", Param::Base(Base::Double(f64::to_bits(73.4)))),
                ("State", Param::Base(Base::Uint32(STATE_CHARGING))),
            ],
        );
        assert!(m.apply_props(&msg));
        let s = m.snapshot();
        assert!(s.available && s.charging);
        assert_eq!(s.percent, 73);

        // Wrong interface argument: ignored wholesale.
        let other = props_changed(
            "org.freedesktop.NotUPower",
            vec![("Percentage", Param::Base(Base::Double(f64::to_bits(1.0))))],
        );
        assert!(!m.apply_props(&other));
        assert_eq!(m.snapshot().percent, 73);
    }

    #[test]
    fn defaults_match_nur() {
        let s = BatteryState::default();
        assert!(!s.available);
        assert_eq!(s.percent, 100);
        assert!(!s.charging);
    }

    #[test]
    fn apply_percentage_rounds_and_reports_change() {
        let mut m = Model::default();
        assert!(m.apply("Percentage", Prop::F64(54.6)));
        assert_eq!(m.raw.percent, 55);
        assert!(!m.apply("Percentage", Prop::F64(54.6)), "no change");
        assert!(m.apply("Percentage", Prop::F64(0.2)));
        assert_eq!(m.raw.percent, 0);
    }

    #[test]
    fn apply_percentage_clamps() {
        let mut m = Model::default();
        assert!(m.apply("Percentage", Prop::F64(-3.0)));
        assert_eq!(m.raw.percent, 0);
        assert!(m.apply("Percentage", Prop::F64(120.0)));
        assert_eq!(m.raw.percent, 100);
        assert!(
            !m.apply("Percentage", Prop::F64(150.0)),
            "both clamp to 100 — no change"
        );
    }

    #[test]
    fn apply_state_maps_charging_only() {
        let mut m = Model::default();
        assert!(m.apply("State", Prop::U32(STATE_CHARGING)));
        assert!(m.raw.charging);
        // Discharging, Empty, FullyCharged, PendingCharge: not charging.
        for other in [2u32, 3, 4, 5] {
            m.raw.charging = true;
            assert!(m.apply("State", Prop::U32(other)));
            assert!(!m.raw.charging);
        }
    }

    #[test]
    fn apply_is_present() {
        let mut m = Model::default();
        assert!(m.apply("IsPresent", Prop::Bool(true)));
        assert!(m.raw.available);
        assert!(!m.apply("IsPresent", Prop::Bool(true)));
    }

    #[test]
    fn unknown_or_mistyped_props_change_nothing() {
        let mut m = Model::default();
        assert!(!m.apply("Voltage", Prop::F64(12.1)));
        assert!(!m.apply("Percentage", Prop::U32(50)), "wrong type");
        assert!(!m.apply("State", Prop::F64(1.0)), "wrong type");
        assert_eq!(m.raw, BatteryState::default());
    }

    #[test]
    fn snapshot_gates_on_available() {
        // A desktop's DisplayDevice: present=false, percentage=0.
        let mut m = Model::default();
        m.apply("Percentage", Prop::F64(0.0));
        m.apply("IsPresent", Prop::Bool(false));
        assert_eq!(
            m.snapshot(),
            BatteryState::default(),
            "no battery ⇒ nur's defaults, not a scary 0%"
        );
        // The battery appears: raw values surface.
        m.apply("IsPresent", Prop::Bool(true));
        m.apply("Percentage", Prop::F64(42.0));
        m.apply("State", Prop::U32(STATE_CHARGING));
        let s = m.snapshot();
        assert!(s.available && s.charging);
        assert_eq!(s.percent, 42);
    }

    #[test]
    fn capacity_parses_and_falls_back() {
        assert_eq!(parse_capacity("75"), 75);
        assert_eq!(parse_capacity("42\n"), 42);
        assert_eq!(parse_capacity("  88  "), 88);
        assert_eq!(parse_capacity("0"), 0);
        assert_eq!(parse_capacity("100"), 100);
        assert_eq!(parse_capacity(""), 100);
        assert_eq!(parse_capacity("unknown"), 100);
        assert_eq!(parse_capacity("75.5"), 100, "sysfs gives integers");
    }

    #[test]
    fn status_is_exact_charging() {
        assert!(parse_status("Charging"));
        assert!(parse_status("Charging\n"));
        assert!(!parse_status("Discharging\n"));
        assert!(!parse_status("Full\n"));
        assert!(!parse_status("Not charging"));
        assert!(!parse_status(""));
        assert!(!parse_status("charging"), "case sensitive");
    }
}
