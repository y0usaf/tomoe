//! TTY backend: DRM/GBM output + libinput input through a libseat session.
//!
//! Single GPU, all connected connectors at the mode chosen by
//! `settings.mode` ("preferred" or "max"). Rendering is
//! damage-driven through a per-output redraw state machine (niri-style):
//! nothing repaints unless `queue_redraw` was called, and an output with a
//! frame in flight coalesces further requests until its vblank.

use std::collections::HashMap;
use std::mem;
use std::time::Duration;

use anyhow::{anyhow, bail, Context, Result};
use smithay::backend::allocator::gbm::{GbmAllocator, GbmBufferFlags, GbmDevice};
use smithay::backend::allocator::Fourcc;
use smithay::backend::drm::compositor::{DrmCompositor, FrameFlags, PrimaryPlaneElement};
use smithay::backend::drm::exporter::gbm::GbmFramebufferExporter;
use smithay::backend::drm::{DrmDevice, DrmDeviceFd, DrmEvent, DrmNode};
use smithay::backend::egl::{EGLContext, EGLDisplay};
use smithay::backend::libinput::{LibinputInputBackend, LibinputSessionInterface};
use smithay::backend::renderer::element::solid::{SolidColorBuffer, SolidColorRenderElement};
use smithay::backend::renderer::element::surface::render_elements_from_surface_tree;
use smithay::backend::renderer::element::Kind;
use smithay::backend::renderer::gles::GlesRenderer;
use smithay::backend::renderer::{ImportDma, ImportEgl};
use smithay::desktop::layer_map_for_output;
use smithay::input::pointer::{CursorImageStatus, CursorImageSurfaceData};
use smithay::wayland::compositor::with_states;
use smithay::wayland::drm_syncobj::{supports_syncobj_eventfd, DrmSyncobjState};
use smithay::backend::session::libseat::LibSeatSession;
use smithay::backend::session::{Event as SessionEvent, Session};
use smithay::backend::udev::{self, UdevBackend, UdevEvent};
use smithay::output::{Mode, Output, PhysicalProperties, Subpixel};
use smithay::reexports::calloop::timer::{TimeoutAction, Timer};
use smithay::reexports::calloop::{LoopHandle, RegistrationToken};
use smithay::reexports::drm::control::{
    connector, crtc, Mode as DrmMode, ModeFlags, ModeTypeFlags,
};
use smithay::reexports::input::Libinput;
use smithay::reexports::rustix::fs::OFlags;
use smithay::utils::{DeviceFd, IsAlive};
use tracing::{debug, info, warn};

use crate::backend::Backend;
use crate::lua::{DisplaySettings, RefreshSetting, Resolution, SizeSetting};
use crate::render::OutputRenderElements;
use crate::space::PhysicalSpace;
use crate::state::Takhti;

const SUPPORTED_COLOR_FORMATS: [Fourcc; 4] = [
    Fourcc::Argb8888,
    Fourcc::Xrgb8888,
    Fourcc::Abgr8888,
    Fourcc::Xbgr8888,
];

const CLEAR_COLOR: [f32; 4] = [0.05, 0.05, 0.05, 1.0];

pub type GbmDrmCompositor = DrmCompositor<
    GbmAllocator<DrmDeviceFd>,
    GbmFramebufferExporter<DrmDeviceFd>,
    (),
    DrmDeviceFd,
>;

/// Per-output redraw state machine (spec: niri's Development:-Redraw-Loop).
/// Invariant: at most one repaint is in flight per output at any time.
#[derive(Debug, Default)]
pub enum RedrawState {
    /// Nothing scheduled; the output repaints only on the next `queue_redraw`.
    #[default]
    Idle,
    /// A repaint is scheduled as an event-loop idle callback.
    Queued,
    /// A frame was queued to DRM; awaiting the vblank that presents it.
    WaitingForVBlank { redraw_needed: bool },
    /// Last render had no damage, so nothing was queued to DRM; a timer
    /// approximates the missed vblank to keep frame-callback pacing.
    WaitingForEstimatedVBlank(RegistrationToken),
    /// Same, but a redraw was requested while waiting.
    WaitingForEstimatedVBlankAndQueued(RegistrationToken),
}

