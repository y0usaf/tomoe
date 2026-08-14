//! The surface/render lifecycle as a spatiotemporal unit.
//!
//! Two axes, one mechanism — the reference kernel (`ekko`'s
//! `crates/kernel/src/lib.rs`) adapted to this slice's vocabulary:
//!
//! - **Temporal** — mounting a [`Component`] runs its effects; each
//!   effect returns an [`Inverse`] closure that undoes it.
//!   [`Context::unmount`] replays those inverses in reverse order, so
//!   the context returns to its pre-mount state with **no residue**.
//! - **Spatial** — each unit declares the context keys it *reads*
//!   ([`Component::reads`]). A committed [`Context::set`] on a key
//!   notifies only the units that declared it. Undeclared changes never
//!   fire a reaction.
//!
//! The [`Context`] is host-owned state; effects write it and
//! declarations name its keys. There is a single write path
//! ([`Context::set`]), matching the functional-core boundary: units must
//! not hold `&mut` host state directly — they operate on the context
//! through the effect closures the host runs.
//!
//! In this slice the values are the reactive signals a surface/layer/
//! render resource reads (theme, geometry, …), and the reaction is
//! marking the owning surface dirty so it repaints — *declared-reader
//! invalidation* replacing "repaint everything". The [`Shell`] in the
//! surface crate owns one [`Context`] and mounts a [`Component`] per
//! window (`create_window`→mount, `destroy_window`→unmount, `set`→
//! notify), so this mechanism is on the live production path, not a
//! detached store.
//!
//! Zero dependency: `[Any]`-typed values + set arithmetic, nothing else.

use std::any::Any;
use std::collections::{HashMap, HashSet};

/// A reactive key a unit can read (theme, geometry, …).
pub type Key = &'static str;

/// The standard keys the surface/render lifecycle reads.
pub const THEME: Key = "theme";
pub const GEOMETRY: Key = "geometry";

/// Undoes one committed effect. Owns whatever it needs to restore the
/// prior context state (e.g. the previous value).
pub type Inverse = Box<dyn FnOnce(&mut Context) + Send>;

/// A context mutation plus its inverse: `apply` runs at mount, returns
/// the inverse `unmount` replays.
pub type Effect = Box<dyn Fn(&mut Context) -> Inverse + Send>;

/// A declared reaction to a changed dependency.
pub type OnChange = Box<dyn FnMut(&mut Context, Key) + Send>;

/// A composition unit: what it reads (spatial), and what it commits
/// when mounted (temporal). Each effect's inverse is replayed on
/// unmount.
pub struct Component {
    /// Context keys this unit reads. Change one of these → this unit's
    /// `on_change` fires (and only these keys trigger it).
    pub reads: Vec<Key>,
    /// Effects applied in order at mount; each returns its inverse.
    pub effects: Vec<Effect>,
    /// Runs when a declared read key changes, with the changed key.
    pub on_change: Option<OnChange>,
}

impl Component {
    pub fn new(reads: Vec<Key>) -> Self {
        Self {
            reads,
            effects: Vec::new(),
            on_change: None,
        }
    }

    pub fn effect(mut self, apply: impl Fn(&mut Context) -> Inverse + Send + 'static) -> Self {
        self.effects.push(Box::new(apply));
        self
    }

    pub fn on_change(mut self, cb: impl FnMut(&mut Context, Key) + Send + 'static) -> Self {
        self.on_change = Some(Box::new(cb));
        self
    }
}

struct ScopeInner {
    reads: Vec<Key>,
    inverses: Vec<Inverse>,
    on_change: Option<OnChange>,
}

/// Host-owned keyed state. The single write path is [`Context::set`];
/// reads go through [`Context::get`]. Mount/unmount exercises the
/// spatiotemporal axes; a `set` on a declared key notifies exactly its
/// readers.
///
/// Reentrancy: a `set` issued from inside an `on_change` callback is not
/// dropped. It commits the value immediately (so later reads see it) and
/// is dispatched once the current reader returns to the drain loop. Each
/// key is dispatched at most once per drain, so a callback that keeps
/// re-setting the very key it was notified for cannot spin forever.
#[derive(Default)]
pub struct Context {
    values: HashMap<Key, Box<dyn Any + Send>>,
    readers: HashMap<Key, HashSet<usize>>,
    scopes: HashMap<usize, ScopeInner>,
    next_scope: usize,
    /// Keys awaiting notification, FIFO, deduplicated by `queued`.
    queue: std::collections::VecDeque<Key>,
    /// The keys currently in `queue`/already drained this pass — a key
    /// scheduled twice is delivered once (the no-loop guarantee).
    queued: HashSet<Key>,
    /// True while a drain loop is running; nested `set`s append to
    /// `queue` instead of starting a nested drain.
    draining: bool,
}

