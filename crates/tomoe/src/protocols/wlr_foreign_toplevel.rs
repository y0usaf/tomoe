//! wlr-foreign-toplevel-management-unstable-v1: the window *control* surface
//! bars and docks actually use (waybar/sfwbar taskbars). The read side
//! mirrors ext-foreign-toplevel-list plus states and outputs; the write side
//! (activate/close/fullscreen/…) hands every request to the
//! [`WlrForeignToplevelHandler`], which routes it through the same
//! `on_window_request` policy path xdg clients use (impl in `handlers.rs`) —
//! never bypassing Lua.
//!
//! Niri's shape (`ref/niri/src/protocols/foreign_toplevel.rs`), adapted from
//! surface-keyed to window-id-keyed: the state stores the last-sent
//! [`ToplevelInfo`] per window and `refresh_toplevel` diffs a fresh snapshot
//! against it, so callers can refresh unconditionally (once per event-loop
//! iteration, from `Tomoe::refresh_wlr_foreign_toplevels`) and only changes
//! hit the wire.

use std::collections::hash_map::Entry;
use std::collections::{HashMap, HashSet};
use std::sync::Arc;

use smithay::output::Output;
use smithay::reexports::wayland_protocols_wlr::foreign_toplevel::v1::server::{
    zwlr_foreign_toplevel_handle_v1::{self, ZwlrForeignToplevelHandleV1},
    zwlr_foreign_toplevel_manager_v1::{self, ZwlrForeignToplevelManagerV1},
};
use smithay::reexports::wayland_server::backend::ClientId;
use smithay::reexports::wayland_server::protocol::wl_output::WlOutput;
use smithay::reexports::wayland_server::{
    Client, DataInit, Dispatch, DisplayHandle, GlobalDispatch, New, Resource,
};

const VERSION: u32 = 3;

/// Snapshot of one window as the protocol sees it. Built by the glue in
/// `foreign_toplevel.rs`; the state diffs it against the last-sent one.
#[derive(Clone, PartialEq)]
pub struct ToplevelInfo {
    pub title: String,
    pub app_id: String,
    pub maximized: bool,
    pub fullscreen: bool,
    /// Keyboard focus. The spec says "activated" mirrors xdg-toplevel, but
    /// clients (waybar, sfwbar, fcitx) treat it as *the* focused window —
    /// send it for at most one window (niri does the same).
    pub activated: bool,
    /// Outputs the window's rendered rect overlaps, `output_enter`/`leave`
    /// diffed per handle.
    pub outputs: Vec<Output>,
}

/// A control request from a handle, resolved to the compositor window id.
pub enum ForeignRequest {
    Activate,
    Close,
    SetFullscreen(Option<WlOutput>),
    UnsetFullscreen,
    SetMaximized,
    UnsetMaximized,
    SetMinimized,
    UnsetMinimized,
}

pub trait WlrForeignToplevelHandler {
    fn wlr_foreign_toplevel_state(&mut self) -> &mut WlrForeignToplevelState;
    /// A client asked for a window-state change through a handle. Route it
    /// through `on_window_request` policy, then the native default — the
    /// same path the window's own client gets.
    fn foreign_toplevel_request(&mut self, id: u64, request: ForeignRequest);
}

/// Window id riding as the handle's dispatch data, so requests resolve
/// without scanning.
pub struct ForeignHandleId(pub u64);

struct ToplevelData {
    info: ToplevelInfo,
    /// Live handles by manager client, each with the wl_outputs it was told
    /// about (for `output_leave` on change).
    instances: HashMap<ZwlrForeignToplevelHandleV1, Vec<WlOutput>>,
}

#[derive(Clone)]
pub struct WlrForeignToplevelGlobalData {
    filter: Arc<dyn for<'c> Fn(&'c Client) -> bool + Send + Sync>,
}