pub struct TtySurface {
    pub compositor: GbmDrmCompositor,
    pub output: Output,
    /// Snapshot from connect time; modes can't change without a hotplug,
    /// which tears the surface down anyway. Lets reloads re-pick the mode.
    pub connector: connector::Info,
    pub redraw_state: RedrawState,
}

pub struct TtyData {
    pub session: LibSeatSession,
    pub libinput: Libinput,
    pub drm: DrmDevice,
    pub gbm: GbmDevice<DrmDeviceFd>,
    pub node: DrmNode,
    pub renderer: GlesRenderer,
    pub surfaces: HashMap<crtc::Handle, TtySurface>,
    /// Displays config as of the last apply; lets `apply_display_settings`
    /// (which runs after every Lua entry) bail without touching DRM.
    pub last_displays: HashMap<String, DisplaySettings>,
    pub cursor_buffer: SolidColorBuffer,
}

pub fn init(takhti: &mut Takhti) -> Result<()> {
    let (session, notifier) = LibSeatSession::new()
        .context("error creating libseat session (is seatd or logind available?)")?;
    let seat_name = session.seat();
    info!("libseat session on seat {seat_name}");

    let mut libinput =
        Libinput::new_with_udev(LibinputSessionInterface::from(session.clone()));
    libinput
        .udev_assign_seat(&seat_name)
        .map_err(|()| anyhow!("error assigning libinput seat"))?;
    let input_backend = LibinputInputBackend::new(libinput.clone());
    takhti.loop_handle
        .insert_source(input_backend, |event, _, takhti| {
            takhti.process_input_event(event);
        })
        .map_err(|err| anyhow!("error inserting libinput source: {err}"))?;

    takhti.loop_handle
        .insert_source(notifier, |event, _, takhti| takhti.on_session_event(event))
        .map_err(|err| anyhow!("error inserting session source: {err}"))?;

    let udev_backend = UdevBackend::new(&seat_name).context("error creating udev backend")?;
    takhti.loop_handle
        .insert_source(udev_backend, |event, _, _comp| match event {
            UdevEvent::Added { device_id, .. } => {
                debug!("udev device added: {device_id} (hotplug not yet supported)");
            }
            UdevEvent::Changed { device_id } => debug!("udev device changed: {device_id}"),
            UdevEvent::Removed { device_id } => debug!("udev device removed: {device_id}"),
        })
        .map_err(|err| anyhow!("error inserting udev source: {err}"))?;

    let primary_gpu_path = udev::primary_gpu(&seat_name)
        .context("error probing primary GPU")?
        .ok_or_else(|| anyhow!("no GPU found on seat {seat_name}"))?;
    info!("using GPU {primary_gpu_path:?}");
    let node = DrmNode::from_path(&primary_gpu_path).context("error opening DRM node")?;

    let mut session_for_open = session.clone();
    let fd = session_for_open
        .open(
            &primary_gpu_path,
            OFlags::RDWR | OFlags::CLOEXEC | OFlags::NOCTTY | OFlags::NONBLOCK,
        )
        .context("error opening DRM device through the session")?;
    let device_fd = DrmDeviceFd::new(DeviceFd::from(fd));

    let (drm, drm_notifier) =
        DrmDevice::new(device_fd.clone(), true).context("error creating DRM device")?;
    let gbm = GbmDevice::new(device_fd.clone()).context("error creating GBM device")?;

    let egl_display =
        unsafe { EGLDisplay::new(gbm.clone()) }.context("error creating EGL display")?;
    let egl_context = EGLContext::new(&egl_display).context("error creating EGL context")?;
    let mut renderer =
        unsafe { GlesRenderer::new(egl_context) }.context("error creating GLES renderer")?;

    if renderer.bind_wl_display(&takhti.display_handle).is_err() {
        debug!("legacy EGL display binding unavailable (expected on modern systems)");
    }
    let formats = renderer.dmabuf_formats();
    let _dmabuf_global = takhti
        .dmabuf_state
        .create_global::<Takhti>(&takhti.display_handle, formats);

    // Expose linux-drm-syncobj-v1 (explicit sync) when the GPU supports
    // syncobj_eventfd. Clients that use it (NVIDIA-driven GL/Vulkan, Electron
    // apps like Discord) then tell us exactly when a buffer is ready instead
    // of relying on implicit fences.
    if supports_syncobj_eventfd(&device_fd) {
        info!("explicit sync (linux-drm-syncobj-v1) enabled");
        takhti.syncobj_state = Some(DrmSyncobjState::new::<Takhti>(
            &takhti.display_handle,
            device_fd.clone(),
        ));
    } else {
        info!("explicit sync unavailable: GPU lacks syncobj_eventfd support");
    }

    takhti.loop_handle
        .insert_source(drm_notifier, |event, _meta, takhti| match event {
            DrmEvent::VBlank(crtc) => on_vblank(takhti, crtc),
            DrmEvent::Error(err) => warn!("DRM error: {err}"),
        })
        .map_err(|err| anyhow!("error inserting DRM source: {err}"))?;

    let mut data = TtyData {
        session,
        libinput,
        drm,
        gbm,
        node,
        renderer,
        surfaces: HashMap::new(),
        last_displays: takhti.lua.settings().displays,
        cursor_buffer: SolidColorBuffer::new((8, 16), [1.0, 1.0, 1.0, 1.0]),
    };

    let mut scanner: smithay_drm_extras::drm_scanner::DrmScanner =
        smithay_drm_extras::drm_scanner::DrmScanner::new();
    let scan = scanner
        .scan_connectors(&data.drm)
        .context("error scanning connectors")?;
    let mut x = 0;
    for event in scan {
        let smithay_drm_extras::drm_scanner::DrmScanEvent::Connected {
            connector,
            crtc: Some(crtc),
        } = event
        else {
            continue;
        };
        match connector_connected(takhti, &mut data, connector, crtc, x) {
            Ok(width) => x += width,
            Err(err) => warn!("error setting up connector: {err:#}"),
        }
    }
    if data.surfaces.is_empty() {
        bail!("no connected outputs found");
    }

    takhti.backend = Backend::Tty(data);
    // Runs the Lua outputs hook and, via after_lua, queues the first redraws.
    takhti.outputs_changed(true);
    queue_redraw_all(takhti);
    Ok(())
}

