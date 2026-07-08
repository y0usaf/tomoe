//! Retained-widget registry: the `tomoe.ui` surface (doctrine 05 — one
//! declaration mechanism for compositor-drawn UI). Lua (or the core, for
//! builtins) declares a widget once; the core renders, damages, and routes
//! input to it, and only selection events re-enter Lua. The exit dialog,
//! hotkey overlay, and config-error banner are builtins on this same
//! registry; the screenshot overlay stays native (declared exemption) until
//! the API grows drag-region interaction.

use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use smithay::backend::renderer::element::memory::{
    MemoryRenderBuffer, MemoryRenderBufferRenderElement,
};
use smithay::backend::renderer::element::solid::{SolidColorBuffer, SolidColorRenderElement};
use smithay::backend::renderer::element::Kind;
use smithay::utils::{Physical, Size};

use super::text::{Canvas, Fonts, Span};
use super::{ACCENT, BACKDROP, BG, FG, KEY_CHIP, RED};
use crate::input::{Action, Bind};
use crate::render::{OutputRenderElements, TomoeRenderer};

const PADDING: i32 = 28;
const BORDER: i32 = 4;
const TITLE_SIZE: f32 = 24.0;
const ROW_SIZE: f32 = 17.0;
const ROW_GAP: i32 = 8;
const COLUMN_GAP: i32 = 20;
const CONFIRM_PADDING: i32 = 32;
const CONFIRM_BORDER: i32 = 8;
const CONFIRM_SIZE: f32 = 20.0;
const MENU_SIZE: f32 = 18.0;
const MENU_ROW_PAD: i32 = 5;
const TOAST_PADDING: i32 = 16;
const TOAST_SIZE: f32 = 17.0;
const TOAST_MARGIN_TOP: i32 = 24;
const TOAST_GAP: i32 = 8;

/// Widget ids are unique for the compositor session (not per Lua VM):
/// builtin widgets outlive config reloads, so a fresh VM must never mint an
/// id that collides with a surviving widget's.
pub fn alloc_id() -> u64 {
    static NEXT: AtomicU64 = AtomicU64::new(1);
    NEXT.fetch_add(1, Ordering::Relaxed)
}

/// What a widget does when an event fires on it.
pub enum WidgetHandler {
    /// Purely informational (toasts, sheets).
    None,
    /// Dispatch a core action on confirm/select — how builtins hook in
    /// (e.g. the exit dialog confirms into `Action::ConfirmQuit`).
    Action(Action),
    /// Config-owned: events re-enter Lua. The callbacks live in the VM,
    /// keyed by widget id (`Shared.ui_callbacks`), never in core state.
    Lua,
}

/// Identities for compositor-owned widgets, so the core can toggle/replace
/// its own without tracking ids.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Tag {
    ExitDialog,
    HotkeyOverlay,
    ConfigError,
}

/// Widget content. Confirm and Menu are modal (they own the keyboard and
/// swallow clicks); Sheet is dismissed by any input; Toast expires on its
/// own and ignores input entirely.
pub enum WidgetKind {
    /// Enter confirms, any other key or click cancels.
    Confirm { text: String },
    /// Up/Down (or k/j) navigate, Enter selects, Esc or a click cancels.
    Menu {
        title: Option<String>,
        items: Vec<String>,
        selected: usize,
    },
    /// Transient notification, auto-hides at `deadline`.
    Toast {
        text: String,
        deadline: Instant,
        urgent: bool,
    },
    /// Rows of (key chip, label) — the hotkey-overlay shape.
    Sheet {
        title: Option<String>,
        rows: Vec<(String, String)>,
    },
}

impl WidgetKind {
    pub fn modal(&self) -> bool {
        matches!(self, WidgetKind::Confirm { .. } | WidgetKind::Menu { .. })
    }

    /// Dismissed by any key press or click (that wasn't the one opening it).
    pub fn dismissable(&self) -> bool {
        matches!(self, WidgetKind::Sheet { .. })
    }
}

/// A widget declaration crossing the Lua boundary (actions out): durations
/// instead of deadlines, no core-side state.
#[derive(Debug, Clone, PartialEq)]
pub enum WidgetSpec {
    Confirm {
        text: String,
    },
    Menu {
        title: Option<String>,
        items: Vec<String>,
    },
    Toast {
        text: String,
        duration: Duration,
        urgent: bool,
    },
    Sheet {
        title: Option<String>,
        rows: Vec<(String, String)>,
    },
}

/// An input event a widget fired. `Select` is 0-based (Lua callbacks get it
/// 1-based).
#[derive(Debug, Clone, Copy)]
pub enum UiEvent {
    Confirm,
    Cancel,
    Select(usize),
}

