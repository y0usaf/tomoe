//! Toplevel (single-window) streaming via ext-image-copy-capture-v1.
//!
//! Sibling to [`crate::pipewire_stream`], which streams whole outputs through
//! wlr-screencopy. Same architecture (DRIVER + ALLOC_BUFFERS +
//! wayland-driven queue on a single thread — see that module for the long
//! rationale), ported from ShojiWM's `toplevel_stream.rs` with three protocol
//! differences from the output path:
//!
//!   1. The capture source comes from
//!      `ext_foreign_toplevel_image_capture_source_manager_v1::create_source`
//!      pointed at an `ExtForeignToplevelHandleV1` (found by identifier).
//!   2. A long-lived `ExtImageCopyCaptureSessionV1` advertises `buffer_size`
//!      / `shm_format` / `done` up front, and *those* pick the PipeWire
//!      dims/format — for a toplevel the compositor is the only authoritative
//!      size source.
//!   3. Per frame: `dequeue_raw_buffer → session.create_frame →
//!      attach_buffer → capture → ready → queue_raw_buffer → next dequeue`.
//!
//! When the window resizes, the compositor pushes new constraints
//! (`buffer_size` + `done` again); the io callback then calls
//! `update_params` so PipeWire reallocates buffers at the new size and the
//! add/remove_buffer callbacks recreate the wl_buffer wraps.
//!
//! Buffers are memfd/shm only for now (ShojiWM parity); the compositor also
//! accepts dmabuf for toplevel sessions, so a GBM path like the output
//! stream's can come later.

use std::cell::RefCell;
use std::collections::HashMap;
use std::io::Cursor;
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, IntoRawFd, OwnedFd, RawFd};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::Arc;
use std::thread;

use pipewire as pw;
use pw::spa;
use pw::spa::pod::serialize::PodSerializer;
use pw::spa::pod::{ChoiceValue, Object, Pod, Property, Value};
use pw::spa::support::system::IoFlags;
use pw::spa::utils::{Choice, ChoiceEnum, ChoiceFlags, Fraction, Id, Rectangle};
use spa::sys as spa_sys;
use wayland_client::protocol::{wl_buffer, wl_registry, wl_shm, wl_shm_pool};
use wayland_client::{Connection, Dispatch, EventQueue, Proxy, QueueHandle, WEnum};
use wayland_protocols::ext::foreign_toplevel_list::v1::client::{
    ext_foreign_toplevel_handle_v1::{self, ExtForeignToplevelHandleV1},
    ext_foreign_toplevel_list_v1::{self, ExtForeignToplevelListV1},
};
use wayland_protocols::ext::image_capture_source::v1::client::{
    ext_foreign_toplevel_image_capture_source_manager_v1::ExtForeignToplevelImageCaptureSourceManagerV1,
    ext_image_capture_source_v1::ExtImageCaptureSourceV1,
};
use wayland_protocols::ext::image_copy_capture::v1::client::{
    ext_image_copy_capture_frame_v1::{self, ExtImageCopyCaptureFrameV1},
    ext_image_copy_capture_manager_v1::{self, ExtImageCopyCaptureManagerV1},
    ext_image_copy_capture_session_v1::{self, ExtImageCopyCaptureSessionV1},
};

#[derive(Debug, Clone)]
pub struct StreamSpec {
    pub toplevel_identifier: String,
    pub framerate: u32,
    /// Whether to advertise `paint_cursors` on the session; the compositor
    /// embeds the cursor while it hovers the captured window.
    pub cursor_visible: bool,
}