/// Choose a display mode per `settings.displays[name].resolution`. Resolve
/// the size first (preferred / largest area / exact), then the refresh among
/// modes of that size. Interlaced modes are skipped (they don't work — see
/// niri's pick_mode). Returns the fallback flag: true means nothing matched
/// and the EDID-preferred mode was used instead, so a config written for one
/// monitor degrades gracefully on another.
fn pick_mode(connector: &connector::Info, target: Resolution) -> Option<(DrmMode, bool)> {
    let modes = connector.modes();
    let preferred = modes
        .iter()
        .find(|m| m.mode_type().contains(ModeTypeFlags::PREFERRED))
        .or_else(|| modes.first())
        .copied()?;
    let progressive = || {
        modes
            .iter()
            .filter(|m| !m.flags().contains(ModeFlags::INTERLACE))
            .copied()
    };

    let size = match target.size {
        SizeSetting::Preferred => preferred.size(),
        SizeSetting::Max => match progressive().max_by_key(|m| {
            let (w, h) = m.size();
            w as u64 * h as u64
        }) {
            Some(m) => m.size(),
            None => return Some((preferred, true)),
        },
        SizeSetting::Exact(w, h) => (w, h),
    };
    let at_size = || progressive().filter(|m| m.size() == size);

    // Refresh comparisons in millihertz, via the wl_output conversion.
    let refresh = |m: &DrmMode| Mode::from(*m).refresh;
    let chosen = match target.refresh {
        // Bare "preferred" means the EDID mode as-is, not its size at max refresh.
        RefreshSetting::Auto if target.size == SizeSetting::Preferred => Some(preferred),
        RefreshSetting::Auto | RefreshSetting::Max => at_size().max_by_key(refresh),
        // Exact match first (niri-style), else within 1 Hz ("60" matches 59.94).
        RefreshSetting::Exact(mhz) => at_size()
            .min_by_key(|m| (refresh(m) - mhz).abs())
            .filter(|m| (refresh(m) - mhz).abs() <= 1000),
    };
    match chosen {
        Some(mode) => Some((mode, false)),
        None => Some((preferred, true)),
    }
}

