//! Render resources as scoped units with inverses — the temporal axis of
//! the lifecycle applied to render resources.
//!
//! A *render resource* is any owned render object: a decoded asset in the
//! renderer's asset cache (`assets`), a retained scene frame, an allocated
//! buffer, a per-surface cache, … The [`ResourceScope`] owns a host (`R`)
//! and a set of mounted resource units over it. Each mount runs a
//! caller-supplied [`Effect`] against the host *now* and returns the
//! [`Inverse`] of that effect; unmount replays the inverses in reverse, so
//! the scope returns to its pre-mount state with **no residue** — no leaked
//! buffers, layers, or assets.
//!
//! This is deliberately small and agnostic of the concrete resource type,
//! exactly like the sibling `compose` kernel in `moonshell-surface`. In
//! this crate it is the live owner of the [`Renderer`]'s asset cache: a
//! surface mounts the assets it decodes (its mount effect), and unmounting
//! that surface evicts exactly the resources it owned. Resources are the
//! spatiotemporal inverse of the scene frame the surface draws.

use std::collections::BTreeMap;

/// Undoes one committed mount-side effect.
pub type Inverse<R> = Box<dyn FnOnce(&mut R)>;

/// A mount-side effect on the resource host `R`; returns its inverse.
pub type Effect<R> = Box<dyn Fn(&mut R) -> Inverse<R>>;

/// Owns a host `R` and the resource scope over it. Mounting a resource
/// runs effects on the host now; unmounting replays their inverses in
/// reverse.
pub struct ResourceScope<R> {
    host: R,
    units: BTreeMap<u64, Vec<Inverse<R>>>,
    next: u64,
    /// Registration order, top to bottom, for reverse unmount.
    order: Vec<u64>,
}

impl<R> ResourceScope<R> {
    pub fn new(host: R) -> Self {
        Self {
            host,
            units: BTreeMap::new(),
            next: 0,
            order: Vec::new(),
        }
    }

    /// Run `effect` against the host now, register its inverse for
    /// unmount, and return the resource id.
    pub fn mount(&mut self, effect: Effect<R>) -> u64 {
        let id = self.next;
        self.next += 1;
        let inverse = effect(&mut self.host);
        self.units.insert(id, vec![inverse]);
        self.order.push(id);
        id
    }

    /// Replay this unit's effects' inverses in reverse order, releasing
    /// every resource it owned. A no-op for an unknown id.
    pub fn unmount(&mut self, id: u64) {
        let Some(mut inverses) = self.units.remove(&id) else {
            return;
        };
        for inverse in inverses.drain(..).rev() {
            inverse(&mut self.host);
        }
        self.order.retain(|&o| o != id);
    }

    /// Live resource IDs in registration order (mount order for the
    /// host's active resources).
    pub fn ids(&self) -> Vec<u64> {
        self.order.clone()
    }

    /// Number of mounted resource units (for the temporal no-residue
    /// assertion).
    pub fn units_len(&self) -> usize {
        self.units.len()
    }

    /// Unmount every unit in reverse registration order — e.g. a whole
    /// surface's worth of layers/assets at once.
    pub fn unmount_all(&mut self) {
        while let Some(&id) = self.order.last() {
            self.unmount(id);
        }
    }

    /// Borrow the host (for reads/gets).
    pub fn host(&self) -> &R {
        &self.host
    }

    pub fn host_mut(&mut self) -> &mut R {
        &mut self.host
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    type Assets = HashMap<&'static str, u32>;

    /// A mount that allocates a resource and whose inverse frees it.
    #[test]
    fn mount_effect_then_unmount_releases() {
        let mut scope = ResourceScope::new(Assets::new());
        let id = scope.mount(Box::new(|assets: &mut Assets| {
            assets.insert("icon", 32);
            Box::new(|assets: &mut Assets| {
                assets.remove("icon");
            })
        }));
        assert_eq!(scope.host().get("icon"), Some(&32));

        scope.unmount(id);
        assert!(!scope.host().contains_key("icon"), "resource leaked");
        assert_eq!(scope.host().len(), 0, "residue after unmount");
        assert!(scope.ids().is_empty());
    }

    /// All units unmount together, releasing everything — no residue.
    #[test]
    fn unmount_all_releases_everything() {
        let mut scope = ResourceScope::new(Assets::new());
        let a = scope.mount(Box::new(|assets: &mut Assets| {
            assets.insert("a", 1);
            Box::new(|assets: &mut Assets| {
                assets.remove("a");
            })
        }));
        let b = scope.mount(Box::new(|assets: &mut Assets| {
            assets.insert("b", 2);
            Box::new(|assets: &mut Assets| {
                assets.remove("b");
            })
        }));
        assert!(scope.host().contains_key("a") && scope.host().contains_key("b"));
        assert_ne!(a, b, "distinct resource ids");
        assert_eq!(scope.ids(), vec![a, b], "registration order, mount order");
        scope.unmount_all();
        assert!(scope.host().is_empty());
        assert!(scope.ids().is_empty());
    }

    /// Unmounting an unknown id is a no-op.
    #[test]
    fn unmount_unknown_id_is_noop() {
        let mut scope = ResourceScope::new(Assets::new());
        scope.unmount(7);
        assert_eq!(scope.host().len(), 0);
    }

    /// Temporal snapshot proof at the scope layer: mount, exercise,
    /// unmount — the host returns to its pre-mount size.
    #[test]
    fn unmount_returns_host_to_premount_size() {
        let mut scope = ResourceScope::new(Assets::new());
        let snapshot = scope.host().len();

        let id = scope.mount(Box::new(|assets: &mut Assets| {
            assets.insert("x", 1);
            assets.insert("y", 2);
            Box::new(|assets: &mut Assets| {
                assets.remove("x");
                assets.remove("y");
            })
        }));

        assert_eq!(scope.host().len(), snapshot + 2, "mounted effects applied");

        scope.unmount(id);
        assert_eq!(scope.host().len(), snapshot, "residue after unmount");
    }
}