impl Context {
    pub fn new() -> Self {
        Self::default()
    }

    /// Read a value by key. `T` must match what the writer stored.
    pub fn get<T: Any + Send>(&self, key: Key) -> Option<&T> {
        self.values.get(key)?.downcast_ref::<T>()
    }

    /// True if a value exists for `key` (any type).
    pub fn has(&self, key: Key) -> bool {
        self.values.contains_key(key)
    }

    /// The single committed write path. Stores the value and notifies
    /// only the units that declared `key` in their `reads`.
    pub fn set<T: Any + Send>(&mut self, key: Key, value: T) {
        self.values.insert(key, Box::new(value));
        self.schedule(key);
    }

    /// Remove a value, notifying readers.
    pub fn remove(&mut self, key: Key) {
        if self.values.remove(&key).is_some() {
            self.schedule(key);
        }
    }

    /// The declared readers of `key`, ascending by component id, so the
    /// caller reacts in a deterministic order. Empty when nothing
    /// declared the key. This is the invariant the spatial axis
    /// guarantees: only these units' `on_change` fire in [`Self::set`].
    pub fn readers(&self, key: Key) -> Vec<usize> {
        let mut v: Vec<usize> = self
            .readers
            .get(key)
            .map(|set| set.iter().copied().collect())
            .unwrap_or_default();
        v.sort_unstable();
        v
    }

    /// Mount a unit: apply its effects, record their inverses,
    /// register its read keys and change reaction. Returns a handle
    /// for `unmount`.
    pub fn mount(&mut self, component: Component) -> usize {
        let id = self.next_scope;
        self.next_scope += 1;

        for key in &component.reads {
            self.readers.entry(*key).or_default().insert(id);
        }

        let mut inverses = Vec::with_capacity(component.effects.len());
        for effect in component.effects {
            inverses.push(effect(self));
        }

        self.scopes.insert(
            id,
            ScopeInner {
                reads: component.reads,
                inverses,
                on_change: component.on_change,
            },
        );
        id
    }

    /// Unmount: replay every recorded inverse in reverse order, then
    /// unregister reads. Context returns to pre-mount state. A no-op
    /// for an unknown handle (already gone).
    pub fn unmount(&mut self, id: usize) {
        let Some(inner) = self.scopes.remove(&id) else {
            return;
        };
        for key in &inner.reads {
            if let Some(set) = self.readers.get_mut(key) {
                set.remove(&id);
            }
        }
        for inverse in inner.inverses.into_iter().rev() {
            inverse(self);
        }
    }

    /// Queue `key` for notification and drain if no drain is active.
    fn schedule(&mut self, key: Key) {
        if self.queued.insert(key) {
            self.queue.push_back(key);
        }
        if !self.draining {
            self.drain();
        }
    }

    /// Dispatch every scheduled key, in FIFO order. Nested `set`s (from
    /// inside an `on_change`) enqueue more keys; each key is delivered
    /// at most once per drain.
    fn drain(&mut self) {
        self.draining = true;
        while let Some(current) = self.queue.pop_front() {
            let targets = self.readers(current);
            for id in targets {
                self.dispatch_one(id, current);
            }
        }
        self.draining = false;
        self.queue.clear();
        self.queued.clear();
    }