fn connector_connected(
    takhti: &mut Takhti,
    data: &mut TtyData,
    connector: connector::Info,
    crtc: crtc::Handle,
    x: i32,
) -> Result<i32> {
    // Kernel connector names ("DP-1", "HDMI-A-1"): what users key
    // `settings.displays` by, matching every other compositor.
    let name = format!(
        "{}-{}",
        connector.interface().as_str(),
        connector.interface_id()
    );

    let (mode, fallback) = pick_mode(&connector, takhti.lua.settings().resolution_for(&name))
        .context("connector has no modes")?;
    if fallback {
        warn!("output {name}: no mode matches the configured resolution; using preferred");
    }
    let (w, h) = mode.size();
    info!("connecting output {name}: {w}x{h}@{}", mode.vrefresh());

    let surface = data
        .drm
        .create_surface(crtc, mode, &[connector.handle()])
        .context("error creating DRM surface")?;

    let (phys_w, phys_h) = connector.size().unwrap_or((0, 0));
    let output = Output::new(
        name,
        PhysicalProperties {
            size: (phys_w as i32, phys_h as i32).into(),
            subpixel: Subpixel::Unknown,
            make: "Unknown".into(),
            model: "Unknown".into(),
            serial_number: "Unknown".into(),
        },
    );
    let wl_mode = Mode::from(mode);
    // Outputs live at integer physical positions; the logical position (for
    // wl_output/xdg-output) is derived at the protocol boundary.
    let scale = takhti.space.scale();
    let logical_loc = crate::coords::rect_to_logical(
        smithay::utils::Rectangle::new((x, 0).into(), wl_mode.size),
        scale,
    )
    .loc;
    output.change_current_state(
        Some(wl_mode),
        None,
        Some(smithay::output::Scale::Fractional(scale)),
        Some(logical_loc),
    );
    output.set_preferred(wl_mode);
    let _global = output.create_global::<Takhti>(&takhti.display_handle);
    takhti.space.map_output(&output, (x, 0));

    let allocator = GbmAllocator::new(
        data.gbm.clone(),
        GbmBufferFlags::RENDERING | GbmBufferFlags::SCANOUT,
    );
    let render_formats = data
        .renderer
        .egl_context()
        .dmabuf_render_formats()
        .clone();
    let compositor = DrmCompositor::new(
        smithay::output::OutputModeSource::Auto(output.downgrade()),
        surface,
        None,
        allocator,
        GbmFramebufferExporter::new(data.gbm.clone(), Some(data.node).into()),
        SUPPORTED_COLOR_FORMATS,
        render_formats,
        data.drm.cursor_size(),
        Some(data.gbm.clone()),
    )
    .context("error creating DRM compositor")?;

    data.surfaces.insert(
        crtc,
        TtySurface {
            compositor,
            output,
            connector,
            redraw_state: RedrawState::Idle,
        },
    );
    Ok(wl_mode.size.w)
}

