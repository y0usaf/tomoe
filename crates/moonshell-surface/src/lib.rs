//! SCTK layer-surface mechanism: registry/seat/output binds, N anchored
//! wlr-layer-shell surfaces with per-window options, double-buffered
//! wl_shm via one shared `SlotPool`, precise `damage_buffer`, and frame
//! callbacks requested only while a commit is in flight — a fully idle
//! shell schedules zero wakeups.
//!
//! This crate is mechanism only: it owns the Wayland plumbing and hands
//! each window's [`Painter`] a raw ARGB8888 canvas. What gets drawn is
//! the caller's policy. The public vocabulary ([`Layer`], [`Anchors`],
//! [`Keyboard`], [`LayerOptions`]) is this crate's own — callers never
//! see SCTK types.
//!
//! Lifecycle: [`Shell::connect`] binds globals and returns the shell
//! plus its calloop event loop (callers insert their own sources —
//! timers, inotify — with `&mut Shell` as the dispatch data);
//! [`Shell::create_window`]/[`Shell::destroy_window`] work at any point,
//! including from inside source callbacks; [`Shell::run`] drives
//! dispatch until [`Shell::quit`] or the compositor drops the
//! connection.
//!
//! Coordinate doctrine (tomoe-inherited): the compositor speaks logical
//! pixels (configure sizes), painters speak integer physical (buffer)
//! pixels. Conversion happens in exactly one place, `Window::buffer_size`,
//! using the integer buffer scale.

use std::collections::BTreeMap;

use smithay_client_toolkit::{
    compositor::{CompositorHandler, CompositorState},
    delegate_compositor, delegate_layer, delegate_output, delegate_registry, delegate_seat,
    delegate_shm,
    output::{OutputHandler, OutputState},
    registry::{ProvidesRegistryState, RegistryState},
    registry_handlers,
    seat::{Capability, SeatHandler, SeatState},
    shell::{
        wlr_layer::{
            Anchor, KeyboardInteractivity, LayerShell, LayerShellHandler, LayerSurface,
            LayerSurfaceConfigure,
        },
        WaylandSurface,
    },
    shm::{
        slot::{ActivateSlotError, CreateBufferError, SlotPool},
        CreatePoolError, Shm, ShmHandler,
    },
};
use wayland_client::{
    globals::{registry_queue_init, BindError, GlobalError},
    protocol::{wl_output, wl_seat, wl_shm, wl_surface},
    ConnectError, Connection, DispatchError, QueueHandle,
};

