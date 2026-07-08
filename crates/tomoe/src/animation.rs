//! Animation engine: springs + easing curves driving render-time window
//! offsets and opacity (M6; niri's `animation` module with the adjustable
//! clock stripped — every query takes an explicit `now`, the compositor's
//! `start_time.elapsed()` timebase).
//!
//! Doctrine split: Lua policy sets *target* geometry (layout); the core
//! animates the **rendered** position/alpha toward it. Layout, hit-testing,
//! and the Lua snapshot always see the target — animations are transient
//! presentation, invisible to policy. Rendered offsets are rounded onto the
//! integer physical grid per the coordinate doctrine, so client buffers stay
//! 1:1 on every animation frame.

use std::collections::HashMap;
use std::time::Duration;

use smithay::desktop::Window;
use smithay::utils::{IsAlive, Physical, Point};

// ─── Cubic bezier (niri's port of libadwaita's adw-easing) ──────────────────

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CubicBezier {
    x1: f64,
    y1: f64,
    x2: f64,
    y2: f64,
}

impl CubicBezier {
    pub fn new(x1: f64, y1: f64, x2: f64, y2: f64) -> Self {
        Self { x1, y1, x2, y2 }
    }

    fn x_for_t(&self, t: f64) -> f64 {
        let omt = 1. - t;
        3. * omt * omt * t * self.x1 + 3. * omt * t * t * self.x2 + t * t * t
    }

    fn y_for_t(&self, t: f64) -> f64 {
        let omt = 1. - t;
        3. * omt * omt * t * self.y1 + 3. * omt * t * t * self.y2 + t * t * t
    }

    fn t_for_x(&self, x: f64) -> f64 {
        let mut min_t = 0.;
        let mut max_t = 1.;
        for _ in 0..=30 {
            let guess_t = (min_t + max_t) / 2.;
            if x < self.x_for_t(guess_t) {
                max_t = guess_t;
            } else {
                min_t = guess_t;
            }
        }
        (min_t + max_t) / 2.
    }

    pub fn y(&self, x: f64) -> f64 {
        if x <= f64::EPSILON {
            return 0.;
        }
        if 1. - f64::EPSILON <= x {
            return 1.;
        }
        self.y_for_t(self.t_for_x(x))
    }
}

// ─── Easing curves ───────────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Curve {
    Linear,
    EaseOutQuad,
    EaseOutCubic,
    EaseOutExpo,
    CubicBezier(CubicBezier),
}

impl Curve {
    pub fn y(self, x: f64) -> f64 {
        match self {
            Curve::Linear => x,
            Curve::EaseOutQuad => 1. - (1. - x) * (1. - x),
            Curve::EaseOutCubic => 1. - (1. - x).powi(3),
            Curve::EaseOutExpo => 1. - 2f64.powf(-10. * x),
            Curve::CubicBezier(b) => b.y(x),
        }
    }
}

// ─── Spring (niri's port of libadwaita's adw-spring-animation) ──────────────

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SpringParams {
    pub damping: f64,
    pub mass: f64,
    pub stiffness: f64,
    pub epsilon: f64,
}

