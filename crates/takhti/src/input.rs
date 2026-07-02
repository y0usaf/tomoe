use anyhow::{bail, Result};
use smithay::backend::input::{
    AbsolutePositionEvent, Axis, AxisSource, ButtonState, Event, InputBackend, InputEvent,
    KeyState, KeyboardKeyEvent, PointerAxisEvent, PointerButtonEvent, PointerMotionEvent,
};
use smithay::input::keyboard::keysyms;
use smithay::input::keyboard::xkb::{keysym_from_name, KEYSYM_CASE_INSENSITIVE, KEYSYM_NO_FLAGS};
use smithay::input::keyboard::{FilterResult, Keysym, ModifiersState};
use smithay::input::pointer::{AxisFrame, ButtonEvent, MotionEvent};
use smithay::utils::SERIAL_COUNTER;

use crate::state::Takhti;

/// A dispatchable compositor action. Lua binds queue these; built-in binds
/// carry them directly.
#[derive(Debug, Clone)]
pub enum Action {
    /// Request exit: opens the confirmation dialog.
    Quit,
    /// Exit immediately (Enter in the dialog, or the "quit!" action string).
    ConfirmQuit,
    CloseWindow,
    Spawn(String),
    ShowHotkeyOverlay,
    ReloadConfig,
    /// Call the Lua function registered for bind index `idx`.
    LuaFn(usize),
    /// Switch virtual terminal (TTY backend; Ctrl+Alt+F1..F12).
    ChangeVt(i32),
}