/// Re-pick every output's mode against the current `settings.displays`
/// (config reload). Returns true if any mode changed; the caller re-emits
/// `outputs_changed` so the Lua WM can retile. Runs after every Lua entry,
/// so it bails immediately unless the displays config actually changed.
pub fn apply_display_settings(takhti: &mut Takhti) -> bool {
    let settings = takhti.lua.settings();
    let Backend::Tty(data) = &mut takhti.backend else {
        return false;
    };
    if settings.displays == data.last_displays {
        return false;
    }
    data.last_displays = settings.displays.clone();

    let mut changed = false;
    for surface in data.surfaces.values_mut() {
        let name = surface.output.name();
        let Some((mode, fallback)) =
            pick_mode(&surface.connector, settings.resolution_for(&name))
        else {
            continue;
        };
        if fallback {
            warn!("output {name}: no mode matches the configured resolution; using preferred");
        }
        if mode == surface.compositor.pending_mode() {
            continue;
        }
        if let Err(err) = surface.compositor.use_mode(mode) {
            warn!(
                "output {}: error setting mode {}x{}@{}: {err}",
                surface.output.name(),
                mode.size().0,
                mode.size().1,
                mode.vrefresh(),
            );
            continue;
        }
        let (w, h) = mode.size();
        info!(
            "output {}: mode changed to {w}x{h}@{}",
            surface.output.name(),
            mode.vrefresh(),
        );
        surface
            .output
            .change_current_state(Some(Mode::from(mode)), None, None, None);
        changed = true;
    }
    if !changed {
        return false;
    }

    // Widths may have changed: re-pack outputs left-to-right, preserving the
    // connect-time order (space outputs keep insertion order).
    let outputs: Vec<Output> = takhti.space.outputs().cloned().collect();
    let scale = takhti.space.scale();
    let mut x = 0;
    for output in &outputs {
        let Some(mode) = output.current_mode() else {
            continue;
        };
        let size = output.current_transform().transform_size(mode.size);
        let logical_loc = crate::coords::rect_to_logical(
            smithay::utils::Rectangle::new((x, 0).into(), size),
            scale,
        )
        .loc;
        output.change_current_state(None, None, None, Some(logical_loc));
        takhti.space.map_output(output, (x, 0));
        x += size.w;
    }
    queue_redraw_all(takhti);
    true
}

/// Request a repaint of one output. Cheap and idempotent: every damage source
/// (commits, Lua ops, cursor motion) calls this; the state machine coalesces.
pub fn queue_redraw(takhti: &mut Takhti, crtc: crtc::Handle) {
    let Takhti {
        backend,
        loop_handle,
        ..
    } = takhti;
    let Backend::Tty(data) = backend else { return };
    let Some(surface) = data.surfaces.get_mut(&crtc) else {
        return;
    };
    surface.redraw_state = match mem::take(&mut surface.redraw_state) {
        RedrawState::Idle => {
            loop_handle.insert_idle(move |takhti| render_surface(takhti, crtc));
            RedrawState::Queued
        }
        RedrawState::Queued => RedrawState::Queued,
        RedrawState::WaitingForVBlank { .. } => RedrawState::WaitingForVBlank {
            redraw_needed: true,
        },
        RedrawState::WaitingForEstimatedVBlank(token)
        | RedrawState::WaitingForEstimatedVBlankAndQueued(token) => {
            RedrawState::WaitingForEstimatedVBlankAndQueued(token)
        }
    };
}

pub fn queue_redraw_all(takhti: &mut Takhti) {
    let Backend::Tty(data) = &takhti.backend else {
        return;
    };
    let crtcs: Vec<_> = data.surfaces.keys().copied().collect();
    for crtc in crtcs {
        queue_redraw(takhti, crtc);
    }
}

fn on_vblank(takhti: &mut Takhti, crtc: crtc::Handle) {
    {
        let Backend::Tty(data) = &mut takhti.backend else {
            return;
        };
        let Some(surface) = data.surfaces.get_mut(&crtc) else {
            return;
        };
        if let Err(err) = surface.compositor.frame_submitted() {
            warn!("error marking frame submitted: {err}");
        }
        match mem::take(&mut surface.redraw_state) {
            // Damage arrived while the frame was in flight: repaint again.
            RedrawState::WaitingForVBlank {
                redraw_needed: true,
            } => {}
            // Presented and nothing new: go idle until the next queue_redraw.
            RedrawState::WaitingForVBlank {
                redraw_needed: false,
            } => return,
            // Stale vblank (e.g. right after a VT switch): don't disturb.
            other => {
                surface.redraw_state = other;
                return;
            }
        }
    }
    queue_redraw(takhti, crtc);
}

/// The estimated-vblank timer fired: idle out, or repaint if damage arrived.
fn on_estimated_vblank(takhti: &mut Takhti, crtc: crtc::Handle) {
    {
        let Backend::Tty(data) = &mut takhti.backend else {
            return;
        };
        let Some(surface) = data.surfaces.get_mut(&crtc) else {
            return;
        };
        match mem::take(&mut surface.redraw_state) {
            RedrawState::WaitingForEstimatedVBlank(_) => return,
            RedrawState::WaitingForEstimatedVBlankAndQueued(_) => {
                surface.redraw_state = RedrawState::Queued;
            }
            other => {
                surface.redraw_state = other;
                return;
            }
        }
    }
    render_surface(takhti, crtc);
}

