//! SCTK layer-surface mechanism: registry/seat/output binds, one anchored
//! wlr-layer-shell surface with an exclusive zone, double-buffered wl_shm
//! via `SlotPool`, precise `damage_buffer`, and frame callbacks requested
//! only while dirty — a fully idle surface schedules zero wakeups.
//!
//! This crate is mechanism only: it owns the Wayland plumbing and hands
//! the caller a raw ARGB8888 canvas through the [`Painter`] trait. What
//! gets drawn is the caller's policy.
//!
//! Coordinate doctrine (tomoe-inherited): the compositor speaks logical
//! pixels (configure sizes), painters speak integer physical (buffer)
//! pixels. Conversion happens in exactly one place, [`State::buffer_size`],
//! using the integer buffer scale.

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
            Anchor, KeyboardInteractivity, Layer, LayerShell, LayerShellHandler, LayerSurface,
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

/// The caller's drawing policy. Painters must leave every pixel outside
/// the returned damage identical to the previous paint — buffers
/// alternate, and the compositor trusts the damage report.
pub trait Painter {
    fn paint(&mut self, canvas: Canvas<'_>) -> Damage;
}

/// Where the bar-style surface anchors.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Edge {
    #[default]
    Top,
    Bottom,
}

/// Options for the single layer surface. (One surface for M0; the
/// multi-window story arrives with the runtime.)
#[derive(Clone, Debug)]
pub struct LayerOptions {
    pub namespace: String,
    /// Logical height of the bar; width follows the output.
    pub height: u32,
    pub edge: Edge,
    /// Reserve the bar's height as an exclusive zone.
    pub exclusive: bool,
    /// Exit the run loop right after the first frame is committed and
    /// flushed — the doctrine-06 boot check.
    pub exit_after_first_draw: bool,
}

impl Default for LayerOptions {
    fn default() -> Self {
        Self {
            namespace: "moonshell".into(),
            height: 32,
            edge: Edge::Top,
            exclusive: true,
            exit_after_first_draw: false,
        }
    }
}

/// Connect, map the layer surface, and run the calloop event loop until
/// the compositor closes us (or the first draw, in boot-check mode).
pub fn run(options: LayerOptions, painter: Box<dyn Painter>) -> Result<(), SurfaceError> {
    let conn = Connection::connect_to_env()?;
    let (globals, event_queue) = registry_queue_init::<State>(&conn)?;
    let qh: QueueHandle<State> = event_queue.handle();

    let compositor = CompositorState::bind(&globals, &qh)?;
    let layer_shell = LayerShell::bind(&globals, &qh)?;
    let shm = Shm::bind(&globals, &qh)?;

    // Two slots of the initial size; SlotPool grows on demand and reuses
    // released slots — buffers are recycled, never regenerated per frame.
    let pool = SlotPool::new((options.height as usize).max(1) * 4 * 2, &shm)?;

    let mut state = State {
        registry_state: RegistryState::new(&globals),
        seat_state: SeatState::new(&globals, &qh),
        output_state: OutputState::new(&globals, &qh),
        compositor,
        layer_shell,
        shm,
        pool,
        layer: None,
        options,
        painter,
        logical_size: (0, 0),
        scale: 1,
        configured: false,
        surface_was_configured: false,
        committed_size: None,
        dirty: false,
        frame_pending: false,
        first_draw_done: false,
        exit: false,
    };
    state.create_layer_surface(&qh);

    let mut event_loop: calloop::EventLoop<State> = calloop::EventLoop::try_new()?;
    calloop_wayland_source::WaylandSource::new(conn.clone(), event_queue)
        .insert(event_loop.handle())
        .map_err(|e| SurfaceError::LoopSource(e.to_string()))?;

    loop {
        event_loop.dispatch(None, &mut state)?;
        state.redraw_if_needed(&qh)?;
        conn.flush().map_err(DispatchError::from)?;
        if state.exit || (state.options.exit_after_first_draw && state.first_draw_done) {
            return Ok(());
        }
    }
}

struct State {
    registry_state: RegistryState,
    seat_state: SeatState,
    output_state: OutputState,
    compositor: CompositorState,
    layer_shell: LayerShell,
    shm: Shm,
    pool: SlotPool,
    layer: Option<LayerSurface>,
    options: LayerOptions,
    painter: Box<dyn Painter>,

    /// The current surface got at least one configure — gates instant
    /// remap on close, so a compositor that closes us before configuring
    /// (no outputs) can't drive a create/close storm.
    surface_was_configured: bool,
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
    first_draw_done: bool,
    exit: bool,
}

impl State {
    /// (Re)create and map the layer surface. Also the recovery path when
    /// the compositor closes us on output unplug.
    fn create_layer_surface(&mut self, qh: &QueueHandle<Self>) {
        let surface = self.compositor.create_surface(qh);
        let layer = self.layer_shell.create_layer_surface(
            qh,
            surface,
            Layer::Top,
            Some(self.options.namespace.clone()),
            None, // compositor picks the output; survives unplug via closed()
        );
        let anchor = match self.options.edge {
            Edge::Top => Anchor::TOP,
            Edge::Bottom => Anchor::BOTTOM,
        } | Anchor::LEFT
            | Anchor::RIGHT;
        layer.set_anchor(anchor);
        layer.set_size(0, self.options.height);
        if self.options.exclusive {
            layer.set_exclusive_zone(self.options.height as i32);
        }
        layer.set_keyboard_interactivity(KeyboardInteractivity::None);
        // Initial commit with no buffer; the configure that answers it
        // gives us the real size.
        layer.commit();
        self.layer = Some(layer);
        self.configured = false;
        self.frame_pending = false;
        self.dirty = false;
        self.surface_was_configured = false;
        self.committed_size = None;
    }

