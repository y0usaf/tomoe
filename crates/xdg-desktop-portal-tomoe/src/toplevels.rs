//! Toplevel enumeration over a short-lived Wayland connection, via
//! ext-foreign-toplevel-list-v1.
//!
//! Like `outputs.rs`: each `SelectSources` call gets a fresh connection so
//! the list is never stale. The `identifier` is the compositor's stable
//! handle for the window — the streaming thread re-finds the toplevel by it
//! on its own connection.

use std::collections::HashMap;

use wayland_client::protocol::wl_registry;
use wayland_client::{Connection, Dispatch, EventQueue, Proxy, QueueHandle};
use wayland_protocols::ext::foreign_toplevel_list::v1::client::{
    ext_foreign_toplevel_handle_v1::{self, ExtForeignToplevelHandleV1},
    ext_foreign_toplevel_list_v1::{self, ExtForeignToplevelListV1},
};

#[derive(Debug, Clone)]
pub struct ToplevelInfo {
    pub identifier: String,
    pub title: String,
    pub app_id: String,
}

#[derive(Default)]
struct Enumerator {
    /// Per-proxy accumulation keyed by protocol id; moved to `done` on the
    /// handle's `Done` event.
    pending: HashMap<u32, ToplevelInfo>,
    done: Vec<ToplevelInfo>,
}

impl Dispatch<wl_registry::WlRegistry, ()> for Enumerator {
    fn event(
        _: &mut Self,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_registry::Event::Global {
            name,
            interface,
            version,
        } = event
        {
            if interface == "ext_foreign_toplevel_list_v1" {
                registry.bind::<ExtForeignToplevelListV1, _, _>(name, version.min(1), qh, ());
            }
        }
    }
}

impl Dispatch<ExtForeignToplevelListV1, ()> for Enumerator {
    fn event(
        _: &mut Self,
        _: &ExtForeignToplevelListV1,
        _: ext_foreign_toplevel_list_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
    wayland_client::event_created_child!(Enumerator, ExtForeignToplevelListV1, [
        ext_foreign_toplevel_list_v1::EVT_TOPLEVEL_OPCODE => (ExtForeignToplevelHandleV1, ()),
    ]);
}

impl Dispatch<ExtForeignToplevelHandleV1, ()> for Enumerator {
    fn event(
        state: &mut Self,
        handle: &ExtForeignToplevelHandleV1,
        event: ext_foreign_toplevel_handle_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        let id = handle.id().protocol_id();
        let cur = state.pending.entry(id).or_insert_with(|| ToplevelInfo {
            identifier: String::new(),
            title: String::new(),
            app_id: String::new(),
        });
        match event {
            ext_foreign_toplevel_handle_v1::Event::Identifier { identifier } => {
                cur.identifier = identifier;
            }
            ext_foreign_toplevel_handle_v1::Event::Title { title } => cur.title = title,
            ext_foreign_toplevel_handle_v1::Event::AppId { app_id } => cur.app_id = app_id,
            ext_foreign_toplevel_handle_v1::Event::Done => {
                let info = state.pending.get(&id).cloned();
                if let Some(info) = info {
                    if !info.identifier.is_empty()
                        && !state.done.iter().any(|t| t.identifier == info.identifier)
                    {
                        state.done.push(info);
                    }
                }
            }
            ext_foreign_toplevel_handle_v1::Event::Closed => {
                if let Some(info) = state.pending.remove(&id) {
                    state.done.retain(|t| t.identifier != info.identifier);
                }
            }
            _ => {}
        }
    }
}

pub fn enumerate() -> Result<Vec<ToplevelInfo>, Box<dyn std::error::Error + Send + Sync>> {
    let conn = Connection::connect_to_env()?;
    let mut event_queue: EventQueue<Enumerator> = conn.new_event_queue();
    let qh = event_queue.handle();
    let _registry = conn.display().get_registry(&qh, ());

    let mut state = Enumerator::default();
    // Round 1 binds the list, rounds 2–3 collect handle events (the toplevel
    // event creates the handle proxies, their state arrives after).
    event_queue.roundtrip(&mut state)?;
    event_queue.roundtrip(&mut state)?;
    event_queue.roundtrip(&mut state)?;

    state.done.sort_by(|a, b| a.title.cmp(&b.title));
    Ok(state.done)
}