pub struct WlrForeignToplevelState {
    display: DisplayHandle,
    instances: HashSet<ZwlrForeignToplevelManagerV1>,
    toplevels: HashMap<u64, ToplevelData>,
}

impl WlrForeignToplevelState {
    pub fn new<D, F>(display: &DisplayHandle, filter: F) -> Self
    where
        D: GlobalDispatch<ZwlrForeignToplevelManagerV1, WlrForeignToplevelGlobalData>,
        D: Dispatch<ZwlrForeignToplevelManagerV1, ()>,
        D: 'static,
        F: for<'c> Fn(&'c Client) -> bool + Send + Sync + 'static,
    {
        let global_data = WlrForeignToplevelGlobalData {
            filter: Arc::new(filter),
        };
        display.create_global::<D, ZwlrForeignToplevelManagerV1, _>(VERSION, global_data);
        Self {
            display: display.clone(),
            instances: HashSet::new(),
            toplevels: HashMap::new(),
        }
    }

    /// Window ids currently tracked; the glue retires the ones that no
    /// longer exist.
    pub fn tracked_ids(&self) -> Vec<u64> {
        self.toplevels.keys().copied().collect()
    }

    /// Diff `info` against the last-sent state for window `id`: first sight
    /// creates a handle per bound manager, later calls push only changes.
    pub fn refresh_toplevel<D>(&mut self, id: u64, info: ToplevelInfo)
    where
        D: Dispatch<ZwlrForeignToplevelHandleV1, ForeignHandleId>,
        D: 'static,
    {
        match self.toplevels.entry(id) {
            Entry::Occupied(entry) => {
                let data = entry.into_mut();
                if data.info == info {
                    return;
                }
                let new_title = (data.info.title != info.title).then_some(&info.title);
                let new_app_id = (data.info.app_id != info.app_id).then_some(&info.app_id);
                let states_changed = (
                    data.info.maximized,
                    data.info.fullscreen,
                    data.info.activated,
                ) != (info.maximized, info.fullscreen, info.activated);
                let outputs_changed = data.info.outputs != info.outputs;
                for (instance, outputs) in &mut data.instances {
                    if let Some(title) = new_title {
                        instance.title(title.clone());
                    }
                    if let Some(app_id) = new_app_id {
                        instance.app_id(app_id.clone());
                    }
                    if states_changed {
                        instance.state(state_vec(&info));
                    }
                    if outputs_changed {
                        for wl_output in outputs.drain(..) {
                            instance.output_leave(&wl_output);
                        }
                        if let Some(client) = instance.client() {
                            for output in &info.outputs {
                                for wl_output in output.client_outputs(&client) {
                                    instance.output_enter(&wl_output);
                                    outputs.push(wl_output);
                                }
                            }
                        }
                    }
                    instance.done();
                    outputs.retain(|o| o.is_alive());
                }
                data.info = info;
            }
            Entry::Vacant(entry) => {
                let mut data = ToplevelData {
                    info,
                    instances: HashMap::new(),
                };
                for manager in &self.instances {
                    if let Some(client) = manager.client() {
                        data.add_instance::<D>(&self.display, &client, manager, id);
                    }
                }
                entry.insert(data);
            }
        }
    }

    /// The window is gone: `closed` on every handle, drop the data. Requests
    /// on the dead handles resolve to an id no window carries — no-ops.
    pub fn retire(&mut self, id: u64) {
        if let Some(data) = self.toplevels.remove(&id) {
            for instance in data.instances.keys() {
                instance.closed();
            }
        }
    }

    /// A client bound a fresh wl_output for `output`: send the enters its
    /// handles couldn't get before the bind existed.
    pub fn on_output_bound(&mut self, output: &Output, wl_output: &WlOutput) {
        let Some(client) = wl_output.client() else {
            return;
        };
        for data in self.toplevels.values_mut() {
            if !data.info.outputs.contains(output) {
                continue;
            }
            for (instance, outputs) in &mut data.instances {
                if instance.client().as_ref() != Some(&client) {
                    continue;
                }
                instance.output_enter(wl_output);
                instance.done();
                outputs.push(wl_output.clone());
            }
        }
    }
}