    /// The one logical→physical conversion point.
    fn buffer_size(&self) -> (u32, u32) {
        let s = self.scale.max(1) as u32;
        (self.logical_size.0 * s, self.logical_size.1 * s)
    }

    /// Draw + commit if content is owed and no frame callback is in
    /// flight. Called once per loop iteration, after dispatch — handlers
    /// only set flags.
    fn redraw_if_needed(&mut self, qh: &QueueHandle<Self>) -> Result<(), SurfaceError> {
        if !self.configured || !self.dirty || self.frame_pending {
            return Ok(());
        }
        let Some(layer) = self.layer.as_ref() else {
            return Ok(());
        };
        let (width, height) = self.buffer_size();
        if width == 0 || height == 0 {
            return Ok(());
        }
        let stride = width as i32 * 4;
        let (buffer, canvas) = self.pool.create_buffer(
            width as i32,
            height as i32,
            stride,
            wl_shm::Format::Argb8888,
        )?;

        let fresh = self.committed_size != Some((width, height));
        let damage = self.painter.paint(Canvas {
            buf: canvas,
            width,
            height,
            scale: self.scale,
            fresh,
        });
        self.dirty = false;
        // A fresh surface has no content to diff against — whatever the
        // painter reported, the compositor needs the whole buffer.
        let damage = if fresh { Damage::Full } else { damage };
        let surface = layer.wl_surface();
        match damage {
            Damage::None => return Ok(()),
            Damage::Full => surface.damage_buffer(0, 0, width as i32, height as i32),
            Damage::Rects(rects) => {
                for r in rects {
                    surface.damage_buffer(r.x, r.y, r.width, r.height);
                }
            }
        }
        surface.set_buffer_scale(self.scale.max(1));
        // Frame callback only because a commit is in flight — once it
        // fires and nothing is dirty, no further callback is requested.
        surface.frame(qh, surface.clone());
        self.frame_pending = true;
        buffer.attach_to(surface)?;
        layer.commit();
        self.committed_size = Some((width, height));
        self.first_draw_done = true;
        Ok(())
    }
}

impl CompositorHandler for State {
    fn scale_factor_changed(
        &mut self,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
        _surface: &wl_surface::WlSurface,
        new_factor: i32,
    ) {
        if new_factor != self.scale {
            self.scale = new_factor;
            self.dirty = true;
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
        _surface: &wl_surface::WlSurface,
        _time: u32,
    ) {
        self.frame_pending = false;
        // If dirty, the post-dispatch redraw pass picks it up.
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

impl LayerShellHandler for State {
    fn closed(&mut self, _conn: &Connection, qh: &QueueHandle<Self>, _layer: &LayerSurface) {
        // Output gone (or compositor policy) — the unplug story. Remap
        // immediately only if this surface ever lived (got a configure)
        // and an output is still around; otherwise wait for new_output.
        // Instant unconditional remap loops forever against a compositor
        // that closes surfaces while it has no outputs.
        self.layer = None;
        self.configured = false;
        self.frame_pending = false;
        if self.surface_was_configured && self.output_state.outputs().next().is_some() {
            tracing::info!("layer surface closed by compositor; remapping");
            self.create_layer_surface(qh);
        } else {
            tracing::info!("layer surface closed; waiting for an output to remap");
        }
    }

    fn configure(
        &mut self,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
        _layer: &LayerSurface,
        configure: LayerSurfaceConfigure,
        _serial: u32,
    ) {
        let (w, h) = configure.new_size;
        let new = (
            if w == 0 { self.logical_size.0 } else { w },
            if h == 0 { self.options.height } else { h },
        );
        if new != self.logical_size || !self.configured {
            self.logical_size = new;
            self.dirty = true;
        }
        self.configured = true;
        self.surface_was_configured = true;
    }
}

impl OutputHandler for State {
    fn output_state(&mut self) -> &mut OutputState {
        &mut self.output_state
    }

    fn new_output(&mut self, _: &Connection, qh: &QueueHandle<Self>, _: wl_output::WlOutput) {
        // The replug story: an output (re)appeared while we were unmapped.
        if self.layer.is_none() {
            tracing::info!("output arrived; remapping layer surface");
            self.create_layer_surface(qh);
        }
    }

    fn update_output(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}

    fn output_destroyed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
}

impl SeatHandler for State {
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

impl ShmHandler for State {
    fn shm_state(&mut self) -> &mut Shm {
        &mut self.shm
    }
}

impl ProvidesRegistryState for State {
    fn registry(&mut self) -> &mut RegistryState {
        &mut self.registry_state
    }
    registry_handlers![OutputState, SeatState];
}

delegate_compositor!(State);
delegate_output!(State);
delegate_shm!(State);
delegate_seat!(State);
delegate_layer!(State);
delegate_registry!(State);
