//! Unified PipeWire + wlr-screencopy pipeline for ScreenCast.
//!
//! # Architecture: DRIVER + ALLOC_BUFFERS + wayland-driven queue
//!
//! The compositor pushes frames rather than the more obvious "PipeWire
//! pulls, we push" model. The choice lets us deliver an output without being
//! pinned to the PipeWire graph driver's audio quantum (~46.875 fps =
//! 1024/48000 — the xdpw bug).
//!
//! ## Why each piece matters
//!
//! - **`PW_STREAM_FLAG_DRIVER`** marks our stream as the cycle driver in
//!   PipeWire's graph. Without it, the graph driver ends up being whatever
//!   audio sink is running, and that sink ticks at its quantum (typically
//!   1024/48000 ≈ 21.3 ms). Our video stream is then scheduled piggy-back on
//!   those ticks. With DRIVER we own the cycle pacing.
//!
//! - **`PW_STREAM_FLAG_ALLOC_BUFFERS`** tells PipeWire to pre-allocate the
//!   buffer slots from our `SPA_PARAM_Buffers` advert; our `add_buffer`
//!   callback fires once per slot with the empty `pw_buffer` so we can
//!   populate `spa_data{type, fd, maxsize}`. The preferred path allocates a
//!   GBM DMA-BUF and exposes the same BO both as a linux-dmabuf `wl_buffer`
//!   and as a PipeWire DMA-BUF. If GBM or linux-dmabuf is unavailable we fall
//!   back to the memfd + `wl_shm_pool` path.
//!
//! - **Consumer-negotiated real-time pacing**. We do NOT pace at output
//!   refresh. The stream is advertised with a free play rate and a
//!   `VideoMaxFramerate` ceiling (the output refresh in Hz) so the consumer
//!   picks the rate (e.g. 60 fps on a 240 Hz output). A `param_changed`
//!   listener reads the negotiated `max_framerate` and derives
//!   `min_time_between_frames = 1s / max_framerate`. The steady-state path in
//!   `on_frame_ready` then holds each buffer submission (sleeping the residual
//!   rather than busy-waiting) until that much real *monotonic* time has
//!   elapsed since the last delivered frame. Pacing off real time — not
//!   vblank cadence — is what keeps the video clock locked to the audio clock
//!   and A/V stable.
//!
//! - **Wayland-driven queue** closes the capture loop. With DRIVER set,
//!   `on_process` no longer fires on its own — there is no external pull. The
//!   `ready` event queues the frame's buffer (`pw_stream_queue_buffer`),
//!   delivering it to consumers and advancing the PipeWire cycle. The next
//!   `capture_output` is issued only after the pacing gate has elapsed, so the
//!   delivery rate is the consumer-negotiated rate, not one per vblank.
//!
//! - **Single thread** with `pipewire::loop_::Loop::add_io` attaching the
//!   wayland socket fd to the same loop the PipeWire main loop polls. The
//!   wayland event callbacks (which call `dequeue_raw_buffer` /
//!   `queue_raw_buffer`) and the PipeWire stream callbacks (which set up
//!   buffers in `add_buffer`) all run on the same thread, accessing
//!   `AppState` through one `Rc<RefCell<_>>`.
//!
//! ## Flow per frame
//!
//!   1. `kick_capture()` sends `capture_output` to the compositor and flushes
//!      the wayland socket immediately. (The `add_io` callback only wakes on
//!      *incoming* bytes, so without the flush the request would sit in the
//!      outbound queue forever.)
//!   2. Compositor sends `Buffer { format, w, h, stride }` — the values match
//!      what we negotiated with PipeWire; we wait for the sync event.
//!   3. `BufferDone` arrives → `dequeue_raw_buffer()` pops a pw_buffer; we
//!      look up its paired `wl_buffer` and call `frame.copy(&wl_buffer)`.
//!      The compositor writes pixels directly into the PipeWire-owned backing
//!      storage.
//!   4. `Ready` arrives → the pacing gate holds the submission until
//!      `min_time_between_frames` of real time has passed (consumer-negotiated
//!      rate, not vblank); then we set `chunk.size` on the pw_buffer, call
//!      `queue_raw_buffer()` (the wake-up for consumers AND the cycle advance
//!      for the DRIVER stream), and issue the next `kick_capture()`.

use std::cell::RefCell;
use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::Cursor;
use std::os::fd::{AsFd, AsRawFd, IntoRawFd, OwnedFd, RawFd};
use std::path::PathBuf;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use drm_fourcc::{DrmFourcc, DrmModifier};
use gbm::{BufferObject, BufferObjectFlags, Device as GbmDevice};
use pipewire as pw;
use pw::spa;
use pw::spa::param::format::{MediaSubtype, MediaType};
use pw::spa::param::format_utils::parse_format;
use pw::spa::param::video::VideoInfoRaw;
use pw::spa::param::ParamType;
use pw::spa::pod::serialize::PodSerializer;
use pw::spa::pod::{ChoiceValue, Object, Pod, Property, Value};
use pw::spa::support::system::IoFlags;
use pw::spa::utils::{Choice, ChoiceEnum, ChoiceFlags, Fraction, Id, Rectangle};
use spa::sys as spa_sys;
use wayland_client::protocol::{wl_buffer, wl_output, wl_registry, wl_shm, wl_shm_pool};
use wayland_client::{Connection, Dispatch, EventQueue, Proxy, QueueHandle};
use wayland_protocols::wp::linux_dmabuf::zv1::client::{
    zwp_linux_buffer_params_v1, zwp_linux_dmabuf_v1,
};
use wayland_protocols_wlr::screencopy::v1::client::{
    zwlr_screencopy_frame_v1::{self, ZwlrScreencopyFrameV1},
    zwlr_screencopy_manager_v1::ZwlrScreencopyManagerV1,
};