pub struct WidgetEntry {
    pub id: u64,
    pub kind: WidgetKind,
    pub handler: WidgetHandler,
    pub tag: Option<Tag>,
    /// Cached rendering; dropped when content changes (menu navigation).
    buffer: Option<(MemoryRenderBuffer, Size<i32, Physical>)>,
}

impl WidgetEntry {
    pub fn new(id: u64, kind: WidgetKind, handler: WidgetHandler, tag: Option<Tag>) -> Self {
        Self {
            id,
            kind,
            handler,
            tag,
            buffer: None,
        }
    }

    /// Drop the cached rendering (content changed).
    pub fn invalidate(&mut self) {
        self.buffer = None;
    }
}

/// The registry. Insertion order is stacking order: the last entry renders
/// topmost, and the topmost modal entry owns the keyboard.
#[derive(Default)]
pub struct Widgets {
    entries: Vec<WidgetEntry>,
    backdrop: SolidColorBuffer,
}

impl Widgets {
    pub fn open(&mut self, entry: WidgetEntry) {
        self.entries.push(entry);
    }

    pub fn close(&mut self, id: u64) -> Option<WidgetEntry> {
        let i = self.entries.iter().position(|e| e.id == id)?;
        Some(self.entries.remove(i))
    }

    pub fn close_tag(&mut self, tag: Tag) -> bool {
        let Some(i) = self.entries.iter().position(|e| e.tag == Some(tag)) else {
            return false;
        };
        self.entries.remove(i);
        true
    }

    pub fn tag_open(&self, tag: Tag) -> bool {
        self.entries.iter().any(|e| e.tag == Some(tag))
    }

    pub fn top_modal_mut(&mut self) -> Option<&mut WidgetEntry> {
        self.entries.iter_mut().rev().find(|e| e.kind.modal())
    }

    pub fn top_modal_id(&self) -> Option<u64> {
        self.entries
            .iter()
            .rev()
            .find(|e| e.kind.modal())
            .map(|e| e.id)
    }

    pub fn dismissable_ids(&self) -> Vec<u64> {
        self.entries
            .iter()
            .filter(|e| e.kind.dismissable())
            .map(|e| e.id)
            .collect()
    }

    /// Close everything (session lock). Callers drop the Lua callbacks of
    /// the returned entries.
    pub fn drain(&mut self) -> Vec<WidgetEntry> {
        std::mem::take(&mut self.entries)
    }

    /// Close every Lua-owned widget: their callbacks died with the VM
    /// (config reload). Builtins (Action/None handlers) survive.
    pub fn close_lua(&mut self) {
        self.entries
            .retain(|e| !matches!(e.handler, WidgetHandler::Lua));
    }

    /// Build render elements, topmost first (earlier elements draw on top).
    /// Expired toasts are culled here — they carry no callbacks, so
    /// render-time removal needs no Lua cleanup; a timer scheduled at open
    /// queues the repaint that reaches this cull.
    pub fn render_elements<R: TomoeRenderer>(
        &mut self,
        fonts: &Fonts,
        renderer: &mut R,
        output_size: Size<i32, Physical>,
        elements: &mut Vec<OutputRenderElements<R>>,
    ) {
        let now = Instant::now();
        self.entries.retain(|e| match &e.kind {
            WidgetKind::Toast { deadline, .. } => *deadline > now,
            _ => true,
        });
        let top_modal = self.entries.iter().rposition(|e| e.kind.modal());
        let mut toast_y = TOAST_MARGIN_TOP;
        for i in (0..self.entries.len()).rev() {
            let entry = &mut self.entries[i];
            if entry.buffer.is_none() {
                entry.buffer = Some(render_widget(fonts, &entry.kind));
            }
            let Some((buffer, size)) = &entry.buffer else {
                continue;
            };
            let loc = match entry.kind {
                WidgetKind::Toast { .. } => {
                    let loc = (((output_size.w - size.w).max(0) / 2) as f64, toast_y as f64);
                    toast_y += size.h + TOAST_GAP;
                    loc
                }
                _ => (
                    ((output_size.w - size.w).max(0) / 2) as f64,
                    ((output_size.h - size.h).max(0) / 2) as f64,
                ),
            };
            let alpha = matches!(entry.kind, WidgetKind::Sheet { .. }).then_some(0.95);
            if let Ok(element) = MemoryRenderBufferRenderElement::from_buffer(
                renderer,
                loc,
                buffer,
                alpha,
                None,
                None,
                Kind::Unspecified,
            ) {
                elements.push(OutputRenderElements::Memory(element));
            }
            // Dim everything beneath the topmost modal widget.
            if Some(i) == top_modal {
                self.backdrop
                    .update((output_size.w, output_size.h), BACKDROP);
                elements.push(OutputRenderElements::Solid(
                    SolidColorRenderElement::from_buffer(
                        &self.backdrop,
                        (0, 0),
                        1.0,
                        1.0,
                        Kind::Unspecified,
                    ),
                ));
            }
        }
    }
}