impl SpringParams {
    pub fn new(damping_ratio: f64, stiffness: f64, epsilon: f64) -> Self {
        let damping_ratio = damping_ratio.max(0.);
        let stiffness = stiffness.max(0.);
        let epsilon = epsilon.max(0.);
        let mass = 1.;
        let critical_damping = 2. * (mass * stiffness).sqrt();
        Self {
            damping: damping_ratio * critical_damping,
            mass,
            stiffness,
            epsilon,
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub struct Spring {
    pub from: f64,
    pub to: f64,
    pub initial_velocity: f64,
    pub params: SpringParams,
}

impl Spring {
    pub fn value_at(&self, t: Duration) -> f64 {
        self.oscillate(t.as_secs_f64())
    }

    /// Duration until the spring is at rest (best effort).
    pub fn duration(&self) -> Duration {
        const DELTA: f64 = 0.001;

        let beta = self.params.damping / (2. * self.params.mass);
        if beta.abs() <= f64::EPSILON || beta < 0. {
            return Duration::MAX;
        }
        if (self.to - self.from).abs() <= f64::EPSILON {
            return Duration::ZERO;
        }

        let omega0 = (self.params.stiffness / self.params.mass).sqrt();

        // First ansatz for the overdamped solution and general estimation
        // for the oscillating ones: the envelope's value when < epsilon.
        let mut x0 = -self.params.epsilon.ln() / beta;

        // f64::EPSILON is too small for this comparison (see niri/adwaita).
        if (beta - omega0).abs() <= f64::from(f32::EPSILON) || beta < omega0 {
            return Duration::from_secs_f64(x0);
        }

        // Overdamped: Newton's root finding on the oscillation itself.
        let mut y0 = self.oscillate(x0);
        let m = (self.oscillate(x0 + DELTA) - y0) / DELTA;
        let mut x1 = (self.to - y0 + m * x0) / m;
        let mut y1 = self.oscillate(x1);

        let mut i = 0;
        while (self.to - y1).abs() > self.params.epsilon {
            if i > 1000 {
                return Duration::ZERO;
            }
            x0 = x1;
            y0 = y1;

            let m = (self.oscillate(x0 + DELTA) - y0) / DELTA;
            x1 = (self.to - y0 + m * x0) / m;
            y1 = self.oscillate(x1);

            // Overdamped springs have numerical stability issues.
            if !y1.is_finite() {
                return Duration::from_secs_f64(x0);
            }
            i += 1;
        }

        Duration::from_secs_f64(x1)
    }

    /// Spring position at time `t` (seconds): the analytic solution of
    /// m·ẍ + b·ẋ + k·x = 0 for the critically-/under-/overdamped cases.
    fn oscillate(&self, t: f64) -> f64 {
        let b = self.params.damping;
        let m = self.params.mass;
        let k = self.params.stiffness;
        let v0 = self.initial_velocity;

        let beta = b / (2. * m);
        let omega0 = (k / m).sqrt();
        let x0 = self.from - self.to;
        let envelope = (-beta * t).exp();

        if (beta - omega0).abs() <= f64::from(f32::EPSILON) {
            // Critically damped.
            self.to + envelope * (x0 + (beta * x0 + v0) * t)
        } else if beta < omega0 {
            // Underdamped.
            let omega1 = ((omega0 * omega0) - (beta * beta)).sqrt();
            self.to
                + envelope
                    * (x0 * (omega1 * t).cos() + ((beta * x0 + v0) / omega1) * (omega1 * t).sin())
        } else {
            // Overdamped.
            let omega2 = ((beta * beta) - (omega0 * omega0)).sqrt();
            self.to
                + envelope
                    * (x0 * (omega2 * t).cosh() + ((beta * x0 + v0) / omega2) * (omega2 * t).sinh())
        }
    }
}

// ─── Config (what `settings.animations` parses into) ────────────────────────

/// One animatable property's configuration.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Config {
    Off,
    Spring(SpringParams),
    Easing { duration: Duration, curve: Curve },
}

/// `settings.animations`: per-property animation configs.
#[derive(Debug, Clone, PartialEq)]
pub struct AnimationSettings {
    /// Window position changes (Lua `set_geometry` on a mapped window).
    pub window_move: Config,
    /// Window map/show: opacity fade-in.
    pub window_open: Config,
}

impl Default for AnimationSettings {
    /// niri's defaults: movement = spring (damping ratio 1.0, stiffness 800,
    /// epsilon 0.0001), open = 150 ms ease-out-expo.
    fn default() -> Self {
        Self {
            window_move: Config::Spring(SpringParams::new(1.0, 800.0, 0.0001)),
            window_open: Config::Easing {
                duration: Duration::from_millis(150),
                curve: Curve::EaseOutExpo,
            },
        }
    }
}

impl AnimationSettings {
    pub fn off() -> Self {
        Self {
            window_move: Config::Off,
            window_open: Config::Off,
        }
    }
}

// ─── Animation: one scalar value over time ───────────────────────────────────

#[derive(Debug, Clone)]
pub struct Animation {
    from: f64,
    to: f64,
    start: Duration,
    duration: Duration,
    kind: Kind,
}

#[derive(Debug, Clone, Copy)]
enum Kind {
    Easing { curve: Curve },
    Spring(Spring),
}

impl Animation {
    /// Start an animation from `from` to `to` at time `now`. `None` when the
    /// config says off — the value snaps to `to` immediately.
    pub fn new(config: Config, from: f64, to: f64, now: Duration) -> Option<Self> {
        match config {
            Config::Off => None,
            Config::Spring(params) => {
                let spring = Spring {
                    from,
                    to,
                    initial_velocity: 0.,
                    params,
                };
                Some(Self {
                    from,
                    to,
                    start: now,
                    duration: spring.duration(),
                    kind: Kind::Spring(spring),
                })
            }
            Config::Easing { duration, curve } => Some(Self {
                from,
                to,
                start: now,
                duration,
                kind: Kind::Easing { curve },
            }),
        }
    }

    pub fn is_done(&self, now: Duration) -> bool {
        now >= self.start.saturating_add(self.duration)
    }

    pub fn value(&self, now: Duration) -> f64 {
        if now <= self.start {
            return self.from;
        }
        if self.is_done(now) {
            return self.to;
        }
        let passed = now.saturating_sub(self.start);

        match self.kind {
            Kind::Easing { curve } => {
                let x = (passed.as_secs_f64() / self.duration.as_secs_f64()).clamp(0., 1.);
                curve.y(x) * (self.to - self.from) + self.from
            }
            Kind::Spring(spring) => {
                let value = spring.value_at(passed);
                // Protect against numerical instability (niri).
                let range = (self.to - self.from) * 10.;
                let a = self.from - range;
                let b = self.to + range;
                if self.from <= self.to {
                    value.clamp(a, b)
                } else {
                    value.clamp(b, a)
                }
            }
        }
    }
}

// ─── Per-window animation state ──────────────────────────────────────────────

#[derive(Debug, Default)]
struct WindowAnims {
    /// Render offset from the layout target, animating to zero: the offset
    /// vector is `start` scaled by the animation's 1 → 0 progress.
    move_from: (f64, f64),
    move_anim: Option<Animation>,
    /// Window alpha animating 0 → 1 on map/show.
    open: Option<Animation>,
}

/// Window-keyed animation state. Rendering asks [`offset`](Self::offset) /
/// [`alpha`](Self::alpha) per frame; [`advance`](Self::advance) prunes
/// finished animations and reports whether any remain (drives redraw
/// keepalive: the backends re-queue while this is true).
// Window keys hash by their stable id despite interior mutability.
#[allow(clippy::mutable_key_type)]
#[derive(Debug, Default)]
pub struct Animations {
    windows: HashMap<Window, WindowAnims>,
}

impl Animations {
    /// The window's layout target moved by `delta` (old − new): keep the
    /// rendered position where it was and animate the remaining offset to
    /// zero. Retargeting composes — a move during a move starts from the
    /// current rendered offset, so the window never jumps.
    pub fn start_move(
        &mut self,
        window: &Window,
        delta: Point<i32, Physical>,
        config: Config,
        now: Duration,
    ) {
        let current = self.offset_f64(window, now);
        let from = (current.0 + delta.x as f64, current.1 + delta.y as f64);
        let entry = self.windows.entry(window.clone()).or_default();
        match Animation::new(config, 1.0, 0.0, now) {
            Some(anim) if from != (0.0, 0.0) => {
                entry.move_from = from;
                entry.move_anim = Some(anim);
            }
            _ => {
                entry.move_from = (0.0, 0.0);
                entry.move_anim = None;
            }
        }
    }

    /// The window just mapped (open) or re-mapped (show): fade alpha 0 → 1.
    pub fn start_open(&mut self, window: &Window, config: Config, now: Duration) {
        let entry = self.windows.entry(window.clone()).or_default();
        entry.open = Animation::new(config, 0.0, 1.0, now);
    }

    pub fn remove(&mut self, window: &Window) {
        self.windows.remove(window);
    }

    fn offset_f64(&self, window: &Window, now: Duration) -> (f64, f64) {
        let Some(anims) = self.windows.get(window) else {
            return (0.0, 0.0);
        };
        let Some(anim) = &anims.move_anim else {
            return (0.0, 0.0);
        };
        let progress = anim.value(now);
        (anims.move_from.0 * progress, anims.move_from.1 * progress)
    }

    /// Rendered offset from the layout position, rounded onto the integer
    /// physical grid (coordinate doctrine: no fractional element positions).
    pub fn offset(&self, window: &Window, now: Duration) -> Point<i32, Physical> {
        let (x, y) = self.offset_f64(window, now);
        Point::from((x.round() as i32, y.round() as i32))
    }

    /// Rendered window alpha (1.0 when not fading).
    pub fn alpha(&self, window: &Window, now: Duration) -> f32 {
        self.windows
            .get(window)
            .and_then(|anims| anims.open.as_ref())
            .map_or(1.0, |anim| anim.value(now).clamp(0.0, 1.0) as f32)
    }

    /// Prune finished animations and dead windows; true while any animation
    /// is still running (the caller keeps the redraw loop alive).
    pub fn advance(&mut self, now: Duration) -> bool {
        self.windows.retain(|window, anims| {
            if !window.alive() {
                return false;
            }
            if anims.move_anim.as_ref().is_some_and(|a| a.is_done(now)) {
                anims.move_from = (0.0, 0.0);
                anims.move_anim = None;
            }
            if anims.open.as_ref().is_some_and(|a| a.is_done(now)) {
                anims.open = None;
            }
            anims.move_anim.is_some() || anims.open.is_some()
        });
        !self.windows.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MS: Duration = Duration::from_millis(1);

    fn ease_linear(ms: u64) -> Config {
        Config::Easing {
            duration: Duration::from_millis(ms),
            curve: Curve::Linear,
        }
    }

    #[test]
    fn overdamped_spring_equal_from_to_nan() {
        let spring = Spring {
            from: 0.,
            to: 0.,
            initial_velocity: 0.,
            params: SpringParams::new(1.15, 850., 0.0001),
        };
        let _ = spring.duration();
        let _ = spring.value_at(Duration::ZERO);
    }

    #[test]
    fn overdamped_spring_duration_panic() {
        let spring = Spring {
            from: 0.,
            to: 1.,
            initial_velocity: 0.,
            params: SpringParams::new(6., 1200., 0.0001),
        };
        let _ = spring.duration();
        let _ = spring.value_at(Duration::ZERO);
    }

    #[test]
    fn easing_hits_endpoints() {
        for curve in [
            Curve::Linear,
            Curve::EaseOutQuad,
            Curve::EaseOutCubic,
            Curve::EaseOutExpo,
            Curve::CubicBezier(CubicBezier::new(0.25, 0.1, 0.25, 1.0)),
        ] {
            let anim = Animation::new(
                Config::Easing {
                    duration: Duration::from_millis(100),
                    curve,
                },
                3.0,
                7.0,
                Duration::ZERO,
            )
            .unwrap();
            assert_eq!(anim.value(Duration::ZERO), 3.0, "{curve:?}");
            assert_eq!(anim.value(Duration::from_millis(100)), 7.0, "{curve:?}");
            let mid = anim.value(Duration::from_millis(50));
            assert!((3.0..=7.0).contains(&mid), "{curve:?}: {mid}");
        }
    }

    #[test]
    fn spring_animation_settles_at_target() {
        let anim = Animation::new(
            Config::Spring(SpringParams::new(1.0, 800.0, 0.0001)),
            0.0,
            1.0,
            Duration::ZERO,
        )
        .unwrap();
        let end = anim.value(Duration::from_secs(10));
        assert!((end - 1.0).abs() < 1e-6);
        assert!(anim.is_done(Duration::from_secs(10)));
    }

    #[test]
    fn off_config_yields_no_animation() {
        assert!(Animation::new(Config::Off, 0.0, 1.0, Duration::ZERO).is_none());
    }

    #[test]
    fn move_offset_decays_to_zero() {
        // Drive WindowAnims directly (constructing a smithay Window needs a
        // live protocol object): same math offset()/alpha() run per window.
        let mut anims = WindowAnims {
            move_from: (100.0, -40.0),
            move_anim: Animation::new(ease_linear(100), 1.0, 0.0, Duration::ZERO),
            open: None,
        };
        let anim = anims.move_anim.as_ref().unwrap();
        assert_eq!(anim.value(Duration::ZERO), 1.0);
        let half = anim.value(50 * MS);
        assert!((half - 0.5).abs() < 1e-9);
        assert_eq!(anim.value(100 * MS), 0.0);
        assert!(anim.is_done(100 * MS));
        anims.move_anim = None;
        assert!(anims.move_anim.is_none() && anims.open.is_none());
    }

    #[test]
    fn retarget_composes_current_offset_with_delta() {
        // Manual composition mirror of start_move: current offset +50 at
        // half-way, target moves by another −30 → new start offset is +20.
        let anim = Animation::new(ease_linear(100), 1.0, 0.0, Duration::ZERO).unwrap();
        let current = 100.0 * anim.value(50 * MS);
        let new_from = current + (-30.0);
        assert!((new_from - 20.0).abs() < 1e-9);
    }
}
