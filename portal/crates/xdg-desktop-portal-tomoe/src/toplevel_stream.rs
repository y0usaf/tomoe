//! Toplevel (single-window) streaming via ext-image-copy-capture-v1.
//!
//! Sibling to [`crate::pipewire_stream`], which streams whole outputs through
//! wlr-screencopy. Same architecture (DRIVER + ALLOC_BUFFERS +
//! wayland-driven queue on a single thread — see that module for the long
//! rationale), with three protocol differences from the output path:
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
//! Buffers are DMA-BUF when the session advertises a dmabuf device and
//! format and the consumer accepts one of its modifiers: the stream offers a
//! modifier-carrying format first (MANDATORY | DONT_FIXATE), test-allocates
//! the consumer's choice with GBM and announces the fixated modifier, then
//! the compositor renders the window straight into the shared BO. Otherwise
//! the stream falls back to memfd/SHM, where the compositor has to read every
//! frame back from the GPU on its own thread.
//!
//! Capture is paced to the consumer-negotiated frame interval: the next
//! frame is requested only once `1 / max_framerate` has passed since the
//! last one, so the compositor renders the window at the stream's rate, not
//! at every output refresh.

use std::cell::RefCell;
use std::collections::HashMap;
use std::fs::File;
use std::io::Cursor;
use std::os::fd::{AsFd, AsRawFd, OwnedFd, RawFd};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
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
use pw::spa::pod::deserialize::PodDeserializer;
use pw::spa::pod::serialize::PodSerializer;
use pw::spa::pod::{ChoiceValue, Object, Pod, PodPropFlags, Property, PropertyFlags, Value};
use pw::spa::support::system::IoFlags;
use pw::spa::utils::{Choice, ChoiceEnum, ChoiceFlags, Fraction, Id, Rectangle};
use spa::sys as spa_sys;
use tokio::sync::oneshot;
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
use wayland_protocols::wp::linux_dmabuf::zv1::client::{
    zwp_linux_buffer_params_v1, zwp_linux_dmabuf_v1,
};

use crate::pipewire_stream::init_gbm_device;

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
}

impl Drop for StreamHandle {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
    }
}

pub struct Started {
    pub handle: StreamHandle,
    pub ready: oneshot::Receiver<Result<StreamInfo, String>>,
    pub ended: oneshot::Receiver<()>,
}

pub fn start(spec: StreamSpec) -> std::io::Result<Started> {
    let stop = Arc::new(AtomicBool::new(false));
    let stop_for_thread = stop.clone();
    let (ready_tx, ready) = oneshot::channel();
    let (ended_tx, ended) = oneshot::channel();
    thread::Builder::new()
        .name("portal-toplevel-cast".into())
        .spawn(move || {
            let _ended = ended_tx;
            if let Err(e) = run(spec, ready_tx, stop_for_thread) {
                tracing::error!("toplevel stream thread exited: {e}");
            }
        })?;
    Ok(Started {
        handle: StreamHandle { stop },
        ready,
        ended,
    })
}

struct AppState {
    spec: StreamSpec,

    conn: Option<Connection>,
    qh: QueueHandle<AppState>,
    shm: Option<wl_shm::WlShm>,
    capture_manager: Option<ExtImageCopyCaptureManagerV1>,
    toplevel_source_manager: Option<ExtForeignToplevelImageCaptureSourceManagerV1>,

    toplevels: Vec<DiscoveredToplevel>,
    target_toplevel: Option<ExtForeignToplevelHandleV1>,

    source: Option<ExtImageCaptureSourceV1>,
    session: Option<ExtImageCopyCaptureSessionV1>,

    adv_width: u32,
    adv_height: u32,
    adv_format: Option<wl_shm::Format>,
    adv_done: bool,
    session_stopped: bool,

    dmabuf: Option<zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1>,
    adv_dmabuf_device: Option<u64>,
    adv_dmabuf_formats: Vec<(u32, Vec<u64>)>,
    /// DMA-BUF format and modifiers to offer; `None` keeps the stream on SHM.
    dmabuf_caps: Option<DmabufCaps>,
    gbm: Option<GbmDevice<File>>,
    /// What add_buffer allocates, set from the negotiated PipeWire format.
    buffer_kind: BufferKind,

    stream: Option<pw::stream::StreamRc>,
    /// pw_buffer → its wrapping wl_buffer + backing storage.
    pw_buffer_slots: HashMap<PwBuf, BufferSlot>,

    pending_frame: Option<PendingFrame>,

    node_id_tx: Option<oneshot::Sender<Result<StreamInfo, String>>>,

    frames_completed: u64,
    last_log_at: std::time::Instant,
    streaming: bool,
    retry_at: Option<Instant>,
    /// When the current run of failed frames began, and its length.
    failing_since: Option<Instant>,
    failed_frames: u32,