/// wlr state array: native-endian u32s.
fn state_vec(info: &ToplevelInfo) -> Vec<u8> {
    let mut states = Vec::with_capacity(3);
    if info.maximized {
        states.push(zwlr_foreign_toplevel_handle_v1::State::Maximized as u32);
    }
    if info.fullscreen {
        states.push(zwlr_foreign_toplevel_handle_v1::State::Fullscreen as u32);
    }
    if info.activated {
        states.push(zwlr_foreign_toplevel_handle_v1::State::Activated as u32);
    }
    states.iter().flat_map(|s| s.to_ne_bytes()).collect()
}

impl ToplevelData {
    fn add_instance<D>(
        &mut self,
        handle: &DisplayHandle,
        client: &Client,
        manager: &ZwlrForeignToplevelManagerV1,
        id: u64,
    ) where
        D: Dispatch<ZwlrForeignToplevelHandleV1, ForeignHandleId>,
        D: 'static,
    {
        let Ok(toplevel) = client.create_resource::<ZwlrForeignToplevelHandleV1, _, D>(
            handle,
            manager.version(),
            ForeignHandleId(id),
        ) else {
            return;
        };
        manager.toplevel(&toplevel);
        toplevel.title(self.info.title.clone());
        toplevel.app_id(self.info.app_id.clone());
        toplevel.state(state_vec(&self.info));
        let mut outputs = Vec::new();
        for output in &self.info.outputs {
            for wl_output in output.client_outputs(client) {
                toplevel.output_enter(&wl_output);
                outputs.push(wl_output);
            }
        }
        toplevel.done();
        self.instances.insert(toplevel, outputs);
    }
}

impl<D> GlobalDispatch<ZwlrForeignToplevelManagerV1, WlrForeignToplevelGlobalData, D>
    for WlrForeignToplevelState
where
    D: GlobalDispatch<ZwlrForeignToplevelManagerV1, WlrForeignToplevelGlobalData>,
    D: Dispatch<ZwlrForeignToplevelManagerV1, ()>,
    D: Dispatch<ZwlrForeignToplevelHandleV1, ForeignHandleId>,
    D: WlrForeignToplevelHandler,
    D: 'static,
{
    fn bind(
        state: &mut D,
        handle: &DisplayHandle,
        client: &Client,
        resource: New<ZwlrForeignToplevelManagerV1>,
        _global_data: &WlrForeignToplevelGlobalData,
        data_init: &mut DataInit<'_, D>,
    ) {
        let manager = data_init.init(resource, ());
        let state = state.wlr_foreign_toplevel_state();
        for (id, data) in state.toplevels.iter_mut() {
            data.add_instance::<D>(handle, client, &manager, *id);
        }
        state.instances.insert(manager);
    }

    fn can_view(client: Client, global_data: &WlrForeignToplevelGlobalData) -> bool {
        (global_data.filter)(&client)
    }
}

impl<D> Dispatch<ZwlrForeignToplevelManagerV1, (), D> for WlrForeignToplevelState
where
    D: Dispatch<ZwlrForeignToplevelManagerV1, ()>,
    D: WlrForeignToplevelHandler,
{
    fn request(
        state: &mut D,
        _client: &Client,
        resource: &ZwlrForeignToplevelManagerV1,
        request: <ZwlrForeignToplevelManagerV1 as Resource>::Request,
        _data: &(),
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, D>,
    ) {
        match request {
            zwlr_foreign_toplevel_manager_v1::Request::Stop => {
                resource.finished();
                state
                    .wlr_foreign_toplevel_state()
                    .instances
                    .remove(resource);
            }
            _ => unreachable!(),
        }
    }

    fn destroyed(
        state: &mut D,
        _client: ClientId,
        resource: &ZwlrForeignToplevelManagerV1,
        _data: &(),
    ) {
        // In case `stop` was never sent (sudden disconnect).
        state
            .wlr_foreign_toplevel_state()
            .instances
            .remove(resource);
    }
}

