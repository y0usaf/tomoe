//! The moonshell binary: config resolution and the calloop bootstrap.
//!
//! With a config (`--config`, `$MOONSHELL_CONFIG`, or
//! `~/.config/moonshell/init.lua`) it boots the Lua runtime, executes
//! the config, and drains the runtime's action queue — window
//! creations, timers to arm, dirty marks, the quit flag — after config
//! exec and once per loop pass ([`Shell::run_with`]). `exec_async`
//! replies arrive over a calloop channel; the display snapshot behind
//! `shell.displays()` is refreshed each drain. With no config it maps one layer surface and
//! draws a version string: the doctrine-06 bare-core artifact.
//! `--boot-check` exits 0 right after every window committed its first
//! frame, which is what `nix flake check` runs under a headless
//! compositor.

use std::cell::RefCell;
use std::path::PathBuf;
use std::rc::Rc;

use anyhow::Context as _;
use calloop::timer::{TimeoutAction, Timer};
use moonshell_render::element::{Align, Edges, Flex, Spacer, Style, Text};
use moonshell_render::{Element, Renderer, Rgba, Scene, SceneDamage};
use moonshell_runtime::{LuaPainter, ShellCtx, Vm};
use moonshell_surface::{Canvas, Damage, DamageRect, Edge, LayerOptions, Painter, Shell};

const BG: Rgba = Rgba::new(0x14, 0x14, 0x1e, 0xff);
const FG: Rgba = Rgba::new(0xc8, 0xc8, 0xd8, 0xff);

struct VersionBar {
    renderer: Renderer,
    scene: Scene,
    root: Element,
}

impl VersionBar {
    fn new(label: String) -> Self {
        // The bare tree: bg + padding + a centered version string —
        // exercises the M1 element/layout/draw path with zero policy.
        let root = Element::HBox(Flex {
            style: Style {
                bg: Some(BG),
                ..Style::default()
            },
            padding: Edges {
                left: 8.0,
                right: 8.0,
                ..Edges::default()
            },
            align: Align::Center,
            children: vec![
                Element::Text(Text {
                    content: label,
                    size: 13.0,
                    color: FG,
                    ..Text::default()
                }),
                Element::Spacer(Spacer::default()),
            ],
            ..Flex::default()
        });
        Self {
            renderer: Renderer::new(),
            scene: Scene::new(),
            root,
        }
    }
}

impl Painter for VersionBar {
    fn paint(&mut self, canvas: Canvas<'_>) -> Damage {
        let Canvas {
            buf,
            width,
            height,
            scale,
            fresh,
        } = canvas;
        if fresh {
            // No prior content on this surface at this size — the diff
            // baseline is gone.
            self.scene.invalidate();
        }
        let damage = self.scene.render(
            &mut self.renderer,
            buf,
            width,
            height,
            scale as f32,
            &self.root,
        );
        match damage {
            SceneDamage::None => Damage::None,
            SceneDamage::Full => Damage::Full,
            SceneDamage::Rects(rects) => Damage::Rects(
                rects
                    .into_iter()
                    .map(|r| DamageRect {
                        x: r.x,
                        y: r.y,
                        width: r.w,
                        height: r.h,
                    })
                    .collect(),
            ),
        }
    }
}

/// Locate the config: explicit `--config` and `$MOONSHELL_CONFIG` must
/// exist (a typo should fail loudly); the default location is optional
/// (absent = the bare version bar).
fn resolve_config(cli: Option<PathBuf>) -> anyhow::Result<Option<PathBuf>> {
    if let Some(p) = cli {
        anyhow::ensure!(p.is_file(), "--config: no such file: {}", p.display());
        return Ok(Some(p));
    }
    if let Some(p) = std::env::var_os("MOONSHELL_CONFIG") {
        let p = PathBuf::from(p);
        anyhow::ensure!(
            p.is_file(),
            "$MOONSHELL_CONFIG: no such file: {}",
            p.display()
        );
        return Ok(Some(p));
    }
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")));
    Ok(base
        .map(|b| b.join("moonshell").join("init.lua"))
        .filter(|p| p.is_file()))
}

/// mlua's error is `!Send + !Sync`; this is the one Lua→anyhow boundary.
fn lua_err(e: moonshell_runtime::mlua::Error) -> anyhow::Error {
    anyhow::anyhow!("{e}")
}

fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("moonshell=info")),
        )
        .init();

    let mut boot_check = false;
    let mut config_arg: Option<PathBuf> = None;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--boot-check" => boot_check = true,
            "--config" => {
                config_arg = Some(args.next().context("--config needs a path")?.into());
            }
            "--version" | "-V" => {
                println!("moonshell {}", env!("CARGO_PKG_VERSION"));
                return Ok(());
            }
            other => {
                anyhow::bail!("unknown argument: {other} (try --version, --config, --boot-check)")
            }
        }
    }

    let config = resolve_config(config_arg)?;
    let (mut shell, event_loop) = Shell::connect()?;
    shell.exit_after_first_draw = boot_check;

    let Some(path) = config else {
        // Bare core (doctrine 06): no policy, still boots.
        let painter = VersionBar::new(format!("moonshell {}", env!("CARGO_PKG_VERSION")));
        shell.create_window(LayerOptions::bar(Edge::Top, 32, true), Box::new(painter));
        shell.run(event_loop)?;
        return Ok(());
    };

    tracing::info!(config = %path.display(), "loading config");
    let vm = Vm::new().map_err(lua_err)?;
    let ctx = ShellCtx::new();
    vm.install_shell(&ctx).map_err(lua_err)?;
    let code = std::fs::read_to_string(&path)
        .with_context(|| format!("reading config {}", path.display()))?;
    // Outputs are known (connect roundtrips) — snapshot them before the
    // config runs so top-level `shell.displays()` sees real geometry.
    ctx.set_displays(shell.displays());
    vm.exec(&code, &path.to_string_lossy()).map_err(lua_err)?;

    // exec_async replies land on the loop thread through this channel;
    // idle it schedules nothing. Inserted once, before any Lua runs —
    // a reply can never beat its source.
    let exec_channel = ctx
        .take_exec_channel()
        .context("exec channel already taken")?;
    let exec_ctx = ctx.clone();
    event_loop
        .handle()
        .insert_source(exec_channel, move |event, _, _shell: &mut Shell| {
            if let calloop::channel::Event::Msg(reply) = event {
                // May queue windows / mark dirty; the tick after this
                // dispatch pass drains.
                exec_ctx.dispatch_exec_reply(reply);
            }
        })
        .map_err(|e| anyhow::anyhow!("inserting exec channel: {e}"))?;

    // One renderer for every window: the font system and glyph caches
    // are the dominant allocation — shared, not per-window.
    let renderer = Rc::new(RefCell::new(Renderer::new()));
    let lua = vm.lua().clone();
    let loop_handle = event_loop.handle();
    let drain = move |shell: &mut Shell| {
        // Snapshot in (displays), actions out (everything below).
        ctx.set_displays(shell.displays());
        for p in ctx.take_pending() {
            let painter = LuaPainter::new(lua.clone(), p.shared, renderer.clone());
            shell.create_window(p.options, Box::new(painter));
        }
        for t in ctx.take_timers() {
            // Armed only because a shell.interval/once exists — the
            // zero-idle-wakeup discipline. `fire` returning false means
            // the VM is gone; the source removes itself.
            let timer = Timer::from_duration(t.delay);
            let inserted =
                loop_handle.insert_source(timer, move |_, _, _shell: &mut Shell| {
                    match (t.fire(), t.period) {
                        (true, Some(period)) => TimeoutAction::ToDuration(period),
                        _ => TimeoutAction::Drop,
                    }
                });
            if let Err(e) = inserted {
                tracing::error!("inserting timer: {e}");
            }
        }
        if ctx.take_dirty() {
            shell.mark_all_dirty();
        }
        if ctx.take_quit() {
            shell.quit();
        }
    };
    // Pre-run drain so the no-window warning is accurate; run_with
    // ticks again (idempotent — the queues are drained) before its
    // first dispatch, so a config-time `shell.quit()` exits cleanly.
    drain(&mut shell);
    if shell.window_count() == 0 {
        tracing::warn!("config created no windows (shell.window was never called)");
    }
    shell.run_with(event_loop, drain)?;
    drop(vm); // outlives the loop: painters hold Lua clones
    Ok(())
}
