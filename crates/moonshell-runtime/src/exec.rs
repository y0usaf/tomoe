//! `shell.exec` / `shell.exec_async` — subprocess helpers.
//!
//! `exec` blocks (nur's contract: returns trimmed stdout; spawn failure
//! is the only error — a nonzero exit still yields whatever stdout
//! said). `exec_async` spawns a short-lived thread that runs the
//! command and sends an [`ExecReply`] over the calloop channel owned by
//! [`crate::ShellCtx`]; the binary's channel source hands it to
//! [`crate::ShellCtx::dispatch_exec_reply`] on the loop thread, where
//! Lua is safe to enter. The reply itself is plain `Send` data (id +
//! output) — the `WeakLua` + registry key stay behind in the ctx's
//! callback map, so a reply that lands after a hot reload (M2 §5)
//! finds a dead weak ref and is silently dropped.
//!
//! These are one-shot spawns by user request — the zero-*steady-state*-
//! subprocess goal (M3) is about polling backends, not about giving
//! Lua a process primitive.

use mlua::prelude::*;

/// A finished `shell.exec_async` command on its way back to the loop
/// thread. Plain data — crosses the thread boundary; the Lua side of
/// the callback lives in [`crate::ShellCtx`], keyed by `id`.
pub struct ExecReply {
    pub(crate) id: u64,
    pub(crate) output: String,
}

/// The loop-thread half of an in-flight `exec_async`: where the reply
/// goes once the command finishes.
pub(crate) struct ExecCallback {
    pub(crate) weak: WeakLua,
    pub(crate) key: LuaRegistryKey,
}

impl ExecCallback {
    /// Call with the command's trimmed stdout. Must run on the loop
    /// thread. A dropped VM discards the reply.
    pub(crate) fn call(&self, output: String) {
        let Some(lua) = self.weak.try_upgrade() else {
            return;
        };
        match lua.registry_value::<LuaFunction>(&self.key) {
            Ok(f) => {
                if let Err(e) = f.call::<()>(output) {
                    tracing::error!("exec_async callback error: {e}");
                }
            }
            Err(e) => tracing::error!("exec_async registry lookup failed: {e}"),
        }
    }
}

/// Blocking `sh -c` with trimmed stdout — `shell.exec`.
pub(crate) fn run_blocking(cmd: &str) -> LuaResult<String> {
    let out = std::process::Command::new("sh")
        .arg("-c")
        .arg(cmd)
        .output()
        .map_err(|e| LuaError::RuntimeError(format!("shell.exec({cmd:?}) failed: {e}")))?;
    Ok(String::from_utf8_lossy(&out.stdout).trim().to_string())
}

/// Fire-and-forget thread for `shell.exec_async`. A failed command
/// reports "" (nur's contract); a dead channel (loop already gone)
/// drops the reply.
pub(crate) fn spawn(cmd: String, id: u64, tx: calloop::channel::Sender<ExecReply>) {
    let builder = std::thread::Builder::new().name("moonshell-exec".into());
    let spawned = builder.spawn(move || {
        let output = std::process::Command::new("sh")
            .arg("-c")
            .arg(&cmd)
            .output()
            .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
            .unwrap_or_default();
        let _ = tx.send(ExecReply { id, output });
    });
    if let Err(e) = spawned {
        tracing::error!("exec_async thread spawn failed: {e}");
    }
}
