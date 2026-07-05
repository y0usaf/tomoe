//! XWayland via xwayland-satellite.
//!
//! tomoe owns the X11 display: it picks a display number, creates the lock
//! file and listening sockets itself, and exports `DISPLAY` immediately —
//! before any satellite process exists. The sockets are watched on the event
//! loop; the first client connection spawns `xwayland-satellite`, handing it
//! the live listening fds via `-listenfd`. Queued connections survive the
//! handoff (kernel backlog), so there is no readiness handshake. When
//! satellite exits, the watch re-arms and the next connection respawns it.

use std::io::Write as _;
use std::os::fd::{AsRawFd as _, BorrowedFd, OwnedFd};
use std::os::linux::net::SocketAddrExt as _;
use std::os::unix::fs::{DirBuilderExt as _, MetadataExt as _, OpenOptionsExt as _};
use std::os::unix::net::{SocketAddr, UnixListener};
use std::os::unix::process::CommandExt as _;
use std::process::{Command, Stdio};
use std::thread;

use anyhow::{anyhow, ensure, Context, Result};
use smithay::reexports::calloop::channel::Sender;
use smithay::reexports::calloop::generic::Generic;
use smithay::reexports::calloop::{Interest, Mode, PostAction, RegistrationToken};
use smithay::reexports::rustix::io::{fcntl_setfd, FdFlags};
use tracing::{debug, warn};

use crate::state::Tomoe;

const SATELLITE: &str = "xwayland-satellite";
const X11_TMP_UNIX_DIR: &str = "/tmp/.X11-unix";

pub struct Satellite {
    x11: X11Connection,
    abstract_token: Option<RegistrationToken>,
    unix_token: Option<RegistrationToken>,
    to_main: Sender<ToMain>,
}

enum ToMain {
    SetupWatch,
}

impl Satellite {
    pub fn display_name(&self) -> &str {
        &self.x11.display_name
    }
}

struct X11Connection {
    display_name: String,
    abstract_fd: OwnedFd,
    unix_fd: OwnedFd,
    _unix_guard: Unlink,
    _lock_guard: Unlink,
}