impl Action {
    /// Parse a built-in action string from config, e.g. "close-window" or "spawn foot".
    /// Built-in action strings. Kept deliberately small: everything else is a
    /// Lua function (e.g. the default WM's `wm.switch(n)`).
    pub fn parse(s: &str) -> Result<Self> {
        if let Some(cmd) = s.strip_prefix("spawn ") {
            return Ok(Action::Spawn(cmd.trim().to_string()));
        }
        Ok(match s {
            "quit" => Action::Quit,
            "quit!" => Action::ConfirmQuit,
            "close-window" => Action::CloseWindow,
            "show-hotkey-overlay" => Action::ShowHotkeyOverlay,
            "reload-config" => Action::ReloadConfig,
            _ => bail!("unknown action {s:?} (define a Lua function instead)"),
        })
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Mods {
    pub logo: bool,
    pub shift: bool,
    pub ctrl: bool,
    pub alt: bool,
}

impl Mods {
    fn matches(&self, state: &ModifiersState) -> bool {
        self.logo == state.logo
            && self.shift == state.shift
            && self.ctrl == state.ctrl
            && self.alt == state.alt
    }
}

pub struct Bind {
    /// The combo as written in the config, for the hotkey overlay.
    pub combo: String,
    pub mods: Mods,
    pub keysym: Keysym,
    pub action: Action,
    /// Overlay label (third argument to `takhti.bind`).
    pub desc: Option<String>,
}

/// Parse "Super+Shift+q" style combos.
pub fn parse_combo(combo: &str) -> Result<(Mods, Keysym)> {
    let parts: Vec<&str> = combo.split('+').map(str::trim).collect();
    let (key, mod_parts) = parts.split_last().unwrap();
    let mut mods = Mods::default();
    for part in mod_parts {
        match part.to_ascii_lowercase().as_str() {
            "super" | "logo" | "win" | "mod" => mods.logo = true,
            "shift" => mods.shift = true,
            "ctrl" | "control" => mods.ctrl = true,
            "alt" => mods.alt = true,
            other => bail!("unknown modifier {other:?}"),
        }
    }
    let mut keysym = keysym_from_name(key, KEYSYM_CASE_INSENSITIVE);
    if keysym.raw() == 0 {
        keysym = keysym_from_name(key, KEYSYM_NO_FLAGS);
    }
    if keysym.raw() == 0 {
        bail!("unknown key {key:?}");
    }
    Ok((mods, keysym))
}

impl Takhti {
    pub fn process_input_event<I: InputBackend>(&mut self, event: InputEvent<I>) {
        match event {
            InputEvent::Keyboard { event } => {
                let serial = SERIAL_COUNTER.next_serial();
                let time = event.time_msec();
                let key_code = event.key_code();
                let key_state = event.state();
                let Some(keyboard) = self.seat.get_keyboard() else {
                    return;
                };
                let pressed = key_state == KeyState::Pressed;
                // Transient UI is dismissed by any key press, but only if it
                // was already up before this event, so the opening bind
                // itself doesn't immediately close it (and re-pressing the
                // bind toggles it away).
                let exit_dialog_was_open = self.ui.exit_dialog.is_open();
                let hotkey_overlay_was_open = self.ui.hotkey_overlay.is_open();
                // Intercept(None) swallows a key without dispatching an action.
                let action = keyboard.input::<Option<Action>, _>(
                    self,
                    key_code,
                    key_state,
                    serial,
                    time,
                    |takhti, mods, handle| {
                        let raw_syms = handle.raw_syms();
                        if pressed {
                            // VT switching always works, even over the dialog.
                            for sym in raw_syms.iter() {
                                let raw = sym.raw();
                                if (keysyms::KEY_XF86Switch_VT_1..=keysyms::KEY_XF86Switch_VT_12)
                                    .contains(&raw)
                                {
                                    let vt = (raw - keysyms::KEY_XF86Switch_VT_1 + 1) as i32;
                                    return FilterResult::Intercept(Some(Action::ChangeVt(vt)));
                                }
                            }
                        }
                        // The exit dialog is modal: while open, no key
                        // reaches clients. Enter confirms, the rest dismiss.
                        if takhti.ui.exit_dialog.is_open() {
                            if pressed
                                && (raw_syms.contains(&Keysym::Return)
                                    || raw_syms.contains(&Keysym::KP_Enter))
                            {
                                return FilterResult::Intercept(Some(Action::ConfirmQuit));
                            }
                            return FilterResult::Intercept(None);
                        }
                        if pressed {
                            for bind in &takhti.binds {
                                if bind.mods.matches(mods) && raw_syms.contains(&bind.keysym) {
                                    return FilterResult::Intercept(Some(bind.action.clone()));
                                }
                            }
                        }
                        FilterResult::Forward
                    },
                );
                if let Some(Some(action)) = action {
                    self.do_action(action);
                }
                if pressed && exit_dialog_was_open && self.ui.exit_dialog.is_open() {
                    self.ui.exit_dialog.hide();
                    self.queue_redraw_all();
                }
                if pressed && hotkey_overlay_was_open && self.ui.hotkey_overlay.is_open() {
                    self.ui.hotkey_overlay.hide();
                    self.queue_redraw_all();
                }
            }
            InputEvent::PointerMotion { event } => {
                let Some(pointer) = self.seat.get_pointer() else {
                    return;
                };
                let scale = self.space.scale();
                // Deltas are logical-paced (a swipe crosses the same fraction
                // of the screen at any scale); position math is physical.
                let mut pos = crate::coords::point_to_physical(
                    pointer.current_location() + event.delta(),
                    scale,
                );
                let mut max_x = 0.0f64;
                let mut max_y = 0.0f64;
                for output in self.space.outputs() {
                    if let Some(geo) = self.space.output_geometry(output) {
                        max_x = max_x.max((geo.loc.x + geo.size.w) as f64);
                        max_y = max_y.max((geo.loc.y + geo.size.h) as f64);
                    }
                }
                pos.x = pos.x.clamp(0.0, (max_x - 1.0).max(0.0));
                pos.y = pos.y.clamp(0.0, (max_y - 1.0).max(0.0));
                let serial = SERIAL_COUNTER.next_serial();
                let under = self.surface_under(pos);
                pointer.motion(
                    self,
                    under,
                    &MotionEvent {
                        location: crate::coords::point_to_protocol(pos, scale),
                        serial,
                        time: event.time_msec(),
                    },
                );
                pointer.frame(self);
                // The cursor is composited, so moving it damages the output.
                self.queue_redraw_all();
            }
            InputEvent::PointerMotionAbsolute { event } => {
                let Some(output) = self.space.outputs().next().cloned() else {
                    return;
                };
                let Some(output_geo) = self.space.output_geometry(&output) else {
                    return;
                };
                let scale = self.space.scale();
                // Absolute events are normalized device coordinates; mapping
                // them over the *physical* rect lands on exact pixels (the
                // Logical type on position_transformed is smithay's, not a
                // statement about our space).
                let size = smithay::utils::Size::from((output_geo.size.w, output_geo.size.h));
                let norm = event.position_transformed(size);
                let pos: smithay::utils::Point<f64, smithay::utils::Physical> =
                    smithay::utils::Point::from((norm.x, norm.y)) + output_geo.loc.to_f64();
                let serial = SERIAL_COUNTER.next_serial();
                let under = self.surface_under(pos);
                let Some(pointer) = self.seat.get_pointer() else {
                    return;
                };
                pointer.motion(
                    self,
                    under,
                    &MotionEvent {
                        location: crate::coords::point_to_protocol(pos, scale),
                        serial,
                        time: event.time_msec(),
                    },
                );
                pointer.frame(self);
                self.queue_redraw_all();
            }
            InputEvent::PointerButton { event } => {
                let serial = SERIAL_COUNTER.next_serial();
                let Some(pointer) = self.seat.get_pointer() else {
                    return;
                };
                if event.state() == ButtonState::Pressed {
                    // Clicks dismiss transient UI, like any key press.
                    let mut ui_dismissed = false;
                    if self.ui.exit_dialog.is_open() {
                        self.ui.exit_dialog.hide();
                        ui_dismissed = true;
                    }
                    if self.ui.hotkey_overlay.is_open() {
                        self.ui.hotkey_overlay.hide();
                        ui_dismissed = true;
                    }
                    if ui_dismissed {
                        self.queue_redraw_all();
                    }
                }
                if event.state() == ButtonState::Pressed && !pointer.is_grabbed() {
                    let pos = crate::coords::point_to_physical(
                        pointer.current_location(),
                        self.space.scale(),
                    );
                    let under = self.space.element_under(pos).map(|(w, _)| w.clone());
                    self.focus_window(under.as_ref());
                }
                pointer.button(
                    self,
                    &ButtonEvent {
                        button: event.button_code(),
                        state: event.state(),
                        serial,
                        time: event.time_msec(),
                    },
                );
                pointer.frame(self);
            }
            InputEvent::PointerAxis { event } => {
                let horizontal = event
                    .amount(Axis::Horizontal)
                    .unwrap_or_else(|| event.amount_v120(Axis::Horizontal).unwrap_or(0.0) / 120.0 * 15.0);
                let vertical = event
                    .amount(Axis::Vertical)
                    .unwrap_or_else(|| event.amount_v120(Axis::Vertical).unwrap_or(0.0) / 120.0 * 15.0);
                let mut frame = AxisFrame::new(event.time_msec()).source(event.source());
                if horizontal != 0.0 {
                    frame = frame.value(Axis::Horizontal, horizontal);
                }
                if vertical != 0.0 {
                    frame = frame.value(Axis::Vertical, vertical);
                }
                if event.source() == AxisSource::Finger {
                    if event.amount(Axis::Horizontal) == Some(0.0) {
                        frame = frame.stop(Axis::Horizontal);
                    }
                    if event.amount(Axis::Vertical) == Some(0.0) {
                        frame = frame.stop(Axis::Vertical);
                    }
                }
                let Some(pointer) = self.seat.get_pointer() else {
                    return;
                };
                pointer.axis(self, frame);
                pointer.frame(self);
            }
            _ => {}
        }
    }
}