/// After a no-damage render nothing is queued to DRM, so no vblank will
/// arrive. Schedule a timer one refresh interval out to stand in for it.
fn queue_estimated_vblank(
    loop_handle: &LoopHandle<'static, Takhti>,
    surface: &mut TtySurface,
    crtc: crtc::Handle,
) {
    // Reuse a timer that is already pending.
    match mem::take(&mut surface.redraw_state) {
        RedrawState::WaitingForEstimatedVBlank(token)
        | RedrawState::WaitingForEstimatedVBlankAndQueued(token) => {
            surface.redraw_state = RedrawState::WaitingForEstimatedVBlank(token);
            return;
        }
        _ => {}
    }
    let refresh_mhz = surface
        .output
        .current_mode()
        .map(|mode| mode.refresh)
        .filter(|&r| r > 0)
        .unwrap_or(60_000);
    let interval = Duration::from_secs_f64(1000.0 / refresh_mhz as f64);
    let timer = Timer::from_duration(interval);
    match loop_handle.insert_source(timer, move |_, _, takhti| {
        on_estimated_vblank(takhti, crtc);
        TimeoutAction::Drop
    }) {
        Ok(token) => surface.redraw_state = RedrawState::WaitingForEstimatedVBlank(token),
        Err(err) => {
            warn!("error scheduling estimated-vblank timer: {err}");
            surface.redraw_state = RedrawState::Idle;
        }
    }
}

pub fn render_surface(takhti: &mut Takhti, crtc: crtc::Handle) {
    // Data that needs shared access to `takhti`, gathered before splitting borrows.
    let output = {
        let Backend::Tty(data) = &takhti.backend else { return };
        let Some(surface) = data.surfaces.get(&crtc) else { return };
        surface.output.clone()
    };
    let (output_loc, output_size) = takhti
        .space
        .output_geometry(&output)
        .map(|geo| (geo.loc, geo.size))
        .unwrap_or_default();
    let borders = crate::render::border_elements(takhti, output_loc);
    let pointer_pos = takhti
        .seat
        .get_pointer()
        .map(|p| p.current_location())
        .unwrap_or_default();
    let cursor_status = takhti.cursor_status.clone();

    let Takhti {
        backend,
        space,
        start_time,
        loop_handle,
        cursor,
        ui,
        binds,
        ..
    } = takhti;
    let Backend::Tty(data) = backend else { return };
    let Some(surface) = data.surfaces.get_mut(&crtc) else {
        return;
    };

    let mut elements: Vec<OutputRenderElements> = Vec::new();
    let scale = space.scale();

    // Cursor: client-provided surface, xcursor theme, or block fallback.
    // Pointer position converts from protocol-logical once, then everything
    // is physical and snapped to the grid.
    let cursor_phys =
        crate::coords::point_to_physical(pointer_pos, scale) - output_loc.to_f64();
    match &cursor_status {
        CursorImageStatus::Hidden => {}
        CursorImageStatus::Surface(cursor_surface) if cursor_surface.alive() => {
            let hotspot = with_states(cursor_surface, |states| {
                states
                    .data_map
                    .get::<CursorImageSurfaceData>()
                    .map(|data| data.lock().unwrap().hotspot)
            })
            .unwrap_or_default();
            // The hotspot is in the cursor surface's coordinates (logical).
            let hotspot_phys =
                crate::coords::logical_point_to_physical(hotspot.to_f64(), scale);
            let pos = (cursor_phys - hotspot_phys.to_f64()).to_i32_round();
            elements.extend(
                render_elements_from_surface_tree(
                    &mut data.renderer,
                    cursor_surface,
                    pos,
                    scale,
                    1.0,
                    Kind::Cursor,
                )
                .into_iter()
                .map(OutputRenderElements::Surface),
            );
        }
        _ => {
            if let Some(element) = cursor.element(&mut data.renderer, cursor_phys) {
                elements.push(OutputRenderElements::Memory(element));
            } else {
                elements.push(OutputRenderElements::Solid(
                    SolidColorRenderElement::from_buffer(
                        &data.cursor_buffer,
                        cursor_phys.to_i32_round::<i32>(),
                        1.0,
                        1.0,
                        Kind::Cursor,
                    ),
                ));
            }
        }
    }

    // Compositor UI (dialogs/overlays): above windows, below the cursor.
    let ui_elements = ui.render_elements(&mut data.renderer, output_size, binds);
    elements.extend(crate::render::scene_elements(
        &mut data.renderer,
        space,
        &surface.output,
        ui_elements,
        borders,
    ));

    match surface
        .compositor
        .render_frame(&mut data.renderer, &elements, CLEAR_COLOR, FrameFlags::empty())
    {
        Ok(res) => {
            // KMS can't fence this frame (no IN_FENCE_FD, or the GL sync
            // isn't exportable — common on NVIDIA): wait for the render to
            // finish CPU-side or the display scans out a half-drawn buffer.
            if res.needs_sync() {
                if let PrimaryPlaneElement::Swapchain(element) = &res.primary_element {
                    if let Err(err) = element.sync.wait() {
                        warn!("error waiting for frame completion: {err:?}");
                    }
                }
            }
            send_frames(space, &surface.output, start_time.elapsed());
            if res.is_empty {
                queue_estimated_vblank(loop_handle, surface, crtc);
            } else {
                match surface.compositor.queue_frame(()) {
                    Ok(()) => {
                        surface.redraw_state = RedrawState::WaitingForVBlank {
                            redraw_needed: false,
                        };
                    }
                    Err(err) => {
                        warn!("error queueing frame: {err}");
                        surface.redraw_state = RedrawState::Idle;
                    }
                }
            }
        }
        Err(err) => {
            warn!("error rendering frame: {err}");
            surface.redraw_state = RedrawState::Idle;
        }
    }
}