    /// Run one reader's `on_change`, if any, for `key`. Defensively
    /// tolerant of the callback having unmounted this very component
    /// (or a sibling) mid-notification: gone scopes are skipped, never
    /// unwrapped.
    fn dispatch_one(&mut self, id: usize, key: Key) {
        let Some(inner) = self.scopes.get_mut(&id) else {
            return; // unmounted mid-notify
        };
        let Some(mut cb) = inner.on_change.take() else {
            return;
        };
        (cb)(self, key);
        // Restore the callback only if this component still exists (its
        // own `on_change` may have unmounted it).
        if let Some(inner) = self.scopes.get_mut(&id) {
            inner.on_change = Some(cb);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const THEME: Key = "theme";
    const MODE: Key = "mode";
    const BACKGROUND: Key = "background";

    /// Canonical temporal check: snapshot, mount, exercise every effect,
    /// unmount, diff — must be empty. No leaked values, readers, or scopes.
    #[test]
    fn unmount_reverts_every_effect() {
        let mut ctx = Context::new();
        ctx.set(THEME, String::from("dark"));

        let snapshot = ctx.values.len() + ctx.readers.len() + ctx.scopes.len();

        let id = ctx.mount(
            Component::new(vec![])
                .effect(|c| {
                    let old = c.get::<String>(GEOMETRY).cloned();
                    c.set(GEOMETRY, String::from("0,0 800x600"));
                    Box::new(move |c| match old {
                        Some(prev) => c.set(GEOMETRY, prev),
                        None => c.remove(GEOMETRY),
                    })
                })
                .effect(|c| {
                    let old = c.get::<String>(THEME).cloned();
                    c.set(THEME, String::from("#111122"));
                    Box::new(move |c| match old {
                        Some(prev) => c.set(THEME, prev),
                        None => c.remove(THEME),
                    })
                }),
        );

        assert!(ctx.has(GEOMETRY));
        assert_eq!(
            ctx.get::<String>(THEME).map(String::as_str),
            Some("#111122")
        );

        ctx.unmount(id);

        assert_eq!(
            ctx.values.len() + ctx.readers.len() + ctx.scopes.len(),
            snapshot,
            "residue after unmount"
        );
        assert!(!ctx.has(GEOMETRY));
        assert_eq!(ctx.get::<String>(THEME).map(String::as_str), Some("dark"));
    }

    /// Canonical spatial check: change each declared key, confirm
    /// exactly its readers react; undeclared key changes must not.
    #[test]
    fn spatial_notifies_only_declared_readers() {
        let mut ctx = Context::new();

        let theme_hits = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let geom_hits = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));

        let t = theme_hits.clone();
        ctx.mount(
            Component::new(vec![THEME])
                .effect(|_| Box::new(|_| {}))
                .on_change(move |_, k| {
                    assert_eq!(k, THEME, "theme reader got non-theme key");
                    t.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                }),
        );
        let g = geom_hits.clone();
        ctx.mount(
            Component::new(vec![GEOMETRY])
                .effect(|_| Box::new(|_| {}))
                .on_change(move |_, k| {
                    assert_eq!(k, GEOMETRY, "geometry reader got non-geometry key");
                    g.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                }),
        );

        ctx.set(GEOMETRY, "0,0 800x600");
        assert_eq!(
            theme_hits.load(std::sync::atomic::Ordering::Relaxed),
            0,
            "theme reader fired on geometry change"
        );
        assert_eq!(geom_hits.load(std::sync::atomic::Ordering::Relaxed), 1);

        ctx.set(THEME, "light");
        assert_eq!(theme_hits.load(std::sync::atomic::Ordering::Relaxed), 1);
        assert_eq!(
            geom_hits.load(std::sync::atomic::Ordering::Relaxed),
            1,
            "geometry reader fired on theme change"
        );
        // An undeclared key: no reader fires.
        ctx.set(BACKGROUND, 7_u8);
        assert_eq!(theme_hits.load(std::sync::atomic::Ordering::Relaxed), 1);
        assert_eq!(geom_hits.load(std::sync::atomic::Ordering::Relaxed), 1);
    }

    /// Two mounts of the same key must not collide: each inverse is
    /// independent, and the second's replay restores the first's value.
    #[test]
    fn overlapping_effects_unmount_independently() {
        let mut ctx = Context::new();
        let a = ctx.mount(Component::new(vec![]).effect(|c| {
            let old = c.get::<&str>(THEME).copied();
            c.set(THEME, "a");
            Box::new(move |c| match old {
                Some(p) => c.set(THEME, p),
                None => c.remove(THEME),
            })
        }));
        let b = ctx.mount(Component::new(vec![]).effect(|c| {
            let old = c.get::<&str>(THEME).copied();
            c.set(THEME, "b");
            Box::new(move |c| match old {
                Some(p) => c.set(THEME, p),
                None => c.remove(THEME),
            })
        }));

        assert_eq!(ctx.get::<&str>(THEME), Some(&"b"));
        ctx.unmount(b);
        assert_eq!(ctx.get::<&str>(THEME), Some(&"a"));
        ctx.unmount(a);
        assert!(!ctx.has(THEME));
    }

    /// An unmount of an unknown handle is a no-op (already gone).
    #[test]
    fn unmount_unknown_id_is_noop() {
        let mut ctx = Context::new();
        ctx.unmount(42); // must not panic
        assert_eq!(ctx.scopes.len(), 0);
    }