    dying: bool,
    stop_flag: Option<Arc<AtomicBool>>,

    needs_renegotiate: bool,
    negotiated_width: u32,
    negotiated_height: u32,

    /// Consumer-negotiated frame interval; zero leaves capture unpaced.
    min_time_between_frames: Duration,
    last_capture_at: Option<Instant>,
}

struct DmabufCaps {
    /// Format of the wl_buffer, the one the compositor advertised.
    fourcc: DrmFourcc,
    /// PipeWire format announced for the same memory: the opaque variant, so
    /// consumers ignore alpha as they do on the SHM path.
    spa_format: u32,
    modifiers: Vec<u64>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum BufferKind {
    Shm,
    Dmabuf { modifier: u64, planes: u32 },
}

struct DiscoveredToplevel {
    proxy: ExtForeignToplevelHandleV1,
    identifier: String,
}

struct PendingFrame {
    frame: ExtImageCopyCaptureFrameV1,
    pw_buffer: Option<PwBuf>,
}

struct BufferSlot {
    wl_buffer: wl_buffer::WlBuffer,
    /// One entry per PipeWire data block, rewritten into its chunk on queue.
    planes: Vec<Plane>,
    _storage: SlotStorage,
}

#[derive(Clone, Copy)]
struct Plane {
    offset: u32,
    stride: i32,
    size: u32,
}

/// Owns everything PipeWire and the compositor see through the slot; the fds
/// handed to PipeWire are these, so they close when the slot is removed.
enum SlotStorage {
    Shm {
        _pool: wl_shm_pool::WlShmPool,
        _fd: OwnedFd,
    },
    Dmabuf {
        _bo: BufferObject<()>,
        _fds: Vec<OwnedFd>,
    },
}

struct NewSlot {
    slot: BufferSlot,
    fds: Vec<RawFd>,
    data_type: spa_sys::spa_data_type,
}

/// Identity key for a PipeWire `pw_buffer` used in the slot maps. The pointer
/// is *never* dereferenced through this wrapper; it is a stable identity test
/// against the same underlying object that the add/remove_buffer callbacks and
/// `dequeue_raw_buffer` hand out. The pointer is valid only for as long as the
/// matching [`BufferSlot`] is present in `pw_buffer_slots`, and is only ever
/// handed back to PipeWire verbatim via `queue_raw_buffer` inside that window.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct PwBuf(*mut pw::sys::pw_buffer);

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

fn run(
    spec: StreamSpec,
    node_id_tx: oneshot::Sender<Result<StreamInfo, String>>,
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
        dmabuf: None,
        adv_dmabuf_device: None,
        adv_dmabuf_formats: Vec::new(),
        dmabuf_caps: None,
        gbm: None,
        buffer_kind: BufferKind::Shm,
        stream: None,
        pw_buffer_slots: HashMap::new(),
        pending_frame: None,
        node_id_tx: Some(node_id_tx),
        frames_completed: 0,
        last_log_at: std::time::Instant::now(),
        streaming: false,
        retry_at: None,
        failing_since: None,
        failed_frames: 0,
        dying: false,
        stop_flag: Some(stop.clone()),
        needs_renegotiate: false,
        negotiated_width: 0,
        negotiated_height: 0,
        min_time_between_frames: Duration::ZERO,
        last_capture_at: None,
    };

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
        tracing::debug!(
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
    state.settle_dmabuf();

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
    let s_param = state_rc.clone();
    let _listener = stream
        .add_local_listener_with_user_data(())
        .state_changed(move |stream, _ud, old, new| {
            s_state.borrow_mut().on_state_changed(stream, old, new);
        })
        .param_changed(move |stream, _ud, id, pod| {
            if ParamType::from_raw(id) != ParamType::Format {
                return;
            }
            if let Some(pod) = pod {
                s_param.borrow_mut().on_format(stream, pod);
            }
        })
        .add_buffer(move |_stream, _ud, buffer| {
            s_add.borrow_mut().on_add_buffer(buffer);
        })
        .remove_buffer(move |stream, _ud, buffer| {
            s_remove.borrow_mut().on_remove_buffer(stream, buffer);
        })
        .process(|_, _| {})
        .register()?;

    let format_bytes = state_rc.borrow().format_params(None)?;
    let mut params = format_bytes
        .iter()
        .map(|bytes| Pod::from_bytes(bytes).ok_or("format POD parse failed".to_string()))
        .collect::<Result<Vec<_>, _>>()?;
    stream.connect(
        spa::utils::Direction::Output,
        None,
        pw::stream::StreamFlags::DRIVER | pw::stream::StreamFlags::ALLOC_BUFFERS,
        &mut params,
    )?;
    tracing::info!("toplevel screencast: PW stream connected (DRIVER | ALLOC_BUFFERS)");

    let wl_fd = conn.as_fd().try_clone_to_owned()?;
    let fd_holder = FdHolder(wl_fd);

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

    let mainloop_for_stop = mainloop.clone();
    let stop_for_event = stop.clone();
    let s_for_tick = state_rc.clone();
    let tick = mainloop.loop_().add_timer(move |_| {
        if stop_for_event.load(Ordering::SeqCst) {
            mainloop_for_stop.quit();
            return;
        }
        s_for_tick.borrow_mut().on_tick();
    });
    let _ = tick.update_timer(Some(TICK), Some(TICK));

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
            "zwp_linux_dmabuf_v1" if version >= 2 => {
                state.dmabuf = Some(
                    registry.bind::<zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1, _, _>(
                        name,
                        version.min(4),
                        qh,
                        (),
                    ),
                );
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
empty_dispatch!(zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1);

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
            tracing::warn!("toplevel screencast: DMA-BUF wl_buffer creation failed");
        }
    }