fn send_frames(space: &PhysicalSpace, output: &Output, time: Duration) {
    for window in space.elements() {
        window.send_frame(output, time, Some(Duration::ZERO), |_, _| {
            Some(output.clone())
        });
    }
    // Layer surfaces (bars, launchers, wallpaper) render on frame callbacks
    // like any client; without these they freeze after their first frame.
    for layer in layer_map_for_output(output).layers() {
        layer.send_frame(output, time, Some(Duration::ZERO), |_, _| {
            Some(output.clone())
        });
    }
}

impl Takhti {
    pub fn on_session_event(&mut self, event: SessionEvent) {
        let Backend::Tty(data) = &mut self.backend else {
            return;
        };
        match event {
            SessionEvent::PauseSession => {
                info!("session paused (VT switched away)");
                data.libinput.suspend();
                data.drm.pause();
            }
            SessionEvent::ActivateSession => {
                info!("session activated");
                if data.libinput.resume().is_err() {
                    warn!("error resuming libinput");
                }
                if let Err(err) = data.drm.activate(false) {
                    warn!("error activating DRM device: {err}");
                }
                let crtcs: Vec<_> = data.surfaces.keys().copied().collect();
                for crtc in &crtcs {
                    if let Some(surface) = data.surfaces.get_mut(crtc) {
                        if let Err(err) = surface.compositor.reset_state() {
                            warn!("error resetting DRM compositor state: {err}");
                        }
                        // Frames in flight were lost with the VT; cancel any
                        // pending estimated-vblank timer and start fresh.
                        match mem::take(&mut surface.redraw_state) {
                            RedrawState::WaitingForEstimatedVBlank(token)
                            | RedrawState::WaitingForEstimatedVBlankAndQueued(token) => {
                                self.loop_handle.remove(token);
                            }
                            _ => {}
                        }
                    }
                }
                for crtc in crtcs {
                    queue_redraw(self, crtc);
                }
            }
        }
    }

    pub fn change_vt(&mut self, vt: i32) {
        if let Backend::Tty(data) = &mut self.backend {
            if let Err(err) = data.session.change_vt(vt) {
                warn!("error switching VT: {err}");
            }
        }
    }
}