    /// Reader order is deterministic (ascending component id), which is
    /// what `readers` promises.
    #[test]
    fn readers_are_sorted_by_component_id() {
        let mut ctx = Context::new();
        let a = ctx.mount(Component::new(vec![THEME]).effect(|_| Box::new(|_| {})));
        let c = ctx.mount(Component::new(vec![THEME]).effect(|_| Box::new(|_| {})));
        let b = ctx.mount(Component::new(vec![THEME]).effect(|_| Box::new(|_| {})));
        // Mount order was a(0), c(1), b(2); a reader query must not return
        // insertion order — it sorts by component id (a, c, b).
        assert_eq!(ctx.readers(THEME), vec![a, c, b]);
        assert!(ctx.readers(BACKGROUND).is_empty());
    }

    /// A reader whose `on_change` unmounts a *later* reader of the same
    /// key must not panic: the later target is gone by the time the pass
    /// reaches it, and is simply skipped (survivors still fire).
    #[test]
    fn unmount_during_notify_does_not_panic() {
        let mut ctx = Context::new();

        let survivor = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        // The bomber is the *lowest* id (id 0), so it dispatches first
        // and unmounts a higher-id reader (id 1) *mid-pass* — exactly
        // the case the old iterate-then-unwrap code panicked on.
        let victim_cell = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let bomber = ctx.mount(
            Component::new(vec![THEME])
                .effect(|_| Box::new(|_| {}))
                .on_change({
                    let victim_cell = victim_cell.clone();
                    move |ctx, _| {
                        let v = victim_cell.load(std::sync::atomic::Ordering::Relaxed);
                        ctx.unmount(v);
                    }
                }),
        );
        // Victim id 1 — will be gone before the pass reaches it.
        let victim = ctx.mount(Component::new(vec![THEME]).effect(|_| Box::new(|_| {})));
        victim_cell.store(victim, std::sync::atomic::Ordering::Relaxed);
        assert_eq!(bomber, 0, "bomber is the lowest id");

        let s = survivor.clone();
        ctx.mount(
            Component::new(vec![THEME])
                .effect(|_| Box::new(|_| {}))
                .on_change(move |_, _| {
                    s.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                }),
        );

        ctx.set(THEME, "dark");
        // No panic; the unmounted reader is skipped; survivors fired.
        assert_eq!(ctx.readers(THEME).len(), 2, "one reader was unmounted");
        assert_eq!(
            survivor.load(std::sync::atomic::Ordering::Relaxed),
            1,
            "survivor still notified"
        );
    }

    /// A reentrant `set` from inside an `on_change` must not be silently
    /// dropped: the value commits and the *new* key's readers are
    /// notified after the current pass.
    #[test]
    fn reentrant_set_notifies_new_key_readers() {
        let mut ctx = Context::new();

        let mode_hits = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let theme_hits = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));

        let m = mode_hits.clone();
        ctx.mount(
            Component::new(vec![MODE])
                .effect(|_| Box::new(|_| {}))
                .on_change(move |c, _| {
                    // Nested committed set on a *different* key.
                    c.set(THEME, "propagated");
                    m.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                }),
        );
        let t = theme_hits.clone();
        ctx.mount(
            Component::new(vec![THEME])
                .effect(|_| Box::new(|_| {}))
                .on_change(move |_, k| {
                    assert_eq!(k, THEME);
                    t.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                }),
        );

        ctx.set(MODE, "command");
        assert_eq!(mode_hits.load(std::sync::atomic::Ordering::Relaxed), 1);
        assert_eq!(
            theme_hits.load(std::sync::atomic::Ordering::Relaxed),
            1,
            "reentrant theme set must reach the theme reader"
        );
        assert_eq!(ctx.get::<&str>(THEME), Some(&"propagated"));
    }

    /// A callback that re-sets the *same* key it was notified for must
    /// not spin: the key is dispatched once per pass.
    #[test]
    fn self_resetting_reader_does_not_loop() {
        let mut ctx = Context::new();
        let count = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let c = count.clone();
        ctx.mount(
            Component::new(vec![THEME])
                .effect(|_| Box::new(|_| {}))
                .on_change(move |ctx, _| {
                    ctx.set(THEME, "again"); // would loop if re-queued
                    c.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                }),
        );
        ctx.set(THEME, "v1");
        assert_eq!(count.load(std::sync::atomic::Ordering::Relaxed), 1);
        assert_eq!(ctx.get::<&str>(THEME), Some(&"again"));
    }
}