// Give a 0.1 ms allowance for presentation-time jitter before treating a
// frame as arriving too soon, mirroring niri's CAST_DELAY_ALLOWANCE.
const CAST_DELAY_ALLOWANCE: Duration = Duration::from_micros(100);

#[derive(Debug, Clone)]
pub struct StreamSpec {
    pub output_name: String,
    pub width: u32,
    pub height: u32,
    pub framerate: u32,
    /// Whether to render the cursor into the captured output. Translates to
    /// `overlay_cursor=1` on wlr-screencopy's `capture_output`.
    pub cursor_visible: bool,
}

pub struct StreamHandle {
    stop: Arc<AtomicBool>,
    join: Option<thread::JoinHandle<()>>,
}

impl Drop for StreamHandle {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        if let Some(j) = self.join.take() {
            let _ = j.join();
        }
    }
}

pub fn start(
    spec: StreamSpec,
) -> Result<(u32, StreamHandle), Box<dyn std::error::Error + Send + Sync>> {
    let stop = Arc::new(AtomicBool::new(false));
    let stop_for_thread = stop.clone();
    let (tx, rx) = mpsc::sync_channel::<Result<u32, String>>(1);
    let join = thread::Builder::new()
        .name("portal-screencast".into())
        .spawn(move || {
            if let Err(e) = run(spec, tx, stop_for_thread) {
                tracing::error!("screencast thread exited: {e}");
            }
        })?;
    let node_id = rx
        .recv()
        .map_err(|_| "screencast thread died before reporting node id".to_string())?
        .map_err(|e| -> Box<dyn std::error::Error + Send + Sync> { e.into() })?;
    tracing::info!(node_id, "screencast stream live");
    Ok((
        node_id,
        StreamHandle {
            stop,
            join: Some(join),
        },
    ))
}

// ─── Shared state ────────────────────────────────────────────────────────

struct AppState {
    spec: StreamSpec,

    // Wayland resources
    conn: Option<Connection>,
    qh: QueueHandle<AppState>,
    manager: Option<ZwlrScreencopyManagerV1>,
    target_output: Option<wl_output::WlOutput>,
    shm: Option<wl_shm::WlShm>,
    dmabuf: Option<zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1>,
    gbm: Option<GbmDevice<File>>,

    // PipeWire stream (cloneable Rc handle)
    stream: Option<pw::stream::StreamRc>,
    /// pw_buffer → its wrapping wl_buffer + backing storage.
    pw_buffer_slots: HashMap<PwBuf, BufferSlot>,
    /// pw_buffer → its negotiated stride (cached at add_buffer time).
    pw_buffer_stride: HashMap<PwBuf, i32>,

    // wlr-screencopy session state
    pending_frame: Option<PendingFrame>,

    // node-id handoff (taken on first Paused transition)
    node_id_tx: Option<mpsc::SyncSender<Result<u32, String>>>,

    // Diagnostics
    frames_completed: u64,
    last_log_at: std::time::Instant,
    capture_kicked: bool,

    // Once the consumer disconnects we short-circuit every callback so we
    // don't dereference a stale pw_buffer.
    dying: bool,
    stop_flag: Option<Arc<AtomicBool>>,

    // Real-time pacing state. The consumer negotiates a `VideoMaxFramerate`
    // via the `param_changed` listener; we translate that into the minimum
    // real (monotonic) interval between frames we may deliver and hold off on
    // submitting output until it elapses. Without this, the DRIVER stream
    // would deliver at output-vblank cadence (~240 Hz on a 240 Hz monitor)
    // while the negotiated format advertises ~60 fps, so video time advances
    // ~4x faster than the audio clock and A/V desync grows without bound.
    min_time_between_frames: Duration,
    /// Monotonic instant of the last frame actually delivered; `None` until
    /// the first frame is submitted.
    last_frame_time: Option<Instant>,
}

struct PendingFrame {
    frame: ZwlrScreencopyFrameV1,
    /// The PW buffer this frame is being copied into; `None` until a buffer
    /// has actually been dequeued and attached. Presence — not a `0` sentinel
    /// — is the flag, so there is no way to confuse "none" with a valid buf.
    pw_buffer: Option<PwBuf>,
}

struct BufferSlot {
    wl_buffer: wl_buffer::WlBuffer,
    _storage: BufferSlotStorage,
}

enum BufferSlotStorage {
    Shm {
        _shm_pool: wl_shm_pool::WlShmPool,
        _fd: OwnedFd,
    },
    Dmabuf {
        _bo: BufferObject<()>,
        _wl_fd: OwnedFd,
    },
}

struct AllocatedSlot {
    wl_buffer: wl_buffer::WlBuffer,
    storage: BufferSlotStorage,
    fd_for_pw: OwnedFd,
    stride: i32,
    size: usize,
}

/// Identity key for a PipeWire `pw_buffer` used in the slot maps. The pointer
/// is *never* dereferenced through this wrapper; it is a stable identity test
/// against the same underlying object that the add/remove_buffer callbacks and
/// `dequeue_raw_buffer` hand out. The pointer is valid only for as long as the
/// matching [`BufferSlot`] is present in `pw_buffer_slots`, and is only ever
/// handed back to PipeWire verbatim via `queue_raw_buffer` inside that window.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct PwBuf(*mut pw::sys::pw_buffer);

// Raw pw_buffer pointers don't carry Send / Sync inferred by the compiler, but
// the whole AppState only ever lives on a single thread.
unsafe impl Send for AppState {}

/// `add_io` requires its io source to implement `AsRawFd`. We hand it a
/// wrapper that *owns* the cloned wayland fd, so the fd lives exactly as long
/// as the `IoSource` PipeWire retains and its callback — no fabricated
/// `'static` borrow of a local is required.
struct FdHolder(OwnedFd);
impl AsRawFd for FdHolder {
    fn as_raw_fd(&self) -> RawFd {
        self.0.as_raw_fd()
    }
}

// ─── Stream thread entry ──────────────────────────────────────────────────