fn render_widget(fonts: &Fonts, kind: &WidgetKind) -> (MemoryRenderBuffer, Size<i32, Physical>) {
    match kind {
        WidgetKind::Confirm { text } => render_confirm(fonts, text),
        WidgetKind::Menu {
            title,
            items,
            selected,
        } => render_menu(fonts, title.as_deref(), items, *selected),
        WidgetKind::Toast { text, urgent, .. } => render_toast(fonts, text, *urgent),
        WidgetKind::Sheet { title, rows } => render_sheet(fonts, title.as_deref(), rows),
    }
}

fn render_confirm(fonts: &Fonts, text: &str) -> (MemoryRenderBuffer, Size<i32, Physical>) {
    let line1 = [Span::sans(text, CONFIRM_SIZE, FG)];
    let line2 = [
        Span::sans("Press ", CONFIRM_SIZE, FG),
        Span::key(" Enter ", CONFIRM_SIZE, FG, KEY_CHIP),
        Span::sans(" to confirm.", CONFIRM_SIZE, FG),
    ];
    let (w1, h1) = fonts.measure(&line1);
    let (w2, h2) = fonts.measure(&line2);
    let gap = (CONFIRM_SIZE * 0.6) as i32;

    let width = w1.max(w2) + 2 * (CONFIRM_PADDING + CONFIRM_BORDER);
    let height = h1 + gap + h2 + 2 * (CONFIRM_PADDING + CONFIRM_BORDER);
    let mut canvas = Canvas::new(width, height);
    canvas.fill(BG);
    canvas.border(CONFIRM_BORDER, RED);
    canvas.draw_spans(
        fonts,
        (width - w1) / 2,
        CONFIRM_BORDER + CONFIRM_PADDING,
        &line1,
    );
    canvas.draw_spans(
        fonts,
        (width - w2) / 2,
        CONFIRM_BORDER + CONFIRM_PADDING + h1 + gap,
        &line2,
    );
    canvas.into_buffer()
}

fn render_menu(
    fonts: &Fonts,
    title: Option<&str>,
    items: &[String],
    selected: usize,
) -> (MemoryRenderBuffer, Size<i32, Physical>) {
    let (title_w, title_h) = title
        .map(|t| fonts.measure(&[Span::sans(t, TITLE_SIZE, FG)]))
        .unwrap_or((0, 0));
    let title_gap = if title.is_some() {
        (TITLE_SIZE * 0.8) as i32
    } else {
        0
    };

    let mut item_w = 0;
    let mut text_h = 0;
    for item in items {
        let (w, h) = fonts.measure(&[Span::sans(item, MENU_SIZE, FG)]);
        item_w = item_w.max(w);
        text_h = text_h.max(h);
    }
    let row_h = text_h + 2 * MENU_ROW_PAD;

    let content_w = title_w.max(item_w);
    let content_h = title_h + title_gap + items.len() as i32 * (row_h + ROW_GAP)
        - if items.is_empty() { 0 } else { ROW_GAP };
    let width = content_w + 2 * (PADDING + BORDER);
    let height = content_h + 2 * (PADDING + BORDER);

    let mut canvas = Canvas::new(width, height);
    canvas.fill(BG);
    canvas.border(BORDER, ACCENT);

    let mut y = BORDER + PADDING;
    if let Some(title) = title {
        canvas.draw_spans(
            fonts,
            (width - title_w) / 2,
            y,
            &[Span::sans(title, TITLE_SIZE, FG)],
        );
        y += title_h + title_gap;
    }
    for (i, item) in items.iter().enumerate() {
        // Highlight bar across the full inner width; dark text on accent.
        let color = if i == selected {
            canvas.fill_rect(BORDER, y, width - 2 * BORDER, row_h, ACCENT);
            BG
        } else {
            FG
        };
        canvas.draw_spans(
            fonts,
            BORDER + PADDING,
            y + MENU_ROW_PAD,
            &[Span::sans(item, MENU_SIZE, color)],
        );
        y += row_h + ROW_GAP;
    }
    canvas.into_buffer()
}

fn render_toast(
    fonts: &Fonts,
    text: &str,
    urgent: bool,
) -> (MemoryRenderBuffer, Size<i32, Physical>) {
    let spans = [Span::sans(text, TOAST_SIZE, FG)];
    let (w, h) = fonts.measure(&spans);
    let width = w + 2 * (TOAST_PADDING + BORDER);
    let height = h + 2 * (TOAST_PADDING + BORDER);
    let mut canvas = Canvas::new(width, height);
    canvas.fill(BG);
    canvas.border(BORDER, if urgent { RED } else { ACCENT });
    canvas.draw_spans(
        fonts,
        BORDER + TOAST_PADDING,
        BORDER + TOAST_PADDING,
        &spans,
    );
    canvas.into_buffer()
}