impl<D> Dispatch<ZwlrForeignToplevelHandleV1, ForeignHandleId, D> for WlrForeignToplevelState
where
    D: Dispatch<ZwlrForeignToplevelHandleV1, ForeignHandleId>,
    D: WlrForeignToplevelHandler,
{
    fn request(
        state: &mut D,
        _client: &Client,
        _resource: &ZwlrForeignToplevelHandleV1,
        request: <ZwlrForeignToplevelHandleV1 as Resource>::Request,
        data: &ForeignHandleId,
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, D>,
    ) {
        use zwlr_foreign_toplevel_handle_v1::Request;
        let foreign_request = match request {
            Request::Activate { .. } => ForeignRequest::Activate,
            Request::Close => ForeignRequest::Close,
            Request::SetFullscreen { output } => ForeignRequest::SetFullscreen(output),
            Request::UnsetFullscreen => ForeignRequest::UnsetFullscreen,
            Request::SetMaximized => ForeignRequest::SetMaximized,
            Request::UnsetMaximized => ForeignRequest::UnsetMaximized,
            Request::SetMinimized => ForeignRequest::SetMinimized,
            Request::UnsetMinimized => ForeignRequest::UnsetMinimized,
            // No native minimized state to report rectangles for.
            Request::SetRectangle { .. } | Request::Destroy => return,
            _ => return,
        };
        // A retired handle's id maps to no window; the handler no-ops.
        state.foreign_toplevel_request(data.0, foreign_request);
    }

    fn destroyed(
        state: &mut D,
        _client: ClientId,
        resource: &ZwlrForeignToplevelHandleV1,
        data: &ForeignHandleId,
    ) {
        if let Some(data) = state
            .wlr_foreign_toplevel_state()
            .toplevels
            .get_mut(&data.0)
        {
            data.instances.remove(resource);
        }
    }
}

#[macro_export]
macro_rules! delegate_wlr_foreign_toplevel {
    ($(@<$( $lt:tt $( : $clt:tt $(+ $dlt:tt )* )? ),+>)? $ty: ty) => {
        smithay::reexports::wayland_server::delegate_global_dispatch!($(@< $( $lt $( : $clt $(+ $dlt )* )? ),+ >)? $ty: [
            smithay::reexports::wayland_protocols_wlr::foreign_toplevel::v1::server::zwlr_foreign_toplevel_manager_v1::ZwlrForeignToplevelManagerV1: $crate::protocols::wlr_foreign_toplevel::WlrForeignToplevelGlobalData
        ] => $crate::protocols::wlr_foreign_toplevel::WlrForeignToplevelState);
        smithay::reexports::wayland_server::delegate_dispatch!($(@< $( $lt $( : $clt $(+ $dlt )* )? ),+ >)? $ty: [
            smithay::reexports::wayland_protocols_wlr::foreign_toplevel::v1::server::zwlr_foreign_toplevel_manager_v1::ZwlrForeignToplevelManagerV1: ()
        ] => $crate::protocols::wlr_foreign_toplevel::WlrForeignToplevelState);
        smithay::reexports::wayland_server::delegate_dispatch!($(@< $( $lt $( : $clt $(+ $dlt )* )? ),+ >)? $ty: [
            smithay::reexports::wayland_protocols_wlr::foreign_toplevel::v1::server::zwlr_foreign_toplevel_handle_v1::ZwlrForeignToplevelHandleV1: $crate::protocols::wlr_foreign_toplevel::ForeignHandleId
        ] => $crate::protocols::wlr_foreign_toplevel::WlrForeignToplevelState);
    };
}