fn run(
    spec: StreamSpec,
    node_id_tx: mpsc::SyncSender<Result<u32, String>>,
    stop: Arc<AtomicBool>,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    pw::init();
    let mainloop = pw::main_loop::MainLoopRc::new(None)?;
    let context = pw::context::ContextRc::new(&mainloop, None)?;
    let core = context.connect_rc(None)?;

    // Connect Wayland.
    let conn = Connection::connect_to_env()?;
    let mut event_queue: EventQueue<AppState> = conn.new_event_queue();
    let qh = event_queue.handle();
    let _registry = conn.display().get_registry(&qh, ());

    // Stub AppState for the bootstrap roundtrips.
    let mut state = AppState {
        spec: spec.clone(),
        conn: Some(conn.clone()),
        qh: qh.clone(),
        manager: None,
        target_output: None,
        shm: None,
        dmabuf: None,
        gbm: None,
        stream: None,
        pw_buffer_slots: HashMap::new(),
        pw_buffer_stride: HashMap::new(),
        pending_frame: None,
        node_id_tx: Some(node_id_tx),
        frames_completed: 0,
        last_log_at: std::time::Instant::now(),
        capture_kicked: false,
        dying: false,
        stop_flag: Some(stop.clone()),
        min_time_between_frames: Duration::ZERO,
        last_frame_time: None,
    };

    // Bind globals (round 1), then receive wl_output names (round 2).
    event_queue.roundtrip(&mut state)?;
    event_queue.roundtrip(&mut state)?;

    if state.manager.is_none() {
        if let Some(tx) = state.node_id_tx.take() {
            let _ = tx.send(Err(
                "compositor doesn't expose zwlr_screencopy_manager_v1".into()
            ));
        }
        return Err("no zwlr_screencopy_manager_v1".into());
    }
    if state.shm.is_none() {
        if let Some(tx) = state.node_id_tx.take() {
            let _ = tx.send(Err("compositor doesn't expose wl_shm".into()));
        }
        return Err("no wl_shm".into());
    }
    state.gbm = match init_gbm_device() {
        Ok(Some(device)) if state.dmabuf.is_some() => {
            tracing::info!(
                backend = device.backend_name(),
                "screencast: DMA-BUF capture enabled"
            );
            Some(device)
        }
        Ok(Some(_)) => {
            tracing::info!("screencast: zwp_linux_dmabuf_v1 missing; using SHM capture");
            None
        }
        Ok(None) => {
            tracing::info!("screencast: no render node found; using SHM capture");
            None
        }
        Err(e) => {
            tracing::warn!("screencast: failed to initialize GBM ({e}); using SHM capture");
            None
        }
    };
    if state.target_output.is_none() {
        if let Some(tx) = state.node_id_tx.take() {
            let _ = tx.send(Err(format!("output {:?} not found", spec.output_name)));
        }
        return Err("target output not found".into());
    }
    tracing::info!(output = spec.output_name, "screencast: globals bound");

    // Build the PipeWire stream.
    let stream = pw::stream::StreamRc::new(
        core,
        "tomoe-screencast",
        pw::properties::properties! {
            *pw::keys::MEDIA_CLASS => "Video/Source",
            *pw::keys::MEDIA_ROLE => "Screen",
            *pw::keys::NODE_NAME => "tomoe-portal-stream",
            *pw::keys::NODE_DESCRIPTION => "tomoe portal screencast",
        },
    )?;
    state.stream = Some(stream.clone());

    let state_rc = Rc::new(RefCell::new(state));

    // Register stream listeners — they all funnel into AppState via the Rc.
    let s_state = state_rc.clone();
    let s_add = state_rc.clone();
    let s_remove = state_rc.clone();
    let s_param = state_rc.clone();
    let _listener = stream
        .add_local_listener_with_user_data(())
        .state_changed(move |stream, _ud, old, new| {
            s_state.borrow_mut().on_state_changed(stream, old, new);
        })
        .param_changed(move |_stream, _ud, id, pod| {
            // Subscribe to the consumer-negotiated format. The interesting
            // field is VideoMaxFramerate: the consumer picks the play rate and
            // we derive the real-time pacing interval from it (mirrors niri's
            // pw_utils param_changed handler). We keep it non-destructive — the
            // buffer/modifier/meta negotiation for the screencap pipeline is
            // driven by our EnumFormat advert + add_buffer below.
            if ParamType::from_raw(id) != ParamType::Format {
                return;
            }
            let Some(pod) = pod else {
                return;
            };
            let Ok((m_type, m_subtype)) = parse_format(pod) else {
                return;
            };
            if m_type != MediaType::Video || m_subtype != MediaSubtype::Raw {
                return;
            }
            let mut format = VideoInfoRaw::new();
            if format.parse(pod).is_err() {
                return;
            }
            let max_framerate = format.max_framerate();
            // Denominated per second: min interval = 1_000_000us * denom / num.
            // Guard num == 0 (free/unbounded rate) → no pacing constraint,
            // matching niri's zero `min_time_between_frames` default.
            let min_time = if max_framerate.num > 0 {
                Duration::from_micros(
                    (1_000_000 * u64::from(max_framerate.denom)) / u64::from(max_framerate.num),
                )
            } else {
                Duration::ZERO
            };
            s_param.borrow_mut().min_time_between_frames = min_time;
            tracing::debug!(
                ?max_framerate,
                ?min_time,
                "screencast: consumer-negotiated frame pacing"
            );
        })
        .add_buffer(move |stream, _ud, buffer| {
            s_add.borrow_mut().on_add_buffer(stream, buffer);
        })
        .remove_buffer(move |stream, _ud, buffer| {
            s_remove.borrow_mut().on_remove_buffer(stream, buffer);
        })
        .process(|_, _| {
            // No-op: the cycle is driven by queue_buffer from the
            // wlr-screencopy ready handler. on_process firing here is rare
            // under DRIVER and doesn't need to do anything.
        })
        .register()?;

    // Negotiate format + buffer params.
    let format_bytes = build_video_format_param(&spec)?;
    let buffers_bytes = build_buffers_param(&spec, state_rc.borrow().gbm.is_some())?;
    let format_pod = Pod::from_bytes(&format_bytes).ok_or("format POD parse failed".to_string())?;
    let buffers_pod =
        Pod::from_bytes(&buffers_bytes).ok_or("buffers POD parse failed".to_string())?;
    let mut params = [format_pod, buffers_pod];
    stream.connect(
        spa::utils::Direction::Output,
        None,
        pw::stream::StreamFlags::DRIVER | pw::stream::StreamFlags::ALLOC_BUFFERS,
        &mut params,
    )?;
    tracing::info!("screencast: PW stream connected (DRIVER | ALLOC_BUFFERS)");

    // Attach the wayland fd to the PW main loop. Move event_queue into the io
    // closure — after this point all wayland dispatching is fd-driven.
    let wl_fd = conn.as_fd().try_clone_to_owned()?;
    // Ownership of the wayland fd moves into `add_io`'s retained io source, so
    // the fd's lifetime is tied to the IoSource — no `'static` widening.
    let fd_holder = FdHolder(wl_fd);

    let s_for_io = state_rc.clone();
    let conn_for_io = conn.clone();
    let event_queue_cell = RefCell::new(event_queue);
    let _io = mainloop.loop_().add_io(fd_holder, IoFlags::IN, move |_| {
        // Read whatever wayland has on the socket without blocking.
        if let Some(guard) = conn_for_io.prepare_read() {
            let _ = guard.read();
        }
        let mut eq = event_queue_cell.borrow_mut();
        let mut state = s_for_io.borrow_mut();
        if let Err(e) = eq.dispatch_pending(&mut *state) {
            tracing::error!("wayland dispatch: {e}");
        }
        let _ = conn_for_io.flush();
    });

    // Make sure any pending wayland requests (the registry bind etc.) hit the
    // wire before mainloop starts blocking on its own poll.
    conn.flush()?;

    // Run until the stop flag flips (StreamHandle drop or stream death).
    let mainloop_for_stop = mainloop.clone();
    let stop_for_event = stop.clone();
    let event_check = mainloop.loop_().add_timer(move |_| {
        if stop_for_event.load(Ordering::SeqCst) {
            mainloop_for_stop.quit();
        }
    });
    let _ = event_check.update_timer(
        Some(std::time::Duration::from_millis(200)),
        Some(std::time::Duration::from_millis(200)),
    );

    mainloop.run();

    // Cleanup: detach stream slots; OwnedFds drop closes them.
    let mut state = state_rc.borrow_mut();
    state.pw_buffer_slots.clear();
    state.pending_frame = None;
    tracing::info!("screencast thread exiting cleanly");
    Ok(())
}