/// Errors from the surface layer. One typed error per layer; the binary
/// converts to `anyhow` at its boundary.
#[derive(Debug, thiserror::Error)]
pub enum SurfaceError {
    #[error("wayland connection: {0}")]
    Connect(#[from] ConnectError),
    #[error("wayland registry: {0}")]
    Global(#[from] GlobalError),
    #[error("required global missing: {0}")]
    Bind(#[from] BindError),
    #[error("shm pool: {0}")]
    CreatePool(#[from] CreatePoolError),
    #[error("shm buffer: {0}")]
    CreateBuffer(#[from] CreateBufferError),
    #[error("shm buffer attach: {0}")]
    AttachBuffer(#[from] ActivateSlotError),
    #[error("wayland dispatch: {0}")]
    Dispatch(#[from] DispatchError),
    #[error("event loop: {0}")]
    EventLoop(#[from] calloop::Error),
    #[error("event loop source: {0}")]
    LoopSource(String),
}

/// A damaged region in buffer (physical) pixels.
#[derive(Clone, Copy, Debug)]
pub struct DamageRect {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

/// What a painter changed this pass. `None` skips the commit entirely.
#[derive(Debug)]
pub enum Damage {
    None,
    Full,
    Rects(Vec<DamageRect>),
}

/// The canvas handed to painters: ARGB8888 little-endian bytes, tightly
/// packed rows, buffer-pixel dimensions.
pub struct Canvas<'a> {
    pub buf: &'a mut [u8],
    pub width: u32,
    pub height: u32,
    /// Integer buffer scale (physical = logical * scale).
    pub scale: i32,
    /// True when the surface has no previously committed content at
    /// this buffer size (first draw, remap after close, resize). The
    /// painter must repaint fully — frame-diff caches must be
    /// invalidated — and any reported damage is upgraded to `Full`.
    pub fresh: bool,
}

/// A window's drawing policy. Painters must leave every pixel outside
/// the returned damage identical to the previous paint — buffers
/// alternate, and the compositor trusts the damage report.
pub trait Painter {
    fn paint(&mut self, canvas: Canvas<'_>) -> Damage;
}

/// Which edge a bar-style surface docks to (the [`LayerOptions::bar`]
/// convenience; arbitrary anchoring goes through [`Anchors`]).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Edge {
    #[default]
    Top,
    Bottom,
}

/// wlr-layer-shell stacking layer, in this crate's own vocabulary.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Layer {
    Background,
    Bottom,
    #[default]
    Top,
    Overlay,
}

/// Which screen edges the surface sticks to. Anchoring both ends of an
/// axis (with size 0 on that axis) stretches the surface along it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Anchors {
    pub top: bool,
    pub bottom: bool,
    pub left: bool,
    pub right: bool,
}

/// Keyboard interactivity for the surface (input routing lands in M4;
/// the option is plumbed now so window shapes are stable).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Keyboard {
    #[default]
    None,
    OnDemand,
    Exclusive,
}

/// Margins between the surface and its anchored edges, logical px.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Margins {
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
    pub left: i32,
}

/// Per-window options.
#[derive(Clone, Debug)]
pub struct LayerOptions {
    pub namespace: String,
    pub layer: Layer,
    pub anchors: Anchors,
    /// Requested logical size. 0 on a fully-anchored axis = stretch;
    /// the configure event supplies the real size.
    pub width: u32,
    pub height: u32,
    /// Screen space to reserve: 0 = none, >0 = that many logical px,
    /// -1 = ignore other surfaces' zones.
    pub exclusive_zone: i32,
    pub margins: Margins,
    pub keyboard: Keyboard,
}

impl Default for LayerOptions {
    fn default() -> Self {
        Self {
            namespace: "moonshell".into(),
            layer: Layer::Top,
            anchors: Anchors::default(),
            width: 0,
            height: 0,
            exclusive_zone: 0,
            margins: Margins::default(),
            keyboard: Keyboard::default(),
        }
    }
}

impl LayerOptions {
    /// A full-width bar docked to `edge`: anchored left+right (width
    /// follows the output), `height` logical px tall, optionally
    /// reserving its height as an exclusive zone.
    pub fn bar(edge: Edge, height: u32, exclusive: bool) -> Self {
        Self {
            anchors: Anchors {
                top: edge == Edge::Top,
                bottom: edge == Edge::Bottom,
                left: true,
                right: true,
            },
            height,
            exclusive_zone: if exclusive { height as i32 } else { 0 },
            ..Self::default()
        }
    }
}

/// Opaque handle to a shell window. Stable for the window's lifetime;
/// never reused within a `Shell`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct WindowId(u64);

struct Window {
    options: LayerOptions,
    painter: Box<dyn Painter>,
    /// `None` while unmapped (before the first output, or between a
    /// compositor close and the remap).
    layer: Option<LayerSurface>,
    /// The current surface got at least one configure — gates instant
    /// remap on close, so a compositor that closes us before
    /// configuring (no outputs) can't drive a create/close storm.
    was_configured: bool,
    /// Logical size from the latest configure.
    logical_size: (u32, u32),
    /// Integer buffer scale.
    scale: i32,
    configured: bool,
    /// Buffer size of the last committed frame on the current surface;
    /// `None` until one lands. A mismatch means the painter starts
    /// `fresh` (no prior content at this size to diff against).
    committed_size: Option<(u32, u32)>,
    /// Content changed; a draw is owed.
    dirty: bool,
    /// A frame callback is outstanding; hold further draws until it fires.
    frame_pending: bool,
    /// At least one frame ever committed (the boot-check gate).
    drawn_once: bool,
}

impl Window {
    fn new(options: LayerOptions, painter: Box<dyn Painter>) -> Self {
        Self {
            options,
            painter,
            layer: None,
            was_configured: false,
            logical_size: (0, 0),
            scale: 1,
            configured: false,
            committed_size: None,
            dirty: false,
            frame_pending: false,
            drawn_once: false,
        }
    }

    /// The one logical→physical conversion point.
    fn buffer_size(&self) -> (u32, u32) {
        let s = self.scale.max(1) as u32;
        (self.logical_size.0 * s, self.logical_size.1 * s)
    }

    fn holds(&self, surface: &wl_surface::WlSurface) -> bool {
        self.layer
            .as_ref()
            .is_some_and(|l| l.wl_surface() == surface)
    }
}

/// The shell: Wayland connection, globals, and every live window. This
/// is the calloop dispatch data — sources the caller inserts receive
/// `&mut Shell` and may create/destroy windows or mark them dirty.
pub struct Shell {
    conn: Connection,
    qh: QueueHandle<Shell>,
    registry_state: RegistryState,
    seat_state: SeatState,
    output_state: OutputState,
    compositor: CompositorState,
    layer_shell: LayerShell,
    shm: Shm,
    /// One pool for all windows; slots are recycled per buffer size —
    /// buffers are reused, never regenerated per frame.
    pool: SlotPool,
    windows: BTreeMap<WindowId, Window>,
    next_id: u64,
    /// Exit `run` once every window has committed its first frame —
    /// the doctrine-06 boot check.
    pub exit_after_first_draw: bool,
    exit: bool,
}

impl Shell {
    /// Connect to the compositor, bind globals, and prepare the event
    /// loop (the Wayland source is already inserted). No window exists
    /// yet — call [`Shell::create_window`].
    pub fn connect() -> Result<(Self, calloop::EventLoop<'static, Shell>), SurfaceError> {
        let conn = Connection::connect_to_env()?;
        let (globals, event_queue) = registry_queue_init::<Shell>(&conn)?;
        let qh: QueueHandle<Shell> = event_queue.handle();

        let compositor = CompositorState::bind(&globals, &qh)?;
        let layer_shell = LayerShell::bind(&globals, &qh)?;
        let shm = Shm::bind(&globals, &qh)?;
        // Grows on demand; sized for nothing in particular.
        let pool = SlotPool::new(4096, &shm)?;

        let event_loop: calloop::EventLoop<Shell> = calloop::EventLoop::try_new()?;
        calloop_wayland_source::WaylandSource::new(conn.clone(), event_queue)
            .insert(event_loop.handle())
            .map_err(|e| SurfaceError::LoopSource(e.to_string()))?;

        Ok((
            Self {
                conn,
                qh: qh.clone(),
                registry_state: RegistryState::new(&globals),
                seat_state: SeatState::new(&globals, &qh),
                output_state: OutputState::new(&globals, &qh),
                compositor,
                layer_shell,
                shm,
                pool,
                windows: BTreeMap::new(),
                next_id: 0,
                exit_after_first_draw: false,
                exit: false,
            },
            event_loop,
        ))
    }

    /// Create and map a window. Usable before `run` and from inside
    /// source callbacks alike.
    pub fn create_window(&mut self, options: LayerOptions, painter: Box<dyn Painter>) -> WindowId {
        let id = WindowId(self.next_id);
        self.next_id += 1;
        self.windows.insert(id, Window::new(options, painter));
        self.map_window(id);
        id
    }

    /// Destroy a window (unmaps immediately). Returns false if the id
    /// is already gone.
    pub fn destroy_window(&mut self, id: WindowId) -> bool {
        // Dropping the LayerSurface destroys the role object and the
        // wl_surface underneath it.
        self.windows.remove(&id).is_some()
    }

    /// Mark one window's content changed; it repaints on the next loop
    /// pass (or when its in-flight frame callback returns).
    pub fn mark_dirty(&mut self, id: WindowId) {
        if let Some(win) = self.windows.get_mut(&id) {
            win.dirty = true;
        }
    }

    /// Mark every window dirty — the notify-all reactive model.
    pub fn mark_all_dirty(&mut self) {
        for win in self.windows.values_mut() {
            win.dirty = true;
        }
    }

    /// Ask `run` to return after the current dispatch pass.
    pub fn quit(&mut self) {
        self.exit = true;
    }

    /// Dispatch events and repaint dirty windows until [`Shell::quit`]
    /// (or, with [`Shell::exit_after_first_draw`], until every window
    /// has committed once).
    pub fn run(
        mut self,
        mut event_loop: calloop::EventLoop<'static, Shell>,
    ) -> Result<(), SurfaceError> {
        loop {
            event_loop.dispatch(None, &mut self)?;
            self.redraw_windows()?;
            self.conn.flush().map_err(DispatchError::from)?;
            let booted = self.exit_after_first_draw
                && !self.windows.is_empty()
                && self.windows.values().all(|w| w.drawn_once);
            if self.exit || booted {
                return Ok(());
            }
        }
    }

    /// (Re)create and map a window's layer surface. Also the recovery
    /// path when the compositor closes it on output unplug.
    fn map_window(&mut self, id: WindowId) {
        let qh = self.qh.clone();
        let Some(win) = self.windows.get_mut(&id) else {
            return;
        };
        let opts = &win.options;
        let surface = self.compositor.create_surface(&qh);
        let layer = self.layer_shell.create_layer_surface(
            &qh,
            surface,
            match opts.layer {
                Layer::Background => smithay_client_toolkit::shell::wlr_layer::Layer::Background,
                Layer::Bottom => smithay_client_toolkit::shell::wlr_layer::Layer::Bottom,
                Layer::Top => smithay_client_toolkit::shell::wlr_layer::Layer::Top,
                Layer::Overlay => smithay_client_toolkit::shell::wlr_layer::Layer::Overlay,
            },
            Some(opts.namespace.clone()),
            None, // compositor picks the output; survives unplug via closed()
        );
        layer.set_anchor(anchor_bits(opts.anchors));
        layer.set_size(opts.width, opts.height);
        layer.set_exclusive_zone(opts.exclusive_zone);
        let m = opts.margins;
        layer.set_margin(m.top, m.right, m.bottom, m.left);
        layer.set_keyboard_interactivity(match opts.keyboard {
            Keyboard::None => KeyboardInteractivity::None,
            Keyboard::OnDemand => KeyboardInteractivity::OnDemand,
            Keyboard::Exclusive => KeyboardInteractivity::Exclusive,
        });
        // Initial commit with no buffer; the configure that answers it
        // gives us the real size.
        layer.commit();
        win.layer = Some(layer);
        win.was_configured = false;
        win.configured = false;
        win.committed_size = None;
        win.dirty = false;
        win.frame_pending = false;
    }

    /// Draw + commit every window that owes content and has no frame
    /// callback in flight. Called once per loop iteration, after
    /// dispatch — handlers only set flags.
    fn redraw_windows(&mut self) -> Result<(), SurfaceError> {
        let qh = self.qh.clone();
        for win in self.windows.values_mut() {
            if !win.configured || !win.dirty || win.frame_pending {
                continue;
            }
            let Some(layer) = win.layer.as_ref() else {
                continue;
            };
            let (width, height) = win.buffer_size();
            if width == 0 || height == 0 {
                continue;
            }
            let stride = width as i32 * 4;
            let (buffer, canvas) = self.pool.create_buffer(
                width as i32,
                height as i32,
                stride,
                wl_shm::Format::Argb8888,
            )?;

            let fresh = win.committed_size != Some((width, height));
            let damage = win.painter.paint(Canvas {
                buf: canvas,
                width,
                height,
                scale: win.scale,
                fresh,
            });
            win.dirty = false;
            // A fresh surface has no content to diff against — whatever
            // the painter reported, the compositor needs the whole buffer.
            let damage = if fresh { Damage::Full } else { damage };
            let surface = layer.wl_surface();
            match damage {
                Damage::None => continue,
                Damage::Full => surface.damage_buffer(0, 0, width as i32, height as i32),
                Damage::Rects(rects) => {
                    for r in rects {
                        surface.damage_buffer(r.x, r.y, r.width, r.height);
                    }
                }
            }
            surface.set_buffer_scale(win.scale.max(1));
            // Frame callback only because a commit is in flight — once
            // it fires and nothing is dirty, no further callback is
            // requested.
            surface.frame(&qh, surface.clone());
            win.frame_pending = true;
            buffer.attach_to(surface)?;
            layer.commit();
            win.committed_size = Some((width, height));
            win.drawn_once = true;
        }
        Ok(())
    }

    fn window_by_surface(&mut self, surface: &wl_surface::WlSurface) -> Option<&mut Window> {
        self.windows.values_mut().find(|w| w.holds(surface))
    }
}

fn anchor_bits(a: Anchors) -> Anchor {
    let mut bits = Anchor::empty();
    bits.set(Anchor::TOP, a.top);
    bits.set(Anchor::BOTTOM, a.bottom);
    bits.set(Anchor::LEFT, a.left);
    bits.set(Anchor::RIGHT, a.right);
    bits
}

impl CompositorHandler for Shell {
    fn scale_factor_changed(
        &mut self,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
        surface: &wl_surface::WlSurface,
        new_factor: i32,
    ) {
        if let Some(win) = self.window_by_surface(surface) {
            if new_factor != win.scale {
                win.scale = new_factor;
                win.dirty = true;
            }
        }
    }

    fn transform_changed(
        &mut self,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
        _surface: &wl_surface::WlSurface,
        _new_transform: wl_output::Transform,
    ) {
    }

    fn frame(
        &mut self,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
        surface: &wl_surface::WlSurface,
        _time: u32,
    ) {
        if let Some(win) = self.window_by_surface(surface) {
            win.frame_pending = false;
            // If dirty, the post-dispatch redraw pass picks it up.
        }
    }

    fn surface_enter(
        &mut self,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
        _surface: &wl_surface::WlSurface,
        _output: &wl_output::WlOutput,
    ) {
    }

    fn surface_leave(
        &mut self,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
        _surface: &wl_surface::WlSurface,
        _output: &wl_output::WlOutput,
    ) {
    }
}

impl LayerShellHandler for Shell {
    fn closed(&mut self, _conn: &Connection, _qh: &QueueHandle<Self>, layer: &LayerSurface) {
        // Output gone (or compositor policy) — the unplug story. Remap
        // immediately only if this surface ever lived (got a configure)
        // and an output is still around; otherwise wait for new_output.
        // Instant unconditional remap loops forever against a compositor
        // that closes surfaces while it has no outputs.
        let closed_surface = layer.wl_surface();
        let Some((&id, win)) = self
            .windows
            .iter_mut()
            .find(|(_, w)| w.holds(closed_surface))
        else {
            return;
        };
        win.layer = None;
        win.configured = false;
        win.frame_pending = false;
        let lived = win.was_configured;
        if lived && self.output_state.outputs().next().is_some() {
            tracing::info!(?id, "layer surface closed by compositor; remapping");
            self.map_window(id);
        } else {
            tracing::info!(?id, "layer surface closed; waiting for an output to remap");
        }
    }

    fn configure(
        &mut self,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
        layer: &LayerSurface,
        configure: LayerSurfaceConfigure,
        _serial: u32,
    ) {
        let Some(win) = self.window_by_surface(layer.wl_surface()) else {
            return;
        };
        let (w, h) = configure.new_size;
        // 0 means "you pick": keep the current size if one exists,
        // else fall back to the requested option.
        let new = (
            if w != 0 {
                w
            } else if win.logical_size.0 != 0 {
                win.logical_size.0
            } else {
                win.options.width
            },
            if h != 0 {
                h
            } else if win.logical_size.1 != 0 {
                win.logical_size.1
            } else {
                win.options.height
            },
        );
        if new != win.logical_size || !win.configured {
            win.logical_size = new;
            win.dirty = true;
        }
        win.configured = true;
        win.was_configured = true;
    }
}

impl OutputHandler for Shell {
    fn output_state(&mut self) -> &mut OutputState {
        &mut self.output_state
    }

    fn new_output(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {
        // The replug story: an output (re)appeared while windows were
        // unmapped.
        let unmapped: Vec<WindowId> = self
            .windows
            .iter()
            .filter(|(_, w)| w.layer.is_none())
            .map(|(&id, _)| id)
            .collect();
        for id in unmapped {
            tracing::info!(?id, "output arrived; remapping layer surface");
            self.map_window(id);
        }
    }

    fn update_output(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}

    fn output_destroyed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
}

impl SeatHandler for Shell {
    fn seat_state(&mut self) -> &mut SeatState {
        &mut self.seat_state
    }

    fn new_seat(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_seat::WlSeat) {}

    // Input capabilities land in M4; the seat is bound so hotplug events
    // flow from day one.
    fn new_capability(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: wl_seat::WlSeat,
        _: Capability,
    ) {
    }

    fn remove_capability(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: wl_seat::WlSeat,
        _: Capability,
    ) {
    }

    fn remove_seat(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_seat::WlSeat) {}
}

impl ShmHandler for Shell {
    fn shm_state(&mut self) -> &mut Shm {
        &mut self.shm
    }
}

impl ProvidesRegistryState for Shell {
    fn registry(&mut self) -> &mut RegistryState {
        &mut self.registry_state
    }
    registry_handlers![OutputState, SeatState];
}

delegate_compositor!(Shell);
delegate_output!(Shell);
delegate_shm!(Shell);
delegate_seat!(Shell);
delegate_layer!(Shell);
delegate_registry!(Shell);

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bar_options_anchor_and_reserve() {
        let top = LayerOptions::bar(Edge::Top, 32, true);
        assert!(top.anchors.top && top.anchors.left && top.anchors.right);
        assert!(!top.anchors.bottom);
        assert_eq!((top.width, top.height), (0, 32));
        assert_eq!(top.exclusive_zone, 32);

        let bottom = LayerOptions::bar(Edge::Bottom, 24, false);
        assert!(bottom.anchors.bottom && !bottom.anchors.top);
        assert_eq!(bottom.exclusive_zone, 0);
    }

    #[test]
    fn anchor_bits_map_one_to_one() {
        assert_eq!(anchor_bits(Anchors::default()), Anchor::empty());
        assert_eq!(
            anchor_bits(Anchors {
                top: true,
                left: true,
                right: true,
                bottom: false,
            }),
            Anchor::TOP | Anchor::LEFT | Anchor::RIGHT
        );
        assert_eq!(
            anchor_bits(Anchors {
                top: true,
                bottom: true,
                left: true,
                right: true,
            }),
            Anchor::all()
        );
    }
}