fn render_sheet(
    fonts: &Fonts,
    title: Option<&str>,
    rows: &[(String, String)],
) -> (MemoryRenderBuffer, Size<i32, Physical>) {
    let (title_w, title_h) = title
        .map(|t| fonts.measure(&[Span::sans(t, TITLE_SIZE, FG)]))
        .unwrap_or((0, 0));
    let title_gap = if title.is_some() {
        (TITLE_SIZE * 0.8) as i32
    } else {
        0
    };

    let mut key_w = 0;
    let mut label_w = 0;
    let mut row_h = 0;
    for (key, label) in rows {
        let (kw, kh) = fonts.measure(&[Span::key(key, ROW_SIZE, FG, KEY_CHIP)]);
        let (lw, lh) = fonts.measure(&[Span::sans(label, ROW_SIZE, FG)]);
        key_w = key_w.max(kw);
        label_w = label_w.max(lw);
        row_h = row_h.max(kh.max(lh));
    }

    let content_w = title_w.max(key_w + COLUMN_GAP + label_w);
    let content_h = title_h + title_gap + rows.len() as i32 * (row_h + ROW_GAP)
        - if rows.is_empty() { 0 } else { ROW_GAP };
    let width = content_w + 2 * (PADDING + BORDER);
    let height = content_h + 2 * (PADDING + BORDER);

    let mut canvas = Canvas::new(width, height);
    canvas.fill(BG);
    canvas.border(BORDER, ACCENT);

    let x0 = BORDER + PADDING;
    let mut y = BORDER + PADDING;
    if let Some(title) = title {
        canvas.draw_spans(
            fonts,
            (width - title_w) / 2,
            y,
            &[Span::sans(title, TITLE_SIZE, FG)],
        );
        y += title_h + title_gap;
    }
    for (key, label) in rows {
        let (kw, _) = fonts.measure(&[Span::key(key, ROW_SIZE, FG, KEY_CHIP)]);
        // Right-align keys against the column edge.
        canvas.draw_spans(
            fonts,
            x0 + key_w - kw,
            y,
            &[Span::key(key, ROW_SIZE, FG, KEY_CHIP)],
        );
        canvas.draw_spans(
            fonts,
            x0 + key_w + COLUMN_GAP,
            y,
            &[Span::sans(label, ROW_SIZE, FG)],
        );
        y += row_h + ROW_GAP;
    }
    canvas.into_buffer()
}

// ─── Hotkey-overlay content (builtin sheet) ──────────────────────────────────

/// Build the hotkey-overlay rows from the live bind list.
pub fn hotkey_rows(binds: &[Bind]) -> Vec<(String, String)> {
    binds
        .iter()
        .filter_map(|bind| {
            Some((
                format!(" {} ", pretty_combo(&bind.combo)),
                bind_title(bind)?,
            ))
        })
        .collect()
}

/// Human-readable label for a bind's row, or None to omit it from the overlay.
fn bind_title(bind: &Bind) -> Option<String> {
    if let Some(desc) = &bind.desc {
        return Some(desc.clone());
    }
    match &bind.action {
        Action::Quit => Some("Exit".to_string()),
        Action::ConfirmQuit => Some("Exit Immediately".to_string()),
        Action::CloseWindow => Some("Close Window".to_string()),
        Action::Spawn(cmd) => Some(format!("Spawn {cmd}")),
        Action::ShowHotkeyOverlay => Some("Show Important Hotkeys".to_string()),
        Action::ReloadConfig => Some("Reload the Config File".to_string()),
        Action::Screenshot => Some("Take a Screenshot".to_string()),
        Action::ScreenshotScreen => Some("Screenshot the Screen".to_string()),
        // Lua functions are opaque; configs label them via the third
        // `tomoe.bind` argument.
        Action::LuaFn(_)
        | Action::ChangeVt(_)
        | Action::ScreenshotConfirm
        | Action::UiEvent(..) => None,
    }
}

/// "Super+Shift+slash" -> "Super + Shift + /".
fn pretty_combo(combo: &str) -> String {
    combo
        .split('+')
        .map(|part| pretty_key(part.trim()))
        .collect::<Vec<_>>()
        .join(" + ")
}

fn pretty_key(key: &str) -> String {
    match key.to_ascii_lowercase().as_str() {
        "return" => "Enter".to_string(),
        "slash" => "/".to_string(),
        "comma" => ",".to_string(),
        "period" => ".".to_string(),
        "space" => "Space".to_string(),
        "minus" => "-".to_string(),
        "equal" => "=".to_string(),
        _ => {
            let mut chars = key.chars();
            match chars.next() {
                Some(first) => first.to_uppercase().collect::<String>() + chars.as_str(),
                None => String::new(),
            }
        }
    }
}