// ─── Wayland Dispatch impls ──────────────────────────────────────────────

impl Dispatch<wl_registry::WlRegistry, ()> for AppState {
    fn event(
        state: &mut Self,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        let wl_registry::Event::Global {
            name,
            interface,
            version,
        } = event
        else {
            return;
        };
        match interface.as_str() {
            "wl_output" => {
                // Bind eagerly; filter by name when we get the Name event.
                let _output =
                    registry.bind::<wl_output::WlOutput, _, _>(name, version.min(4), qh, ());
            }
            "wl_shm" => {
                state.shm =
                    Some(registry.bind::<wl_shm::WlShm, _, _>(name, version.min(1), qh, ()));
            }
            "zwp_linux_dmabuf_v1" => {
                state.dmabuf = Some(
                    registry.bind::<zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1, _, _>(
                        name,
                        version.min(3),
                        qh,
                        (),
                    ),
                );
            }
            "zwlr_screencopy_manager_v1" => {
                state.manager = Some(registry.bind::<ZwlrScreencopyManagerV1, _, _>(
                    name,
                    version.min(3),
                    qh,
                    (),
                ));
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_output::WlOutput, ()> for AppState {
    fn event(
        state: &mut Self,
        output: &wl_output::WlOutput,
        event: wl_output::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_output::Event::Name { name } = event {
            if name == state.spec.output_name && state.target_output.is_none() {
                state.target_output = Some(output.clone());
            }
        }
    }
}

// Empty/no-op dispatch for everything else we touch.
macro_rules! empty_dispatch {
    ($t:ty) => {
        impl Dispatch<$t, ()> for AppState {
            fn event(
                _: &mut Self,
                _: &$t,
                _: <$t as Proxy>::Event,
                _: &(),
                _: &Connection,
                _: &QueueHandle<Self>,
            ) {
            }
        }
    };
}
empty_dispatch!(wl_shm::WlShm);
empty_dispatch!(wl_shm_pool::WlShmPool);
empty_dispatch!(wl_buffer::WlBuffer);
empty_dispatch!(zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1);
empty_dispatch!(ZwlrScreencopyManagerV1);

impl Dispatch<zwp_linux_buffer_params_v1::ZwpLinuxBufferParamsV1, ()> for AppState {
    fn event(
        _: &mut Self,
        _: &zwp_linux_buffer_params_v1::ZwpLinuxBufferParamsV1,
        event: zwp_linux_buffer_params_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if matches!(event, zwp_linux_buffer_params_v1::Event::Failed) {
            tracing::warn!("screencast: DMA-BUF wl_buffer creation failed");
        }
    }

    wayland_client::event_created_child!(AppState, zwp_linux_buffer_params_v1::ZwpLinuxBufferParamsV1, [
        zwp_linux_buffer_params_v1::EVT_CREATED_OPCODE => (wl_buffer::WlBuffer, ())
    ]);
}

impl Dispatch<ZwlrScreencopyFrameV1, ()> for AppState {
    fn event(
        state: &mut Self,
        frame: &ZwlrScreencopyFrameV1,
        event: zwlr_screencopy_frame_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            // The advertised layout matches what we negotiated with PipeWire
            // (Xrgb8888 at output size); nothing to record, just wait for the
            // BufferDone sync event.
            zwlr_screencopy_frame_v1::Event::Buffer { .. } => {}
            zwlr_screencopy_frame_v1::Event::LinuxDmabuf { .. } => {}
            zwlr_screencopy_frame_v1::Event::BufferDone => {
                state.on_buffer_done(frame);
            }
            zwlr_screencopy_frame_v1::Event::Flags { .. } => {}
            zwlr_screencopy_frame_v1::Event::Damage { .. } => {}
            zwlr_screencopy_frame_v1::Event::Ready { .. } => {
                state.on_frame_ready();
            }
            zwlr_screencopy_frame_v1::Event::Failed => {
                state.on_frame_failed();
            }
            _ => {}
        }
    }
}

// ─── AppState behaviour ──────────────────────────────────────────────────

impl AppState {
    fn on_state_changed(
        &mut self,
        stream: &pw::stream::Stream,
        old: pw::stream::StreamState,
        new: pw::stream::StreamState,
    ) {
        tracing::info!(?old, ?new, "pw stream state");
        if matches!(
            new,
            pw::stream::StreamState::Paused | pw::stream::StreamState::Streaming
        ) {
            if let Some(tx) = self.node_id_tx.take() {
                let _ = tx.send(Ok(stream.node_id()));
            }
        }
        if matches!(new, pw::stream::StreamState::Streaming) && !self.capture_kicked {
            self.capture_kicked = true;
            self.kick_capture();
        }
        if matches!(
            new,
            pw::stream::StreamState::Error(_) | pw::stream::StreamState::Unconnected
        ) {
            self.dying = true;
            if let Some(pending) = self.pending_frame.take() {
                pending.frame.destroy();
            }
            if let Some(stop) = self.stop_flag.as_ref() {
                stop.store(true, Ordering::SeqCst);
            }
        }
    }

    fn on_add_buffer(&mut self, _stream: &pw::stream::Stream, buffer: *mut pw::sys::pw_buffer) {
        let negotiated_data_types = unsafe {
            let buf = (*buffer).buffer;
            if buf.is_null() {
                tracing::error!("on_add_buffer: pw_buffer.buffer is null");
                return;
            }
            let datas = std::slice::from_raw_parts_mut((*buf).datas, (*buf).n_datas as usize);
            if datas.is_empty() {
                tracing::error!("on_add_buffer: no datas in pw_buffer");
                return;
            }
            datas[0].type_
        };

        let slot = match self.create_slot_for_pw_data_types(negotiated_data_types) {
            Ok(slot) => slot,
            Err(e) => {
                tracing::error!(
                    data_types = negotiated_data_types,
                    "create screencast buffer: {e}"
                );
                return;
            }
        };
        let stride = slot.stride;
        let size = slot.size;
        // The backing type is determined by which branch produced the storage;
        // deriving it here keeps it consistent with the enum instead of a
        // hand-set duplicate.
        let data_type = match &slot.storage {
            BufferSlotStorage::Dmabuf { .. } => spa_sys::SPA_DATA_DmaBuf,
            BufferSlotStorage::Shm { .. } => spa_sys::SPA_DATA_MemFd,
        };
        let fd_for_pw = slot.fd_for_pw.into_raw_fd();
        let wl_buf = slot.wl_buffer;
        let storage = slot.storage;

        unsafe {
            let buf = (*buffer).buffer;
            let datas = std::slice::from_raw_parts_mut((*buf).datas, (*buf).n_datas as usize);
            let data = &mut datas[0];
            data.type_ = data_type;
            data.flags = spa_sys::SPA_DATA_FLAG_READWRITE;
            data.fd = fd_for_pw as i64;
            data.data = std::ptr::null_mut();
            // `size` was bounded to i32::MAX by `payload_size`, so it is
            // provably representable in both u32 (maxsize/chunk.size) fields.
            data.maxsize = size as u32;
            data.mapoffset = 0;

            let chunk = &mut *data.chunk;
            chunk.offset = 0;
            chunk.stride = stride;
            chunk.size = size as u32;
        }

        let key = PwBuf(buffer);
        self.pw_buffer_stride.insert(key, stride);
        self.pw_buffer_slots.insert(
            key,
            BufferSlot {
                wl_buffer: wl_buf,
                _storage: storage,
            },
        );
    }

    fn create_slot_for_pw_data_types(
        &self,
        data_types: spa_sys::spa_data_type,
    ) -> Result<AllocatedSlot, Box<dyn std::error::Error + Send + Sync>> {
        let dmabuf_flag = 1 << spa_sys::SPA_DATA_DmaBuf;
        let memfd_flag = 1 << spa_sys::SPA_DATA_MemFd;
        // Sanitize the mask at the boundary: keep only the bits we understand
        // so stray/unknown bits can't steer the fallback logic. (`==` on the
        // raw enum index previously compared mask to index, which is wrong.)
        let data_types = data_types & (dmabuf_flag | memfd_flag);
        let allows_dmabuf = data_types & dmabuf_flag != 0;
        let allows_memfd = data_types & memfd_flag != 0;

        if allows_dmabuf {
            match self.create_dmabuf_slot() {
                Ok(Some(slot)) => return Ok(slot),
                Ok(None) if !allows_memfd => {
                    return Err(
                        "PipeWire selected DMA-BUF, but DMA-BUF allocation is unavailable".into(),
                    );
                }
                Ok(None) => {}
                Err(e) if !allows_memfd => return Err(e),
                Err(e) => {
                    tracing::warn!("create DMA-BUF screencast buffer: {e}; falling back to SHM");
                }
            }
        }

        if allows_memfd {
            return self.create_shm_slot();
        }

        Err(format!("unsupported PipeWire data types bitmask: {data_types}").into())
    }

    fn create_dmabuf_slot(
        &self,
    ) -> Result<Option<AllocatedSlot>, Box<dyn std::error::Error + Send + Sync>> {
        let (Some(dmabuf), Some(gbm)) = (self.dmabuf.as_ref(), self.gbm.as_ref()) else {
            return Ok(None);
        };

        let flags = BufferObjectFlags::RENDERING | BufferObjectFlags::LINEAR;
        if !gbm.is_format_supported(DrmFourcc::Xrgb8888, flags) {
            return Ok(None);
        }

        let bo = gbm.create_buffer_object::<()>(
            self.spec.width,
            self.spec.height,
            DrmFourcc::Xrgb8888,
            flags,
        )?;
        if bo.plane_count() != 1 {
            return Err(format!(
                "expected single-plane XRGB8888 BO, got {}",
                bo.plane_count()
            )
            .into());
        }
        let stride = bo.stride_for_plane(0) as i32;
        let size = payload_size(stride, self.spec.height)?;
        let modifier = u64::from(DrmModifier::Linear);
        let modifier_hi = (modifier >> 32) as u32;
        let modifier_lo = (modifier & 0xffff_ffff) as u32;

        let wl_fd = bo.fd()?;
        let fd_for_pw = bo.fd()?;
        let params = dmabuf.create_params(&self.qh, ());
        params.add(
            wl_fd.as_fd(),
            0,
            bo.offset(0),
            stride as u32,
            modifier_hi,
            modifier_lo,
        );
        let wl_buffer = params.create_immed(
            self.spec.width as i32,
            self.spec.height as i32,
            DrmFourcc::Xrgb8888 as u32,
            zwp_linux_buffer_params_v1::Flags::empty(),
            &self.qh,
            (),
        );
        params.destroy();

        Ok(Some(AllocatedSlot {
            wl_buffer,
            storage: BufferSlotStorage::Dmabuf {
                _bo: bo,
                _wl_fd: wl_fd,
            },
            fd_for_pw,
            stride,
            size,
        }))
    }

    fn create_shm_slot(&self) -> Result<AllocatedSlot, Box<dyn std::error::Error + Send + Sync>> {
        let stride = xrgb8888_stride(self.spec.width)?;
        let size = payload_size(stride, self.spec.height)?;
        // size <= i32::MAX (enforced by payload_size), so these widenings are
        // sound: u64 for ftruncate, i32 for the shm pool, u32 for maxsize.
        let memfd =
            rustix::fs::memfd_create("tomoe-portal-pwbuf", rustix::fs::MemfdFlags::CLOEXEC)?;
        rustix::fs::ftruncate(&memfd, size as u64)?;

        let Some(shm) = self.shm.as_ref() else {
            return Err("wl_shm is not bound".into());
        };
        let pool = shm.create_pool(memfd.as_fd(), size as i32, &self.qh, ());
        let wl_buffer = pool.create_buffer(
            0,
            self.spec.width as i32,
            self.spec.height as i32,
            stride,
            wl_shm::Format::Xrgb8888,
            &self.qh,
            (),
        );
        let fd_for_pw = memfd.try_clone()?;

        Ok(AllocatedSlot {
            wl_buffer,
            storage: BufferSlotStorage::Shm {
                _shm_pool: pool,
                _fd: memfd,
            },
            fd_for_pw,
            stride,
            size,
        })
    }

    fn on_remove_buffer(&mut self, _stream: &pw::stream::Stream, buffer: *mut pw::sys::pw_buffer) {
        let key = PwBuf(buffer);
        if let Some(slot) = self.pw_buffer_slots.remove(&key) {
            slot.wl_buffer.destroy();
        }
        self.pw_buffer_stride.remove(&key);
        // If an in-flight wlr-screencopy frame was targeting this buffer,
        // abandon it. A late Ready event would otherwise call queue_raw_buffer
        // on the now-freed pointer (use-after-free when a consumer like OBS
        // disconnects mid-stream).
        let targets_this = self
            .pending_frame
            .as_ref()
            .is_some_and(|p| p.pw_buffer == Some(key));
        if targets_this {
            let pending = self.pending_frame.take().unwrap();
            pending.frame.destroy();
        }
    }

    /// Issue the next capture_output request. Must be called when there is
    /// no in-flight frame.
    fn kick_capture(&mut self) {
        if self.dying {
            return;
        }
        let (Some(manager), Some(output)) = (self.manager.as_ref(), self.target_output.as_ref())
        else {
            tracing::warn!("kick_capture: missing manager or output");
            return;
        };
        let overlay_cursor = if self.spec.cursor_visible { 1 } else { 0 };
        let frame = manager.capture_output(overlay_cursor, output, &self.qh, ());
        self.pending_frame = Some(PendingFrame {
            frame,
            pw_buffer: None,
        });
        // Critical: flush the request to the compositor immediately. The
        // wayland fd add_io callback only fires when we receive bytes, so
        // without an explicit flush here the request would sit in the
        // outbound queue forever and the compositor would never respond.
        if let Some(conn) = self.conn.as_ref() {
            if let Err(e) = conn.flush() {
                tracing::warn!("kick_capture: flush failed: {e}");
            }
        }
    }

    fn on_buffer_done(&mut self, _frame: &ZwlrScreencopyFrameV1) {
        if self.dying {
            return;
        }
        // Dequeue a PW buffer to fill.
        let Some(stream) = self.stream.clone() else {
            return;
        };
        let pw_buf = unsafe { stream.dequeue_raw_buffer() };
        if pw_buf.is_null() {
            // Consumer hasn't returned a buffer yet — all advertised slots
            // are in flight. Frequent under slow consumers like OBS at high
            // resolution; back off a bit before retrying so we don't
            // busy-loop. Functionally a dropped frame.
            tracing::debug!("buffer_done: dequeue_raw_buffer returned null");
            if let Some(p) = self.pending_frame.take() {
                p.frame.destroy();
            }
            thread::sleep(std::time::Duration::from_millis(2));
            self.kick_capture();
            return;
        }
        let key = PwBuf(pw_buf);
        let Some(slot) = self.pw_buffer_slots.get(&key) else {
            tracing::error!("buffer_done: no slot for dequeued pw_buffer");
            // SAFETY: pw_buf was dequeued above; handing it straight back to
            // the owning stream is the correct unwind.
            unsafe { stream.queue_raw_buffer(pw_buf) };
            return;
        };
        // Tell wlr-screencopy to copy into our wl_buffer.
        let Some(pending) = self.pending_frame.as_mut() else {
            tracing::error!("buffer_done: no pending frame");
            // SAFETY: as above.
            unsafe { stream.queue_raw_buffer(pw_buf) };
            return;
        };
        pending.frame.copy(&slot.wl_buffer);
        pending.pw_buffer = Some(key);
    }

    fn on_frame_ready(&mut self) {
        if self.dying {
            return;
        }

        // Real-time pacing gate (mirrors niri's check_time_and_schedule at the
        // start of the render pass): before submitting a frame, hold until at
        // least `min_time_between_frames` of real monotonic time has elapsed
        // since the last delivered frame. The consumer negotiates this
        // interval via `VideoMaxFramerate` in `param_changed`. Without the
        // hold, the DRIVER stream pushes one frame per output vblank (~240 fps
        // on a 240 Hz monitor) while the negotiated format advertises ~60 fps,
        // advancing video time ~4x faster than the audio clock and growing A/V
        // desync monotonically. We sleep the residual rather than busy-wait.
        let now = Instant::now();
        if let Some(last) = self.last_frame_time {
            let next_deadline = last + self.min_time_between_frames;
            if now < next_deadline {
                let remaining = next_deadline - now;
                if remaining >= CAST_DELAY_ALLOWANCE {
                    thread::sleep(remaining);
                }
            }
        }
        self.last_frame_time = Some(Instant::now());

        let Some(pending) = self.pending_frame.take() else {
            return;
        };
        pending.frame.destroy();

        // Set the chunk size on the dequeued PW buffer so the consumer reads
        // the right amount, then queue it. Re-check that the buffer still
        // exists in our slot map — PW may have freed it between dequeue and
        // ready (consumer disconnect path).
        if let Some(key) = pending.pw_buffer {
            if self.pw_buffer_slots.contains_key(&key) {
                if let Some(stream) = self.stream.clone() {
                    // SAFETY: `key` was dequeued by dequeue_raw_buffer and is
                    // only valid while its slot is present (checked above);
                    // within that window we read the pre-negotiated chunk fields
                    // and hand the same pointer back to `queue_raw_buffer`,
                    // which restores ownership to the consumer.
                    let pw_buf = key.0;
                    unsafe {
                        if !pw_buf.is_null()
                            && !(*pw_buf).buffer.is_null()
                            && (*(*pw_buf).buffer).n_datas > 0
                        {
                            let datas = std::slice::from_raw_parts_mut(
                                (*(*pw_buf).buffer).datas,
                                (*(*pw_buf).buffer).n_datas as usize,
                            );
                            let data = &mut datas[0];
                            let stride = *self.pw_buffer_stride.get(&key).unwrap_or(&0);
                            let chunk = &mut *data.chunk;
                            chunk.offset = 0;
                            chunk.stride = stride;
                            // Same stride/height validated at add_buffer; use
                            // checked arithmetic so an unexpected overflow
                            // degrades to a 0-byte chunk rather than wrapping.
                            chunk.size = (stride as usize)
                                .checked_mul(self.spec.height as usize)
                                .unwrap_or(0) as u32;
                        }
                        stream.queue_raw_buffer(pw_buf);
                    }
                }
            }
        }

        self.frames_completed += 1;
        if self.last_log_at.elapsed() >= std::time::Duration::from_secs(2) {
            tracing::info!(frames = self.frames_completed, "screencast: frames queued");
            self.last_log_at = std::time::Instant::now();
        }

        // Issue the next capture. The pacing gate at the top of this fn
        // already held us to the consumer-negotiated rate, so the cycle stays
        // real-time paced rather than running at vblank cadence.
        self.kick_capture();
    }

    fn on_frame_failed(&mut self) {
        if self.dying {
            return;
        }
        tracing::warn!("screencast frame failed");
        if let Some(pending) = self.pending_frame.take() {
            pending.frame.destroy();
            // Return the dequeued buffer (if any) — PW expects buffers back.
            if let Some(pw_buf) = pending.pw_buffer {
                if let Some(stream) = self.stream.clone() {
                    // SAFETY: the pointer is still owned by this stream (it
                    // was dequeued and never queued); returning it verbatim to
                    // `queue_raw_buffer` restores the consumer's ownership.
                    unsafe { stream.queue_raw_buffer(pw_buf.0) };
                }
            }
        }
        // Backoff briefly and retry.
        thread::sleep(std::time::Duration::from_millis(50));
        self.kick_capture();
    }
}

/// XRGB8888 stride for a width, in checked arithmetic. Widths come from the
/// compositor/output (untrusted); reject any that can't be expressed as an
/// `i32` stride (the `spa_chunk.stride` field width) before any truncating
/// cast.
fn xrgb8888_stride(width: u32) -> Result<i32, String> {
    let bytes = width
        .checked_mul(4)
        .ok_or_else(|| format!("screencast stride overflow: {width} * 4"))?;
    i32::try_from(bytes).map_err(|_| format!("screencast stride {bytes} exceeds i32"))
}

/// Checked stride × height payload size for the buffer backing store. The
/// result is bounded to `i32::MAX` so it is provably representable in the `u32`
/// `spa_data.maxsize` / `spa_chunk.size` fields and the `i32`
/// `SPA_PARAM_BUFFERS_size` pod-value — no truncating / wrapping casts later.
fn payload_size(stride: i32, height: u32) -> Result<usize, String> {
    let stride = usize::try_from(stride).map_err(|_| "negative buffer stride".to_string())?;
    let height = usize::try_from(height).map_err(|_| "height overflows usize".to_string())?;
    let size = stride
        .checked_mul(height)
        .ok_or_else(|| format!("screencast payload overflow: {stride} * {height}"))?;
    if size > i32::MAX as usize {
        return Err(format!("screencast payload {size} exceeds i32 maxsize"));
    }
    Ok(size)
}

fn init_gbm_device() -> Result<Option<GbmDevice<File>>, Box<dyn std::error::Error + Send + Sync>> {
    let mut candidates = Vec::new();
    if let Ok(path) = std::env::var("TOMOE_SCREENCAST_RENDER_NODE") {
        candidates.push(PathBuf::from(path));
    }
    candidates.extend((128..200).map(|idx| PathBuf::from(format!("/dev/dri/renderD{idx}"))));

    for path in candidates {
        let file = match OpenOptions::new().read(true).write(true).open(&path) {
            Ok(file) => file,
            Err(_) => continue,
        };
        match GbmDevice::new(file) {
            Ok(device) => {
                tracing::info!(path = %path.display(), "screencast: opened GBM render node");
                return Ok(Some(device));
            }
            Err(e) => {
                tracing::debug!(path = %path.display(), "GBM device init failed: {e}");
            }
        }
    }

    Ok(None)
}

// ─── POD builders ─────────────────────────────────────────────────────────

fn build_video_format_param(
    spec: &StreamSpec,
) -> Result<Vec<u8>, Box<dyn std::error::Error + Send + Sync>> {
    // Advertise like niri: `VideoFramerate` is free (0/1) so the consumer picks
    // the play rate, and `VideoMaxFramerate` is a range capped at the output
    // refresh in Hz (spec.framerate). The consumer coalesces on the rate it
    // wants (e.g. 60 fps on a 240 Hz output); we then pace delivery to that
    // negotiated max via `param_changed`.
    let max_framerate = Fraction {
        num: spec.framerate.max(1),
        denom: 1,
    };
    let obj = Value::Object(Object {
        type_: spa_sys::SPA_TYPE_OBJECT_Format,
        id: spa_sys::SPA_PARAM_EnumFormat,
        properties: vec![
            Property::new(
                spa_sys::SPA_FORMAT_mediaType,
                Value::Id(Id(spa_sys::SPA_MEDIA_TYPE_video)),
            ),
            Property::new(
                spa_sys::SPA_FORMAT_mediaSubtype,
                Value::Id(Id(spa_sys::SPA_MEDIA_SUBTYPE_raw)),
            ),
            Property::new(
                spa_sys::SPA_FORMAT_VIDEO_format,
                Value::Id(Id(spa_sys::SPA_VIDEO_FORMAT_BGRx)),
            ),
            Property::new(
                spa_sys::SPA_FORMAT_VIDEO_size,
                Value::Rectangle(Rectangle {
                    width: spec.width,
                    height: spec.height,
                }),
            ),
            // Free play rate: the consumer chooses.
            Property::new(
                spa_sys::SPA_FORMAT_VIDEO_framerate,
                Value::Fraction(Fraction { num: 0, denom: 1 }),
            ),
            // Ceiling at the output refresh; consumer picks within [1, refresh].
            Property::new(
                spa_sys::SPA_FORMAT_VIDEO_maxFramerate,
                Value::Choice(ChoiceValue::Fraction(Choice(
                    ChoiceFlags::empty(),
                    ChoiceEnum::Range {
                        default: max_framerate,
                        min: Fraction { num: 1, denom: 1 },
                        max: max_framerate,
                    },
                ))),
            ),
        ],
    });
    Ok(PodSerializer::serialize(Cursor::new(Vec::new()), &obj)?
        .0
        .into_inner())
}

fn build_buffers_param(
    spec: &StreamSpec,
    prefer_dmabuf: bool,
) -> Result<Vec<u8>, Box<dyn std::error::Error + Send + Sync>> {
    let memfd_flag = 1 << spa_sys::SPA_DATA_MemFd;
    let dmabuf_flag = 1 << spa_sys::SPA_DATA_DmaBuf;
    let mut properties = vec![
        Property::new(
            spa_sys::SPA_PARAM_BUFFERS_buffers,
            Value::Choice(ChoiceValue::Int(Choice(
                ChoiceFlags::empty(),
                ChoiceEnum::Range {
                    default: 8,
                    min: 2,
                    max: 16,
                },
            ))),
        ),
        Property::new(spa_sys::SPA_PARAM_BUFFERS_blocks, Value::Int(1)),
    ];

    if prefer_dmabuf {
        properties.push(Property::new(
            spa_sys::SPA_PARAM_BUFFERS_dataType,
            Value::Choice(ChoiceValue::Int(Choice(
                ChoiceFlags::empty(),
                ChoiceEnum::Flags {
                    default: dmabuf_flag | memfd_flag,
                    flags: vec![dmabuf_flag, memfd_flag],
                },
            ))),
        ));
    } else {
        let stride = xrgb8888_stride(spec.width)?;
        let size = payload_size(stride, spec.height)?;
        properties.extend([
            Property::new(spa_sys::SPA_PARAM_BUFFERS_size, Value::Int(size as i32)),
            Property::new(spa_sys::SPA_PARAM_BUFFERS_stride, Value::Int(stride)),
            Property::new(
                spa_sys::SPA_PARAM_BUFFERS_dataType,
                Value::Choice(ChoiceValue::Int(Choice(
                    ChoiceFlags::empty(),
                    ChoiceEnum::Flags {
                        default: memfd_flag,
                        flags: vec![memfd_flag],
                    },
                ))),
            ),
        ]);
    }

    let obj = Value::Object(Object {
        type_: spa_sys::SPA_TYPE_OBJECT_ParamBuffers,
        id: spa_sys::SPA_PARAM_Buffers,
        properties,
    });
    Ok(PodSerializer::serialize(Cursor::new(Vec::new()), &obj)?
        .0
        .into_inner())
}