/// What `start` hands back: the PW node plus the compositor-advertised
/// window size the stream negotiated with.
#[derive(Debug, Clone, Copy)]
pub struct StreamInfo {
    pub node_id: u32,
    pub width: u32,
    pub height: u32,
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
) -> Result<(StreamInfo, StreamHandle), Box<dyn std::error::Error + Send + Sync>> {
    let stop = Arc::new(AtomicBool::new(false));
    let stop_for_thread = stop.clone();
    let (tx, rx) = mpsc::sync_channel::<Result<StreamInfo, String>>(1);
    let join = thread::Builder::new()
        .name("portal-toplevel-cast".into())
        .spawn(move || {
            if let Err(e) = run(spec, tx, stop_for_thread) {
                tracing::error!("toplevel stream thread exited: {e}");
            }
        })?;
    let info = rx
        .recv()
        .map_err(|_| "toplevel stream thread died before reporting node id".to_string())?
        .map_err(|e| -> Box<dyn std::error::Error + Send + Sync> { e.into() })?;
    tracing::info!(node_id = info.node_id, "toplevel stream live");
    Ok((
        info,
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
    shm: Option<wl_shm::WlShm>,
    capture_manager: Option<ExtImageCopyCaptureManagerV1>,
    toplevel_source_manager: Option<ExtForeignToplevelImageCaptureSourceManagerV1>,

    // Toplevel discovery (filled during bootstrap roundtrips).
    toplevels: Vec<DiscoveredToplevel>,
    target_toplevel: Option<ExtForeignToplevelHandleV1>,

    // Session + source (created after the toplevel is found).
    source: Option<ExtImageCaptureSourceV1>,
    session: Option<ExtImageCopyCaptureSessionV1>,

    // Session-advertised buffer constraints.
    adv_width: u32,
    adv_height: u32,
    adv_format: Option<wl_shm::Format>,
    adv_done: bool,
    session_stopped: bool,

    // PipeWire stream (cloneable Rc handle).
    stream: Option<pw::stream::StreamRc>,
    /// pw_buffer raw ptr → its wrapping wl_buffer + owned memfd.
    pw_buffer_slots: HashMap<usize, BufferSlot>,
    /// pw_buffer raw ptr → its negotiated stride (cached at add_buffer time).
    pw_buffer_stride: HashMap<usize, i32>,

    // Per-frame in-flight state.
    pending_frame: Option<PendingFrame>,

    // node-id handoff (taken on first Paused transition).
    node_id_tx: Option<mpsc::SyncSender<Result<StreamInfo, String>>>,

    // Diagnostics.
    frames_completed: u64,
    last_log_at: std::time::Instant,
    capture_kicked: bool,

    // Once the consumer disconnects, short-circuit every callback so we
    // never touch a torn-down buffer or stream.
    dying: bool,
    stop_flag: Option<Arc<AtomicBool>>,

    // Renegotiation: set by the session BufferSize handler when constraints
    // change AFTER the initial Done (the compositor saw the window resize).
    // The io callback picks it up post-dispatch and calls `update_params` so
    // PipeWire reallocates buffers at the new size.
    needs_renegotiate: bool,
    negotiated_width: u32,
    negotiated_height: u32,
}

struct DiscoveredToplevel {
    proxy: ExtForeignToplevelHandleV1,
    identifier: String,
}

struct PendingFrame {
    frame: ExtImageCopyCaptureFrameV1,
    pw_buffer: usize,
}

struct BufferSlot {
    wl_buffer: wl_buffer::WlBuffer,
    _shm_pool: wl_shm_pool::WlShmPool,
    _fd: OwnedFd,
}

// Raw pw_buffer pointers don't carry Send inferred by the compiler, but the
// whole AppState only ever lives on a single thread.
unsafe impl Send for AppState {}

/// Carrier for a BorrowedFd inside `add_io` (which needs `AsRawFd`).
struct FdHolder(BorrowedFd<'static>);
impl AsRawFd for FdHolder {
    fn as_raw_fd(&self) -> RawFd {
        self.0.as_raw_fd()
    }
}

// ─── Stream thread entry ──────────────────────────────────────────────────

fn run(
    spec: StreamSpec,
    node_id_tx: mpsc::SyncSender<Result<StreamInfo, String>>,
    stop: Arc<AtomicBool>,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    pw::init();
    let mainloop = pw::main_loop::MainLoopRc::new(None)?;
    let context = pw::context::ContextRc::new(&mainloop, None)?;
    let core = context.connect_rc(None)?;

    let conn = Connection::connect_to_env()?;
    let mut event_queue: EventQueue<AppState> = conn.new_event_queue();
    let qh = event_queue.handle();
    let _registry = conn.display().get_registry(&qh, ());

    let mut state = AppState {
        spec: spec.clone(),
        conn: Some(conn.clone()),
        qh: qh.clone(),
        shm: None,
        capture_manager: None,
        toplevel_source_manager: None,
        toplevels: Vec::new(),
        target_toplevel: None,
        source: None,
        session: None,
        adv_width: 0,
        adv_height: 0,
        adv_format: None,
        adv_done: false,
        session_stopped: false,
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
        needs_renegotiate: false,
        negotiated_width: 0,
        negotiated_height: 0,
    };

    // Bind globals (round 1), receive toplevel handle events (rounds 2–3).
    for _ in 0..3 {
        event_queue.roundtrip(&mut state)?;
    }

    let fail = |state: &mut AppState, err: String| {
        if let Some(tx) = state.node_id_tx.take() {
            let _ = tx.send(Err(err.clone()));
        }
        err
    };
    if state.capture_manager.is_none() {
        let err = fail(
            &mut state,
            "compositor doesn't expose ext_image_copy_capture_manager_v1".into(),
        );
        return Err(err.into());
    }
    if state.toplevel_source_manager.is_none() {
        let err = fail(
            &mut state,
            "compositor doesn't expose ext_foreign_toplevel_image_capture_source_manager_v1".into(),
        );
        return Err(err.into());
    }
    if state.shm.is_none() {
        let err = fail(&mut state, "compositor doesn't expose wl_shm".into());
        return Err(err.into());
    }
    state.target_toplevel = state
        .toplevels
        .iter()
        .find(|t| t.identifier == spec.toplevel_identifier)
        .map(|t| t.proxy.clone());
    if state.target_toplevel.is_none() {
        let err = fail(
            &mut state,
            format!("toplevel {:?} not found", spec.toplevel_identifier),
        );
        return Err(err.into());
    }

    // Create source + session, then wait for the session to advertise its
    // constraints; the PipeWire stream dims/format depend on them.
    {
        let manager = state.toplevel_source_manager.clone().unwrap();
        let capture = state.capture_manager.clone().unwrap();
        let toplevel = state.target_toplevel.clone().unwrap();
        let source = manager.create_source(&toplevel, &qh, ());
        let options = if spec.cursor_visible {
            ext_image_copy_capture_manager_v1::Options::PaintCursors
        } else {
            ext_image_copy_capture_manager_v1::Options::empty()
        };
        let session = capture.create_session(&source, options, &qh, ());
        state.source = Some(source);
        state.session = Some(session);
    }
    conn.flush()?;
    for _ in 0..6 {
        event_queue.roundtrip(&mut state)?;
        if state.adv_done || state.session_stopped {
            break;
        }
    }
    if state.session_stopped {
        let err = fail(
            &mut state,
            "session stopped before advertising constraints".into(),
        );
        return Err(err.into());
    }
    if !state.adv_done || state.adv_width == 0 || state.adv_height == 0 {
        let err = fail(
            &mut state,
            "session never finalized buffer constraints".into(),
        );
        return Err(err.into());
    }
    if !matches!(state.adv_format, Some(wl_shm::Format::Xrgb8888)) {
        tracing::warn!(
            advertised = ?state.adv_format,
            "toplevel session didn't advertise Xrgb8888; forcing it anyway"
        );
        state.adv_format = Some(wl_shm::Format::Xrgb8888);
    }
    tracing::info!(
        width = state.adv_width,
        height = state.adv_height,
        format = ?state.adv_format,
        "toplevel session constraints negotiated"
    );
    state.negotiated_width = state.adv_width;
    state.negotiated_height = state.adv_height;

    // Build the PipeWire stream with the negotiated dims.
    let stream = pw::stream::StreamRc::new(
        core,
        "tomoe-toplevel-screencast",
        pw::properties::properties! {
            *pw::keys::MEDIA_CLASS => "Video/Source",
            *pw::keys::MEDIA_ROLE => "Screen",
            *pw::keys::NODE_NAME => "tomoe-portal-toplevel-stream",
            *pw::keys::NODE_DESCRIPTION => "tomoe portal toplevel screencast",
        },
    )?;
    state.stream = Some(stream.clone());

    let state_rc = Rc::new(RefCell::new(state));

    let s_state = state_rc.clone();
    let s_add = state_rc.clone();
    let s_remove = state_rc.clone();
    let _listener = stream
        .add_local_listener_with_user_data(())
        .state_changed(move |stream, _ud, old, new| {
            s_state.borrow_mut().on_state_changed(stream, old, new);
        })
        .add_buffer(move |stream, _ud, buffer| {
            s_add.borrow_mut().on_add_buffer(stream, buffer);
        })
        .remove_buffer(move |stream, _ud, buffer| {
            s_remove.borrow_mut().on_remove_buffer(stream, buffer);
        })
        .process(|_, _| {
            // No-op: the cycle is driven by queue_buffer from on_frame_ready.
        })
        .register()?;

    let (adv_width, adv_height) = {
        let s = state_rc.borrow();
        (s.adv_width, s.adv_height)
    };
    let format_bytes = build_video_format_param(adv_width, adv_height, spec.framerate)?;
    let buffers_bytes = build_buffers_param(adv_width, adv_height)?;
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
    tracing::info!("toplevel screencast: PW stream connected (DRIVER | ALLOC_BUFFERS)");

    // Attach the wayland fd to the PW main loop.
    let wl_fd = conn.as_fd().try_clone_to_owned()?;
    // SAFETY: we own the OwnedFd for the lifetime of the closure; the fd
    // outlives the IoSource.
    let wl_fd_static: BorrowedFd<'static> =
        unsafe { std::mem::transmute::<BorrowedFd<'_>, BorrowedFd<'static>>(wl_fd.as_fd()) };
    let fd_holder = FdHolder(wl_fd_static);

    let s_for_io = state_rc.clone();
    let conn_for_io = conn.clone();
    let event_queue_cell = RefCell::new(event_queue);
    let _io = mainloop.loop_().add_io(fd_holder, IoFlags::IN, move |_| {
        if let Some(guard) = conn_for_io.prepare_read() {
            let _ = guard.read();
        }
        let mut eq = event_queue_cell.borrow_mut();
        let mut state = s_for_io.borrow_mut();
        if let Err(e) = eq.dispatch_pending(&mut *state) {
            tracing::error!("wayland dispatch: {e}");
        }
        state.maybe_renegotiate();
        let _ = conn_for_io.flush();
    });

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

    let mut state = state_rc.borrow_mut();
    state.pw_buffer_slots.clear();
    state.pending_frame = None;
    if let Some(s) = state.session.take() {
        s.destroy();
    }
    if let Some(s) = state.source.take() {
        s.destroy();
    }
    tracing::info!("toplevel screencast thread exiting cleanly");
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
            "wl_shm" => {
                state.shm =
                    Some(registry.bind::<wl_shm::WlShm, _, _>(name, version.min(1), qh, ()));
            }
            "ext_image_copy_capture_manager_v1" => {
                state.capture_manager = Some(registry.bind::<ExtImageCopyCaptureManagerV1, _, _>(
                    name,
                    version.min(1),
                    qh,
                    (),
                ));
            }
            "ext_foreign_toplevel_image_capture_source_manager_v1" => {
                state.toplevel_source_manager = Some(
                    registry.bind::<ExtForeignToplevelImageCaptureSourceManagerV1, _, _>(
                        name,
                        version.min(1),
                        qh,
                        (),
                    ),
                );
            }
            "ext_foreign_toplevel_list_v1" => {
                registry.bind::<ExtForeignToplevelListV1, _, _>(name, version.min(1), qh, ());
            }
            _ => {}
        }
    }
}

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
empty_dispatch!(ExtImageCopyCaptureManagerV1);
empty_dispatch!(ExtForeignToplevelImageCaptureSourceManagerV1);
empty_dispatch!(ExtImageCaptureSourceV1);

impl Dispatch<ExtForeignToplevelListV1, ()> for AppState {
    fn event(
        _: &mut Self,
        _: &ExtForeignToplevelListV1,
        _: ext_foreign_toplevel_list_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
    wayland_client::event_created_child!(AppState, ExtForeignToplevelListV1, [
        ext_foreign_toplevel_list_v1::EVT_TOPLEVEL_OPCODE => (ExtForeignToplevelHandleV1, ()),
    ]);
}

impl Dispatch<ExtForeignToplevelHandleV1, ()> for AppState {
    fn event(
        state: &mut Self,
        proxy: &ExtForeignToplevelHandleV1,
        event: ext_foreign_toplevel_handle_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            ext_foreign_toplevel_handle_v1::Event::Identifier { identifier } => {
                state.toplevels.push(DiscoveredToplevel {
                    proxy: proxy.clone(),
                    identifier,
                });
            }
            ext_foreign_toplevel_handle_v1::Event::Closed => {
                let id = proxy.id();
                state.toplevels.retain(|t| t.proxy.id() != id);
            }
            _ => {}
        }
    }
}

impl Dispatch<ExtImageCopyCaptureSessionV1, ()> for AppState {
    fn event(
        state: &mut Self,
        _: &ExtImageCopyCaptureSessionV1,
        event: ext_image_copy_capture_session_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            ext_image_copy_capture_session_v1::Event::BufferSize { width, height } => {
                // BufferSize after the initial Done is a renegotiation (the
                // compositor saw the window resize). Flag it for the io
                // callback to swap in fresh PW params.
                if state.adv_done
                    && (state.negotiated_width != width || state.negotiated_height != height)
                {
                    state.needs_renegotiate = true;
                }
                state.adv_width = width;
                state.adv_height = height;
            }
            ext_image_copy_capture_session_v1::Event::ShmFormat {
                format: WEnum::Value(f),
            } => {
                let is_preferred = matches!(f, wl_shm::Format::Xrgb8888);
                if state.adv_format.is_none() || is_preferred {
                    state.adv_format = Some(f);
                }
            }
            ext_image_copy_capture_session_v1::Event::Done => {
                state.adv_done = true;
            }
            ext_image_copy_capture_session_v1::Event::Stopped => {
                // The window closed (or the compositor stopped the session):
                // wind the stream down cleanly.
                state.session_stopped = true;
                state.dying = true;
                if let Some(pending) = state.pending_frame.take() {
                    pending.frame.destroy();
                }
                if let Some(stop) = state.stop_flag.as_ref() {
                    stop.store(true, Ordering::SeqCst);
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<ExtImageCopyCaptureFrameV1, ()> for AppState {
    fn event(
        state: &mut Self,
        _: &ExtImageCopyCaptureFrameV1,
        event: ext_image_copy_capture_frame_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            ext_image_copy_capture_frame_v1::Event::Ready => state.on_frame_ready(),
            ext_image_copy_capture_frame_v1::Event::Failed { .. } => state.on_frame_failed(),
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
        tracing::info!(?old, ?new, "pw toplevel stream state");
        if matches!(
            new,
            pw::stream::StreamState::Paused | pw::stream::StreamState::Streaming
        ) {
            if let Some(tx) = self.node_id_tx.take() {
                let _ = tx.send(Ok(StreamInfo {
                    node_id: stream.node_id(),
                    width: self.negotiated_width,
                    height: self.negotiated_height,
                }));
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

    /// Push renegotiated PW params after a compositor-side size change; PW
    /// reallocates buffers and add/remove_buffer recreate the wl_buffers.
    fn maybe_renegotiate(&mut self) {
        if !self.needs_renegotiate || self.dying {
            return;
        }
        self.needs_renegotiate = false;
        let new_w = self.adv_width;
        let new_h = self.adv_height;
        self.negotiated_width = new_w;
        self.negotiated_height = new_h;
        if let Some(pending) = self.pending_frame.take() {
            pending.frame.destroy();
        }
        let Some(stream) = self.stream.clone() else {
            return;
        };
        tracing::info!(new_w, new_h, "toplevel: pushing renegotiated PW params");
        match (
            build_video_format_param(new_w, new_h, self.spec.framerate),
            build_buffers_param(new_w, new_h),
        ) {
            (Ok(fbytes), Ok(bbytes)) => {
                if let (Some(fpod), Some(bpod)) =
                    (Pod::from_bytes(&fbytes), Pod::from_bytes(&bbytes))
                {
                    let mut params = [fpod, bpod];
                    if let Err(e) = stream.update_params(&mut params) {
                        tracing::warn!("update_params failed: {e:?}");
                    } else {
                        // Streaming resumes from the state_changed handler
                        // once the new buffers land.
                        self.capture_kicked = false;
                    }
                }
            }
            (e1, e2) => {
                tracing::warn!(?e1, ?e2, "build params failed during renegotiate");
            }
        }
    }

    fn on_add_buffer(&mut self, _stream: &pw::stream::Stream, buffer: *mut pw::sys::pw_buffer) {
        let stride = (self.adv_width * 4) as i32;
        let size = stride as usize * self.adv_height as usize;
        let memfd =
            match rustix::fs::memfd_create("tomoe-portal-pwbuf", rustix::fs::MemfdFlags::CLOEXEC) {
                Ok(fd) => fd,
                Err(e) => {
                    tracing::error!("memfd_create: {e}");
                    return;
                }
            };
        if let Err(e) = rustix::fs::ftruncate(&memfd, size as u64) {
            tracing::error!("ftruncate: {e}");
            return;
        }
        let Some(shm) = self.shm.as_ref() else {
            return;
        };
        let pool = shm.create_pool(memfd.as_fd(), size as i32, &self.qh, ());
        let wl_buf = pool.create_buffer(
            0,
            self.adv_width as i32,
            self.adv_height as i32,
            stride,
            wl_shm::Format::Xrgb8888,
            &self.qh,
            (),
        );

        let fd_for_pw = match memfd.try_clone() {
            Ok(c) => c.into_raw_fd(),
            Err(e) => {
                tracing::error!("dup memfd for pw: {e}");
                return;
            }
        };
        unsafe {
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
            let data = &mut datas[0];
            data.type_ = spa_sys::SPA_DATA_MemFd;
            data.flags = spa_sys::SPA_DATA_FLAG_READWRITE;
            data.fd = fd_for_pw as i64;
            data.data = std::ptr::null_mut();
            data.maxsize = size as u32;
            data.mapoffset = 0;

            let chunk = &mut *data.chunk;
            chunk.offset = 0;
            chunk.stride = stride;
            chunk.size = size as u32;
        }

        let key = buffer as usize;
        self.pw_buffer_stride.insert(key, stride);
        self.pw_buffer_slots.insert(
            key,
            BufferSlot {
                wl_buffer: wl_buf,
                _shm_pool: pool,
                _fd: memfd,
            },
        );
    }

    fn on_remove_buffer(&mut self, _stream: &pw::stream::Stream, buffer: *mut pw::sys::pw_buffer) {
        let key = buffer as usize;
        if let Some(slot) = self.pw_buffer_slots.remove(&key) {
            slot.wl_buffer.destroy();
        }
        self.pw_buffer_stride.remove(&key);
        // Abandon any in-flight frame targeting this buffer so a late Ready
        // doesn't dereference freed PipeWire state (the consumer-disconnect
        // use-after-free path).
        let targets_this = self
            .pending_frame
            .as_ref()
            .is_some_and(|p| p.pw_buffer == key);
        if targets_this {
            let pending = self.pending_frame.take().unwrap();
            pending.frame.destroy();
        }
    }

    /// Dequeue a PW buffer, create a frame on the session, attach the
    /// wl_buffer wrapping the same memfd, and capture. The compositor
    /// renders straight into PipeWire-owned memory and answers with Ready.
    fn kick_capture(&mut self) {
        if self.dying {
            return;
        }
        let Some(session) = self.session.clone() else {
            return;
        };
        let Some(stream) = self.stream.clone() else {
            return;
        };
        let pw_buf = unsafe { stream.dequeue_raw_buffer() };
        if pw_buf.is_null() {
            // Consumer hasn't returned a buffer yet — all slots in flight.
            // Small backoff; the natural cycle re-kicks. A dropped frame.
            tracing::debug!("kick_capture: dequeue_raw_buffer returned null");
            thread::sleep(std::time::Duration::from_millis(2));
            return;
        }
        let key = pw_buf as usize;
        let Some(slot) = self.pw_buffer_slots.get(&key) else {
            tracing::error!("kick_capture: no slot for dequeued pw_buffer {key:#x}");
            unsafe { stream.queue_raw_buffer(pw_buf) };
            return;
        };
        let frame = session.create_frame(&self.qh, ());
        frame.attach_buffer(&slot.wl_buffer);
        frame.capture();
        self.pending_frame = Some(PendingFrame {
            frame,
            pw_buffer: key,
        });
        // Flush so the request reaches the compositor now — the io callback
        // only wakes on incoming bytes.
        if let Some(conn) = self.conn.as_ref() {
            if let Err(e) = conn.flush() {
                tracing::warn!("kick_capture: flush failed: {e}");
            }
        }
    }

    fn on_frame_ready(&mut self) {
        if self.dying {
            return;
        }
        let Some(pending) = self.pending_frame.take() else {
            return;
        };
        pending.frame.destroy();

        // Set the chunk size on the dequeued PW buffer, then queue it (the
        // wake-up for consumers AND the cycle advance for a DRIVER stream).
        // Re-check the buffer still exists — PW may have freed it between
        // dequeue and ready (consumer disconnect path).
        if pending.pw_buffer != 0 && self.pw_buffer_slots.contains_key(&pending.pw_buffer) {
            if let Some(stream) = self.stream.clone() {
                let pw_buf = pending.pw_buffer as *mut pw::sys::pw_buffer;
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
                        let stride = *self.pw_buffer_stride.get(&pending.pw_buffer).unwrap_or(&0);
                        let chunk = &mut *data.chunk;
                        chunk.offset = 0;
                        chunk.stride = stride;
                        chunk.size = (stride as u32) * self.negotiated_height;
                    }
                    stream.queue_raw_buffer(pw_buf);
                }
            }
        }

        self.frames_completed += 1;
        if self.last_log_at.elapsed() >= std::time::Duration::from_secs(2) {
            tracing::info!(
                frames = self.frames_completed,
                "toplevel screencast: frames queued"
            );
            self.last_log_at = std::time::Instant::now();
        }

        self.kick_capture();
    }

    fn on_frame_failed(&mut self) {
        if self.dying {
            return;
        }
        tracing::warn!("toplevel screencast frame failed");
        if let Some(pending) = self.pending_frame.take() {
            pending.frame.destroy();
            if pending.pw_buffer != 0 {
                if let Some(stream) = self.stream.clone() {
                    let pw_buf = pending.pw_buffer as *mut pw::sys::pw_buffer;
                    unsafe { stream.queue_raw_buffer(pw_buf) };
                }
            }
        }
        // Frame failures also cover transient states (e.g. constraints
        // changing); back off briefly and retry.
        thread::sleep(std::time::Duration::from_millis(50));
        self.kick_capture();
    }
}

// ─── POD builders ─────────────────────────────────────────────────────────

fn build_video_format_param(
    width: u32,
    height: u32,
    framerate: u32,
) -> Result<Vec<u8>, Box<dyn std::error::Error + Send + Sync>> {
    let max_framerate = Fraction {
        num: framerate.max(1),
        denom: 1,
    };
    let preferred_framerate = Fraction {
        num: framerate.clamp(1, 60),
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
                Value::Rectangle(Rectangle { width, height }),
            ),
            Property::new(
                spa_sys::SPA_FORMAT_VIDEO_framerate,
                Value::Choice(ChoiceValue::Fraction(Choice(
                    ChoiceFlags::empty(),
                    ChoiceEnum::Range {
                        default: preferred_framerate,
                        min: Fraction { num: 0, denom: 1 },
                        max: max_framerate,
                    },
                ))),
            ),
            Property::new(
                spa_sys::SPA_FORMAT_VIDEO_maxFramerate,
                Value::Choice(ChoiceValue::Fraction(Choice(
                    ChoiceFlags::empty(),
                    ChoiceEnum::Range {
                        default: preferred_framerate,
                        min: Fraction { num: 0, denom: 1 },
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
    width: u32,
    height: u32,
) -> Result<Vec<u8>, Box<dyn std::error::Error + Send + Sync>> {
    let stride = (width * 4) as i32;
    let size = stride * height as i32;
    let memfd_flag = 1 << spa_sys::SPA_DATA_MemFd;
    let obj = Value::Object(Object {
        type_: spa_sys::SPA_TYPE_OBJECT_ParamBuffers,
        id: spa_sys::SPA_PARAM_Buffers,
        properties: vec![
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
            Property::new(spa_sys::SPA_PARAM_BUFFERS_size, Value::Int(size)),
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
        ],
    });
    Ok(PodSerializer::serialize(Cursor::new(Vec::new()), &obj)?
        .0
        .into_inner())
}