struct Unlink(String);
impl Drop for Unlink {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

/// Set up the X11 sockets and the on-demand satellite watch. Idempotent; a
/// missing or too-old xwayland-satellite binary disables the integration
/// with a warning (X11 apps just won't run).
pub fn setup(tomoe: &mut Tomoe) {
    if tomoe.satellite.is_some() {
        return;
    }

    if !test_ondemand() {
        return;
    }

    let x11 = match setup_connection() {
        Ok(x11) => x11,
        Err(err) => {
            warn!("error opening X11 sockets, disabling xwayland-satellite integration: {err:#}");
            return;
        }
    };

    let (to_main, rx) = smithay::reexports::calloop::channel::channel();
    let res = tomoe
        .loop_handle
        .insert_source(rx, move |event, _, tomoe| match event {
            smithay::reexports::calloop::channel::Event::Msg(ToMain::SetupWatch) => {
                setup_watch(tomoe)
            }
            smithay::reexports::calloop::channel::Event::Closed => (),
        });
    if let Err(err) = res {
        warn!("error inserting satellite channel source: {err}");
        return;
    }

    tomoe.satellite = Some(Satellite {
        x11,
        abstract_token: None,
        unix_token: None,
        to_main,
    });

    setup_watch(tomoe);
}

/// Probe that the installed binary supports `-listenfd` on-demand spawning.
fn test_ondemand() -> bool {
    let mut process = Command::new(SATELLITE);
    process
        .args([":0", "--test-listenfd-support"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .env_remove("DISPLAY")
        .env_remove("RUST_BACKTRACE")
        .env_remove("RUST_LIB_BACKTRACE");

    let mut child = match process.spawn() {
        Ok(child) => child,
        Err(err) => {
            warn!("error spawning {SATELLITE}, disabling X11 integration: {err}");
            return false;
        }
    };

    let status = match child.wait() {
        Ok(status) => status,
        Err(err) => {
            warn!("error waiting for {SATELLITE}, disabling X11 integration: {err}");
            return false;
        }
    };

    if !status.success() {
        warn!("{SATELLITE} doesn't support on-demand activation, disabling X11 integration");
        return false;
    }

    true
}

// Create /tmp/.X11-unix or validate that an existing one is trustworthy.
fn ensure_x11_unix_dir() -> Result<()> {
    let mut builder = std::fs::DirBuilder::new();
    builder.mode(0o1777);
    match builder.create(X11_TMP_UNIX_DIR) {
        Ok(()) => Ok(()),
        Err(err) if err.kind() == std::io::ErrorKind::AlreadyExists => {
            ensure_x11_unix_perms().context("wrong X11 directory permissions")
        }
        Err(err) => Err(err).context("error creating X11 directory"),
    }
}

fn ensure_x11_unix_perms() -> Result<()> {
    let x11_tmp = std::fs::symlink_metadata(X11_TMP_UNIX_DIR)
        .context("error checking X11 directory permissions")?;
    let tmp =
        std::fs::symlink_metadata("/tmp").context("error checking /tmp directory permissions")?;

    ensure!(
        x11_tmp.uid() == tmp.uid()
            || x11_tmp.uid() == smithay::reexports::rustix::process::getuid().as_raw(),
        "wrong ownership for X11 directory"
    );
    ensure!(
        (x11_tmp.mode() & 0o022) == 0o022,
        "X11 directory is not writable"
    );
    ensure!(
        (x11_tmp.mode() & 0o1000) == 0o1000,
        "X11 directory is missing the sticky bit"
    );

    Ok(())
}

/// Exclusively create `/tmp/.X{n}-lock` for the first free display number.
fn pick_x11_display(start: u32) -> Result<(u32, std::fs::File, Unlink)> {
    for n in start..start + 50 {
        let lock_path = format!("/tmp/.X{n}-lock");
        let Ok(lock_file) = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o444)
            .open(&lock_path)
        else {
            continue;
        };
        return Ok((n, lock_file, Unlink(lock_path)));
    }

    Err(anyhow!("no free X11 display found after 50 attempts"))
}

fn bind_to_abstract_socket(display: u32) -> Result<UnixListener> {
    let name = format!("{X11_TMP_UNIX_DIR}/X{display}");
    let addr = SocketAddr::from_abstract_name(name)?;
    UnixListener::bind_addr(&addr).context("error binding abstract socket")
}

fn bind_to_unix_socket(display: u32) -> Result<(UnixListener, Unlink)> {
    let name = format!("{X11_TMP_UNIX_DIR}/X{display}");
    // Unlink old leftover socket if any.
    let _ = std::fs::remove_file(&name);
    let addr = SocketAddr::from_pathname(&name)?;
    let guard = Unlink(name);
    let listener = UnixListener::bind_addr(&addr).context("error binding unix socket")?;
    Ok((listener, guard))
}

fn setup_connection() -> Result<X11Connection> {
    ensure_x11_unix_dir()?;

    let mut n = 0;
    let mut attempt = 0;
    let (display, lock_guard, abstract_listener, unix_listener, unix_guard) = loop {
        let (display, mut lock_file, lock_guard) = pick_x11_display(n)?;

        // Write our PID into the lock file (the traditional format).
        let pid_string = format!("{:>10}\n", std::process::id());
        lock_file
            .write_all(pid_string.as_bytes())
            .context("error writing PID to X11 lock file")?;
        drop(lock_file);

        let sockets = bind_to_abstract_socket(display)
            .and_then(|a| bind_to_unix_socket(display).map(|(u, g)| (a, u, g)));
        match sockets {
            Ok((a, u, g)) => break (display, lock_guard, a, u, g),
            Err(err) => {
                if attempt == 50 {
                    return Err(err)
                        .context("error opening X11 sockets after creating a lock file");
                }
                n = display + 1;
                attempt += 1;
            }
        }
    };

    Ok(X11Connection {
        display_name: format!(":{display}"),
        abstract_fd: OwnedFd::from(abstract_listener),
        unix_fd: OwnedFd::from(unix_listener),
        _unix_guard: unix_guard,
        _lock_guard: lock_guard,
    })
}

// When satellite fails to start and accept, the stale queued connection keeps
// the listening socket readable, respawning satellite in a busyloop. Drain
// (accept and drop) pending connections before re-arming the watch.
fn clear_out_pending_connections(fd: OwnedFd) -> OwnedFd {
    let listener = UnixListener::from(fd);

    if let Err(err) = listener.set_nonblocking(true) {
        warn!("error setting X11 socket to nonblocking: {err:?}");
        return OwnedFd::from(listener);
    }

    while listener.accept().is_ok() {}

    if let Err(err) = listener.set_nonblocking(false) {
        warn!("error setting X11 socket to blocking: {err:?}");
    }

    OwnedFd::from(listener)
}

fn setup_watch(tomoe: &mut Tomoe) {
    let Tomoe {
        satellite,
        loop_handle,
        ..
    } = tomoe;
    let Some(satellite) = satellite.as_mut() else {
        return;
    };

    if let Some(token) = satellite.abstract_token.take() {
        loop_handle.remove(token);
    }
    if let Some(token) = satellite.unix_token.take() {
        loop_handle.remove(token);
    }

    let watch = |fd: &OwnedFd,
                 take_own: fn(&mut Satellite) -> &mut Option<RegistrationToken>,
                 take_other: fn(&mut Satellite) -> &mut Option<RegistrationToken>|
     -> Option<RegistrationToken> {
        let fd = fd.try_clone().ok()?;
        let fd = clear_out_pending_connections(fd);
        let source = Generic::new(fd, Interest::READ, Mode::Level);
        loop_handle
            .insert_source(source, move |_, _, tomoe| {
                let Tomoe {
                    satellite,
                    loop_handle,
                    ..
                } = tomoe;
                if let Some(satellite) = satellite.as_mut() {
                    // Only one respawn even if both sockets fire.
                    if let Some(token) = take_other(satellite).take() {
                        loop_handle.remove(token);
                    }
                    *take_own(satellite) = None;

                    debug!("connection on X11 socket; spawning {SATELLITE}");
                    spawn(satellite);
                }
                Ok(PostAction::Remove)
            })
            .map_err(|err| warn!("error inserting X11 socket source: {err}"))
            .ok()
    };

    satellite.abstract_token = watch(
        &satellite.x11.abstract_fd,
        |s| &mut s.abstract_token,
        |s| &mut s.unix_token,
    );
    satellite.unix_token = watch(
        &satellite.x11.unix_fd,
        |s| &mut s.unix_token,
        |s| &mut s.abstract_token,
    );
}

fn spawn(satellite: &Satellite) {
    let abstract_fd = satellite.x11.abstract_fd.try_clone().ok();
    let unix_fd = match satellite.x11.unix_fd.try_clone() {
        Ok(fd) => fd,
        Err(err) => {
            warn!("error cloning X11 socket fd: {err}");
            let _ = satellite.to_main.send(ToMain::SetupWatch);
            return;
        }
    };
    let to_main = satellite.to_main.clone();

    let mut process = Command::new(SATELLITE);
    process
        .arg(&satellite.x11.display_name)
        .env_remove("DISPLAY")
        .env_remove("RUST_BACKTRACE")
        .env_remove("RUST_LIB_BACKTRACE")
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());

    // Spawning and waiting takes some milliseconds, so do it in a thread.
    let res = thread::Builder::new()
        .name("xwl-s spawner".to_owned())
        .spawn(move || {
            spawn_and_wait(process, abstract_fd, unix_fd);

            // Satellite exited or failed to spawn: re-arm the socket watch so
            // the next connection attempt respawns it.
            let _ = to_main.send(ToMain::SetupWatch);
        });

    if let Err(err) = res {
        warn!("error spawning a thread to spawn {SATELLITE}: {err:?}");
        let _ = satellite.to_main.send(ToMain::SetupWatch);
    }
}

fn spawn_and_wait(mut process: Command, abstract_fd: Option<OwnedFd>, unix_fd: OwnedFd) {
    let abstract_raw = abstract_fd.as_ref().map(|fd| fd.as_raw_fd());
    let unix_raw = unix_fd.as_raw_fd();

    process.arg("-listenfd").arg(unix_raw.to_string());
    if let Some(abstract_raw) = abstract_raw {
        process.arg("-listenfd").arg(abstract_raw.to_string());
    }

    unsafe {
        process.pre_exec(move || {
            // About to exec: clear CLOEXEC on the fds passed via -listenfd.
            // (Not dropped until after spawn(), so borrowing raw is sound.)
            let unix_fd = BorrowedFd::borrow_raw(unix_raw);
            fcntl_setfd(unix_fd, FdFlags::empty())?;

            if let Some(abstract_raw) = abstract_raw {
                let abstract_fd = BorrowedFd::borrow_raw(abstract_raw);
                fcntl_setfd(abstract_fd, FdFlags::empty())?;
            }

            Ok(())
        })
    };

    let mut child = match process.spawn() {
        Ok(child) => child,
        Err(err) => {
            warn!("error spawning {SATELLITE}: {err:?}");
            return;
        }
    };

    // The process spawned, we can drop our fds.
    drop(abstract_fd);
    drop(unix_fd);

    match child.wait() {
        // Most likely a crash, hence warn.
        Ok(status) => warn!("{SATELLITE} exited with: {status}"),
        Err(err) => warn!("error waiting for {SATELLITE}: {err:?}"),
    }
}