    wayland_client::event_created_child!(AppState, zwp_linux_buffer_params_v1::ZwpLinuxBufferParamsV1, [
        zwp_linux_buffer_params_v1::EVT_CREATED_OPCODE => (wl_buffer::WlBuffer, ())
    ]);
}

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
            ext_image_copy_capture_session_v1::Event::DmabufDevice { device } => {
                state.adv_dmabuf_device = device.try_into().ok().map(u64::from_ne_bytes);
            }
            ext_image_copy_capture_session_v1::Event::DmabufFormat { format, modifiers } => {
                let modifiers = modifiers
                    .as_chunks::<8>()
                    .0
                    .iter()
                    .map(|m| u64::from_ne_bytes(*m))
                    .collect();
                state.adv_dmabuf_formats.retain(|(f, _)| *f != format);
                state.adv_dmabuf_formats.push((format, modifiers));
            }
            ext_image_copy_capture_session_v1::Event::Done => {
                state.adv_done = true;
            }
            ext_image_copy_capture_session_v1::Event::Stopped => {
                state.session_stopped = true;
                state.dying = true;
                state.drop_pending_frame();
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
        self.streaming = matches!(new, pw::stream::StreamState::Streaming);
        if self.streaming && self.pending_frame.is_none() {
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
    /// renegotiates the format, \`on_format\` answers with buffer params and
    /// add/remove_buffer recreate the wl_buffers.
    fn maybe_renegotiate(&mut self) {
        if !self.needs_renegotiate || self.dying {
            return;
        }
        self.needs_renegotiate = false;
        self.negotiated_width = self.adv_width;
        self.negotiated_height = self.adv_height;
        self.drop_pending_frame();
        let Some(stream) = self.stream.clone() else {
            return;
        };
        tracing::info!(
            new_w = self.negotiated_width,
            new_h = self.negotiated_height,
            "toplevel: pushing renegotiated PW params"
        );
        self.push_formats(&stream, None);
    }

    /// Decide once, from the session's constraints, whether DMA-BUF can be
    /// offered: the compositor must name a device and a format we can
    /// describe to PipeWire, and GBM must open a render node for it.
    fn settle_dmabuf(&mut self) {
        if self.dmabuf.is_none() {
            tracing::info!("toplevel screencast: zwp_linux_dmabuf_v1 missing; using SHM capture");
            return;
        }
        let Some((fourcc, spa_format, modifiers)) =
            self.adv_dmabuf_formats
                .iter()
                .find_map(|(format, modifiers)| {
                    let fourcc = DrmFourcc::try_from(*format).ok()?;
                    let modifiers: Vec<u64> = modifiers
                        .iter()
                        .copied()
                        .filter(|&m| m != u64::from(DrmModifier::Invalid))
                        .collect();
                    (!modifiers.is_empty()).then_some((
                        fourcc,
                        opaque_spa_format(fourcc)?,
                        modifiers,
                    ))
                })
        else {
            tracing::info!(
                formats = ?self.adv_dmabuf_formats,
                "toplevel screencast: no streamable DMA-BUF format offered; using SHM capture"
            );
            return;
        };
        match init_gbm_device(self.adv_dmabuf_device) {
            Ok(Some(gbm)) => {
                tracing::info!(
                    ?fourcc,
                    modifiers = ?modifiers.iter().map(|m| format!("{m:#x}")).collect::<Vec<_>>(),
                    "toplevel screencast: offering DMA-BUF capture"
                );
                self.gbm = Some(gbm);
                self.dmabuf_caps = Some(DmabufCaps {
                    fourcc,
                    spa_format,
                    modifiers,
                });
            }
            Ok(None) => {
                tracing::info!("toplevel screencast: no render node found; using SHM capture")
            }
            Err(e) => {
                tracing::warn!("toplevel screencast: GBM init failed ({e}); using SHM capture")
            }
        }
    }

    /// Stop offering DMA-BUF and renegotiate onto SHM.
    fn abandon_dmabuf(&mut self, stream: &pw::stream::Stream, why: &str) {
        tracing::warn!("toplevel screencast: {why}; falling back to SHM capture");
        self.dmabuf_caps = None;
        self.gbm = None;
        self.push_formats(stream, None);
    }

    /// Allocate a test BO with one of \`modifiers\` (those we also offered)
    /// and report the modifier GBM picked and its plane count.
    fn probe_modifier(&self, modifiers: &[u64]) -> Option<(u64, u32)> {
        let (caps, gbm) = (self.dmabuf_caps.as_ref()?, self.gbm.as_ref()?);
        let wanted: Vec<DrmModifier> = modifiers
            .iter()
            .copied()
            .filter(|m| caps.modifiers.contains(m))
            .map(DrmModifier::from)
            .collect();
        if wanted.is_empty() {
            return None;
        }
        match gbm.create_buffer_object_with_modifiers2::<()>(
            self.negotiated_width,
            self.negotiated_height,
            caps.fourcc,
            wanted.into_iter(),
            BufferObjectFlags::RENDERING,
        ) {
            Ok(bo) => Some((u64::from(bo.modifier()), bo.plane_count())),
            Err(e) => {
                tracing::warn!("toplevel screencast: GBM test allocation failed: {e}");
                None
            }
        }
    }

    /// The consumer settled a format. Without a modifier it is SHM; with an
    /// unfixated modifier choice we pick one by test allocation and announce
    /// it; with a fixed modifier we ask for DMA-BUF buffers.
    fn on_format(&mut self, stream: &pw::stream::Stream, pod: &Pod) {
        if !matches!(parse_format(pod), Ok((MediaType::Video, MediaSubtype::Raw))) {
            return;
        }
        let mut info = VideoInfoRaw::new();
        if info.parse(pod).is_err() {
            return;
        }
        let rate = Some(info.max_framerate())
            .filter(|f| f.num > 0)
            .or(Some(info.framerate()).filter(|f| f.num > 0));
        self.min_time_between_frames = rate.map_or(Duration::ZERO, |f| {
            Duration::from_micros(1_000_000 * u64::from(f.denom) / u64::from(f.num))
        });
        tracing::info!(
            ?rate,
            interval = ?self.min_time_between_frames,
            "toplevel screencast: consumer-negotiated frame pacing"
        );

        let modifier_prop = pod.as_object().ok().and_then(|object| {
            object.find_prop(spa::utils::Id(spa_sys::SPA_FORMAT_VIDEO_modifier))
        });
        let Some(prop) = modifier_prop else {
            self.buffer_kind = BufferKind::Shm;
            self.push_buffers(stream);
            return;
        };
        if prop.flags().contains(PodPropFlags::DONT_FIXATE) {
            match self.probe_modifier(&long_values(prop.value())) {
                Some((modifier, _)) => {
                    tracing::info!(
                        modifier = format_args!("{modifier:#x}"),
                        "toplevel screencast: fixating DMA-BUF modifier"
                    );
                    self.push_formats(stream, Some(modifier));
                }
                None => self.abandon_dmabuf(stream, "no offered modifier allocates"),
            }
            return;
        }
        let modifier = info.modifier();
        match self.probe_modifier(&[modifier]) {
            Some((_, planes)) => {
                tracing::info!(
                    modifier = format_args!("{modifier:#x}"),
                    planes,
                    "toplevel screencast: DMA-BUF modifier negotiated"
                );
                self.buffer_kind = BufferKind::Dmabuf { modifier, planes };
                self.push_buffers(stream);
            }
            None => self.abandon_dmabuf(stream, "negotiated modifier does not allocate"),
        }
    }

    /// EnumFormat params, preferred first: a fixated DMA-BUF format, the
    /// DMA-BUF modifier list, then SHM.
    fn format_params(
        &self,
        fixed: Option<u64>,
    ) -> Result<Vec<Vec<u8>>, Box<dyn std::error::Error + Send + Sync>> {
        let (w, h, rate) = (
            self.negotiated_width,
            self.negotiated_height,
            self.spec.framerate,
        );
        let mut params = Vec::new();
        if let Some(caps) = self.dmabuf_caps.as_ref() {
            if let Some(modifier) = fixed {
                params.push(build_video_format_param(
                    w,
                    h,
                    rate,
                    caps.spa_format,
                    &[modifier],
                )?);
            }
            if fixed.is_none() || caps.modifiers.len() > 1 {
                params.push(build_video_format_param(
                    w,
                    h,
                    rate,
                    caps.spa_format,
                    &caps.modifiers,
                )?);
            }
        }
        params.push(build_video_format_param(
            w,
            h,
            rate,
            spa_sys::SPA_VIDEO_FORMAT_BGRx,
            &[],
        )?);
        Ok(params)
    }

    fn push_formats(&self, stream: &pw::stream::Stream, fixed: Option<u64>) {
        match self.format_params(fixed) {
            Ok(params) => update_params(stream, &params),
            Err(e) => tracing::warn!("build format params: {e}"),
        }
    }

    fn push_buffers(&self, stream: &pw::stream::Stream) {
        match build_buffers_param(
            self.negotiated_width,
            self.negotiated_height,
            self.buffer_kind,
        ) {
            Ok(param) => update_params(stream, &[param]),
            Err(e) => tracing::warn!("build buffers param: {e}"),
        }
    }

    fn create_shm_slot(&self) -> Result<NewSlot, String> {
        let (width, height) = (self.negotiated_width, self.negotiated_height);
        let stride = xrgb8888_stride(width)?;
        let size = payload_size(stride, height)?;
        let memfd = rustix::fs::memfd_create("tomoe-portal-pwbuf", rustix::fs::MemfdFlags::CLOEXEC)
            .map_err(|e| format!("memfd_create: {e}"))?;
        rustix::fs::ftruncate(&memfd, size as u64).map_err(|e| format!("ftruncate: {e}"))?;
        let shm = self.shm.as_ref().ok_or("wl_shm is not bound")?;
        let pool = shm.create_pool(memfd.as_fd(), size as i32, &self.qh, ());
        let wl_buffer = pool.create_buffer(
            0,
            width as i32,
            height as i32,
            stride,
            wl_shm::Format::Xrgb8888,
            &self.qh,
            (),
        );
        let fds = vec![memfd.as_raw_fd()];
        Ok(NewSlot {
            slot: BufferSlot {
                wl_buffer,
                planes: vec![Plane {
                    offset: 0,
                    stride,
                    size: size as u32,
                }],
                _storage: SlotStorage::Shm {
                    _pool: pool,
                    _fd: memfd,
                },
            },
            fds,
            data_type: spa_sys::SPA_DATA_MemFd,
        })
    }

    fn create_dmabuf_slot(&self, modifier: u64) -> Result<NewSlot, String> {
        let (Some(caps), Some(gbm), Some(dmabuf)) = (
            self.dmabuf_caps.as_ref(),
            self.gbm.as_ref(),
            self.dmabuf.as_ref(),
        ) else {
            return Err("DMA-BUF negotiated without a GBM device".into());
        };
        let (width, height) = (self.negotiated_width, self.negotiated_height);
        let bo = gbm
            .create_buffer_object_with_modifiers2::<()>(
                width,
                height,
                caps.fourcc,
                std::iter::once(DrmModifier::from(modifier)),
                BufferObjectFlags::RENDERING,
            )
            .map_err(|e| format!("GBM allocation: {e}"))?;
        let mut fds = Vec::new();
        let mut planes = Vec::new();
        for plane in 0..bo.plane_count() as i32 {
            let fd = bo
                .fd_for_plane(plane)
                .map_err(|e| format!("export plane {plane}: {e}"))?;
            let stride = i32::try_from(bo.stride_for_plane(plane))
                .map_err(|_| format!("plane {plane} stride exceeds i32"))?;
            planes.push(Plane {
                offset: bo.offset(plane),
                stride,
                size: payload_size(stride, height)? as u32,
            });
            fds.push(fd);
        }
        let params = dmabuf.create_params(&self.qh, ());
        for (index, (fd, plane)) in fds.iter().zip(&planes).enumerate() {
            params.add(
                fd.as_fd(),
                index as u32,
                plane.offset,
                plane.stride as u32,
                (modifier >> 32) as u32,
                (modifier & 0xffff_ffff) as u32,
            );
        }
        let wl_buffer = params.create_immed(
            width as i32,
            height as i32,
            caps.fourcc as u32,
            zwp_linux_buffer_params_v1::Flags::empty(),
            &self.qh,
            (),
        );
        params.destroy();
        let raw_fds = fds.iter().map(|fd| fd.as_raw_fd()).collect();
        Ok(NewSlot {
            slot: BufferSlot {
                wl_buffer,
                planes,
                _storage: SlotStorage::Dmabuf { _bo: bo, _fds: fds },
            },
            fds: raw_fds,
            data_type: spa_sys::SPA_DATA_DmaBuf,
        })
    }

    fn on_add_buffer(&mut self, buffer: *mut pw::sys::pw_buffer) {
        let made = match self.buffer_kind {
            BufferKind::Shm => self.create_shm_slot(),
            BufferKind::Dmabuf { modifier, .. } => self.create_dmabuf_slot(modifier),
        };
        let NewSlot {
            slot,
            fds,
            data_type,
        } = match made {
            Ok(new) => new,
            Err(e) => {
                tracing::error!(kind = ?self.buffer_kind, "toplevel: create buffer: {e}");
                return;
            }
        };
        unsafe {
            let buf = (*buffer).buffer;
            let datas = if buf.is_null() {
                &mut [][..]
            } else {
                std::slice::from_raw_parts_mut((*buf).datas, (*buf).n_datas as usize)
            };
            if datas.len() < slot.planes.len() {
                tracing::error!(
                    blocks = datas.len(),
                    planes = slot.planes.len(),
                    "on_add_buffer: pw_buffer has too few data blocks"
                );
                slot.wl_buffer.destroy();
                return;
            }
            for ((data, plane), fd) in datas.iter_mut().zip(&slot.planes).zip(&fds) {
                data.type_ = data_type;
                data.flags = spa_sys::SPA_DATA_FLAG_READWRITE;
                data.fd = i64::from(*fd);
                data.data = std::ptr::null_mut();
                data.maxsize = plane.size;
                data.mapoffset = 0;
                write_chunk(data, plane);
            }
        }
        self.pw_buffer_slots.insert(PwBuf(buffer), slot);
    }

    fn on_remove_buffer(&mut self, _stream: &pw::stream::Stream, buffer: *mut pw::sys::pw_buffer) {
        let key = PwBuf(buffer);
        if let Some(slot) = self.pw_buffer_slots.remove(&key) {
            slot.wl_buffer.destroy();
        }
        let targets_this = self
            .pending_frame
            .as_ref()
            .is_some_and(|p| p.pw_buffer == Some(key));
        if targets_this {
            let pending = self.pending_frame.take().unwrap();
            pending.frame.destroy();
        }
    }

    /// Dequeue a PW buffer, create a frame on the session, attach the
    /// wl_buffer wrapping the same memory, and capture. The compositor
    /// renders straight into PipeWire-owned memory and answers with Ready.
    fn kick_capture(&mut self) {
        if self.dying || !self.streaming {
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
            tracing::debug!("kick_capture: dequeue_raw_buffer returned null");
            return;
        }
        let key = PwBuf(pw_buf);
        let Some(slot) = self.pw_buffer_slots.get(&key) else {
            tracing::error!("kick_capture: no slot for dequeued pw_buffer");
            unsafe { return_buffer(&stream, pw_buf) };
            return;
        };
        let frame = session.create_frame(&self.qh, ());
        frame.attach_buffer(&slot.wl_buffer);
        frame.capture();
        self.last_capture_at = Some(Instant::now());
        self.pending_frame = Some(PendingFrame {
            frame,
            pw_buffer: Some(key),
        });
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
        self.failing_since = None;
        self.failed_frames = 0;

        if let Some(key) = pending.pw_buffer {
            if let (Some(slot), Some(stream)) =
                (self.pw_buffer_slots.get(&key), self.stream.clone())
            {
                let pw_buf = key.0;
                unsafe {
                    if !pw_buf.is_null() && !(*pw_buf).buffer.is_null() {
                        let datas = std::slice::from_raw_parts_mut(
                            (*(*pw_buf).buffer).datas,
                            (*(*pw_buf).buffer).n_datas as usize,
                        );
                        for (data, plane) in datas.iter_mut().zip(&slot.planes) {
                            write_chunk(data, plane);
                        }
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

        if let Some(remaining) = self.until_next_capture() {
            if remaining >= CAST_DELAY_ALLOWANCE {
                thread::sleep(remaining);
            }
        }
        self.kick_capture();
    }

    /// Time left in the consumer-negotiated frame interval since the last
    /// capture request, if any.
    fn until_next_capture(&self) -> Option<Duration> {
        let next = self.last_capture_at? + self.min_time_between_frames;
        Some(next.saturating_duration_since(Instant::now())).filter(|d| !d.is_zero())
    }

    /// Destroy the in-flight frame and hand its buffer back to PipeWire.
    fn drop_pending_frame(&mut self) {
        let Some(pending) = self.pending_frame.take() else {
            return;
        };
        pending.frame.destroy();
        if let (Some(pw_buf), Some(stream)) = (pending.pw_buffer, self.stream.as_ref()) {
            unsafe { return_buffer(stream, pw_buf.0) };
        }
    }

    fn end(&mut self, why: &str) {
        if self.dying {
            return;
        }
        tracing::info!(why, "toplevel screencast: ending the stream");
        self.dying = true;
        self.drop_pending_frame();
        if let Some(stop) = self.stop_flag.as_ref() {
            stop.store(true, Ordering::SeqCst);
        }
    }

    /// A failed DMA-BUF frame may be the compositor refusing the buffer, so a
    /// short run of them drops to SHM; failures that outlast FAILURE_BUDGET
    /// end the stream instead of retrying forever.
    fn on_frame_failed(&mut self) {
        if self.dying {
            return;
        }
        tracing::warn!("toplevel screencast frame failed");
        self.drop_pending_frame();
        self.failed_frames += 1;
        let on_dmabuf =
            self.dmabuf_caps.is_some() && matches!(self.buffer_kind, BufferKind::Dmabuf { .. });
        if on_dmabuf && self.failed_frames >= DMABUF_FAILURE_LIMIT {
            self.failing_since = None;
            self.failed_frames = 0;
            if let Some(stream) = self.stream.clone() {
                self.abandon_dmabuf(&stream, "DMA-BUF frames kept failing");
            }
        } else if self
            .failing_since
            .get_or_insert_with(Instant::now)
            .elapsed()
            > FAILURE_BUDGET
        {
            self.end("its captures kept failing");
            return;
        }
        self.retry_at = Some(Instant::now() + RETRY_DELAY);
    }

    fn on_tick(&mut self) {
        if self.pending_frame.is_some()
            || self.retry_at.is_some_and(|at| Instant::now() < at)
            || self.until_next_capture().is_some()
        {
            return;
        }
        self.retry_at = None;
        self.kick_capture();
    }
}

const TICK: Duration = Duration::from_millis(10);
const RETRY_DELAY: Duration = Duration::from_millis(50);
const FAILURE_BUDGET: Duration = Duration::from_secs(2);
/// Consecutive failed DMA-BUF frames before falling back to SHM.
const DMABUF_FAILURE_LIMIT: u32 = 3;
const CAST_DELAY_ALLOWANCE: Duration = Duration::from_micros(100);

unsafe fn return_buffer(stream: &pw::stream::Stream, buffer: *mut pw::sys::pw_buffer) {
    pw::sys::pw_stream_return_buffer(stream.as_raw_ptr(), buffer);
}

/// # Safety
/// `data.chunk` must point at the chunk PipeWire allocated for this block.
unsafe fn write_chunk(data: &mut spa_sys::spa_data, plane: &Plane) {
    let chunk = &mut *data.chunk;
    chunk.offset = plane.offset;
    chunk.stride = plane.stride;
    chunk.size = plane.size;
}

fn update_params(stream: &pw::stream::Stream, params: &[Vec<u8>]) {
    let mut pods: Vec<&Pod> = params.iter().filter_map(|b| Pod::from_bytes(b)).collect();
    if let Err(e) = stream.update_params(&mut pods) {
        tracing::warn!("update_params failed: {e:?}");
    }
}

/// The PipeWire format for a DRM format's memory with alpha ignored.
fn opaque_spa_format(fourcc: DrmFourcc) -> Option<u32> {
    match fourcc {
        DrmFourcc::Argb8888 | DrmFourcc::Xrgb8888 => Some(spa_sys::SPA_VIDEO_FORMAT_BGRx),
        DrmFourcc::Abgr8888 | DrmFourcc::Xbgr8888 => Some(spa_sys::SPA_VIDEO_FORMAT_RGBx),
        _ => None,
    }
}

/// Every value of a Long or Long-choice pod (the modifier property).
fn long_values(pod: &Pod) -> Vec<u64> {
    let values = match PodDeserializer::deserialize_any_from(pod.as_bytes()) {
        Ok((_, Value::Long(value))) => vec![value],
        Ok((_, Value::Choice(ChoiceValue::Long(Choice(_, choice))))) => match choice {
            ChoiceEnum::None(value) => vec![value],
            ChoiceEnum::Enum {
                default,
                alternatives,
            } => std::iter::once(default).chain(alternatives).collect(),
            _ => Vec::new(),
        },
        _ => Vec::new(),
    };
    values.into_iter().map(|m| m as u64).collect()
}

/// XRGB8888 stride for a width, in checked arithmetic. Widths come from the
/// compositor's advertised session constraints (untrusted); reject any that
/// can't be expressed as an `i32` stride (the `spa_chunk.stride` field width)
/// before any truncating cast.
fn xrgb8888_stride(width: u32) -> Result<i32, String> {
    let bytes = width
        .checked_mul(4)
        .ok_or_else(|| format!("toplevel stride overflow: {width} * 4"))?;
    i32::try_from(bytes).map_err(|_| format!("toplevel stride {bytes} exceeds i32"))
}

/// Checked payload: stride × height payload per the buffer backing store. The
/// result is bounded to `i32::MAX` so it is provably representable in the `u32`
/// `spa_data.maxsize` / `spa_chunk.size` fields and the `i32`
/// `SPA_PARAM_BUFFERS_size` pod-value — no truncating / wrapping casts later.
fn payload_size(stride: i32, height: u32) -> Result<usize, String> {
    let stride = usize::try_from(stride).map_err(|_| "negative buffer stride".to_string())?;
    let height = usize::try_from(height).map_err(|_| "height overflows usize".to_string())?;
    let size = stride
        .checked_mul(height)
        .ok_or_else(|| format!("toplevel payload overflow: {stride} * {height}"))?;
    if size > i32::MAX as usize {
        return Err(format!("toplevel payload {size} exceeds i32 maxsize"));
    }
    Ok(size)
}

/// One EnumFormat. `modifiers` empty: SHM; one: that DMA-BUF modifier;
/// several: a DMA-BUF choice the consumer narrows and we fixate.
fn build_video_format_param(
    width: u32,
    height: u32,
    framerate: u32,
    spa_format: u32,
    modifiers: &[u64],
) -> Result<Vec<u8>, Box<dyn std::error::Error + Send + Sync>> {
    let max_framerate = Fraction {
        num: framerate.max(1),
        denom: 1,
    };
    let preferred_framerate = Fraction {
        num: framerate.clamp(1, 60),
        denom: 1,
    };
    let mut properties = vec![
        Property::new(
            spa_sys::SPA_FORMAT_mediaType,
            Value::Id(Id(spa_sys::SPA_MEDIA_TYPE_video)),
        ),
        Property::new(
            spa_sys::SPA_FORMAT_mediaSubtype,
            Value::Id(Id(spa_sys::SPA_MEDIA_SUBTYPE_raw)),
        ),
        Property::new(spa_sys::SPA_FORMAT_VIDEO_format, Value::Id(Id(spa_format))),
    ];
    match modifiers {
        [] => {}
        [modifier] => properties.push(Property {
            key: spa_sys::SPA_FORMAT_VIDEO_modifier,
            flags: PropertyFlags::MANDATORY,
            value: Value::Long(*modifier as i64),
        }),
        [first, ..] => properties.push(Property {
            key: spa_sys::SPA_FORMAT_VIDEO_modifier,
            flags: PropertyFlags::MANDATORY | PropertyFlags::DONT_FIXATE,
            value: Value::Choice(ChoiceValue::Long(Choice(
                ChoiceFlags::empty(),
                ChoiceEnum::Enum {
                    default: *first as i64,
                    alternatives: modifiers.iter().map(|&m| m as i64).collect(),
                },
            ))),
        }),
    }
    let obj = Value::Object(Object {
        type_: spa_sys::SPA_TYPE_OBJECT_Format,
        id: spa_sys::SPA_PARAM_EnumFormat,
        properties: properties
            .into_iter()
            .chain([
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
            ])
            .collect(),
    });
    Ok(PodSerializer::serialize(Cursor::new(Vec::new()), &obj)?
        .0
        .into_inner())
}

fn build_buffers_param(
    width: u32,
    height: u32,
    kind: BufferKind,
) -> Result<Vec<u8>, Box<dyn std::error::Error + Send + Sync>> {
    let data_type_choice = |flag: i32| {
        Property::new(
            spa_sys::SPA_PARAM_BUFFERS_dataType,
            Value::Choice(ChoiceValue::Int(Choice(
                ChoiceFlags::empty(),
                ChoiceEnum::Flags {
                    default: flag,
                    flags: vec![flag],
                },
            ))),
        )
    };
    let mut properties = vec![Property::new(
        spa_sys::SPA_PARAM_BUFFERS_buffers,
        Value::Choice(ChoiceValue::Int(Choice(
            ChoiceFlags::empty(),
            ChoiceEnum::Range {
                default: 8,
                min: 2,
                max: 16,
            },
        ))),
    )];
    match kind {
        BufferKind::Shm => {
            let stride = xrgb8888_stride(width)?;
            let size = payload_size(stride, height)?;
            properties.extend([
                Property::new(spa_sys::SPA_PARAM_BUFFERS_blocks, Value::Int(1)),
                Property::new(spa_sys::SPA_PARAM_BUFFERS_size, Value::Int(size as i32)),
                Property::new(spa_sys::SPA_PARAM_BUFFERS_stride, Value::Int(stride)),
                data_type_choice(1 << spa_sys::SPA_DATA_MemFd),
            ]);
        }
        BufferKind::Dmabuf { planes, .. } => {
            let blocks = i32::try_from(planes).map_err(|_| "plane count exceeds i32")?;
            properties.extend([
                Property::new(spa_sys::SPA_PARAM_BUFFERS_blocks, Value::Int(blocks)),
                data_type_choice(1 << spa_sys::SPA_DATA_DmaBuf),
            ]);
        }
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
