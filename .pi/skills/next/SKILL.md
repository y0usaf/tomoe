---
name: next
description: Review the last completed PLAN.md step, then implement the next open item in the build plan. Use at the start of a fresh session to continue working through moonshell's plan.
---

# next

You are continuing work on moonshell — a GPU-free, Lua-scriptable
Wayland desktop shell (SCTK + tiny-skia + cosmic-text + mlua/LuaJIT)
targeting **QuickShell's extensibility, a terminal emulator's
footprint, waybar's portability**. The previous session's context is
gone: DESIGN.md, PLAN.md, and git history are your memory.

## 1. Review the past step

- Read `DESIGN.md` in full — vision, locked-decisions table, doctrine
  conformance, the crate map, and the extension-surface contract (all
  widgets/themes/service facades are Lua in `lua/` on the public API).
- Read `PLAN.md` — the concrete gap inventory and milestone breakdown.
  The next open item is the first unchecked box in milestone order
  (M0 → M6); the "Milestone order & first steps" section is the queue.
- Check recent commits (`git log --oneline -10` plus the last few
  diffs) to see what actually landed. If work landed but PLAN.md wasn't
  updated, fix PLAN.md first.
- Sanity-check the tree: `git status` clean, `nix flake check` passes
  (once the flake exists — building it is M0 step 1). If the last step
  left things broken or half-finished, finishing it IS the next step.

## 2. Implement the next step

- **References win.** nur (`~/Dev/nur`) is the source of truth for the
  Lua API, `lua/` stdlib, widgets, and services *logic* — read its
  ARCHITECTURE.md and the corresponding module before porting.
  Inherited: the contract. Not inherited: GPUI, `APP_PTR`/cx bridge,
  tokio, CLI-polling service backends. tomoe (`~/Dev/tomoe`) supplies
  the `tomoe-ipc` wire crate (git dep only — no other shared Rust) and
  the coordinate/buffer discipline.
- **Core is mechanism, policy is Lua.** If a shipped widget or theme
  can't be expressed through the public API, grow the API, don't
  bypass it. The bare no-config binary (doctrine 06) must keep booting.
- **Nothing Lua-shaped in M0/M1** — the render core must be provably
  tiny before the runtime lands on top.
- **Memory is an acceptance criterion, not a hope.** Every milestone
  has an RSS/wakeup gate (20 MB bare, 25 MB full bar, 40 MB hard; zero
  idle wakeups; zero steady-state subprocesses). Measure with smem /
  powertop when a milestone claims completion; record numbers in
  PLAN.md.
- **Standing lessons** (PLAN.md bottom): never regenerate buffers per
  frame; integer-physical pixel sizing (blurry text is the one
  unforgivable sin); Lua-facing functions return `LuaResult`, convert
  to anyhow at one boundary; store `LuaRegistryKey`, never
  `LuaFunction`, for callbacks that outlive the stack frame.
- **Code standard**: typed errors per layer, no `unwrap`/`expect` in
  library crates; one-way dependency arrow (`runtime` sees
  `surface`/`render`/`services`, nothing sees `runtime`); element
  types and services follow the single declaration shape (doctrine 05).
- Verify through the flake (`nix flake check`; `cargo fmt` / `clippy`
  are the sanctioned exceptions). `git add` new files before building —
  the flake only sees tracked files.

## 3. Close the loop

- Update PLAN.md: check the item off (or note the slice landed and
  what remains). Update DESIGN.md decision/doctrine rows as needed;
  interconnection changes get mirrored in tomoe's PLAN.md note.
- Commit in the style of the existing history: component prefix,
  imperative, body explains shape/why.
- End your reply with a one-paragraph handoff — what landed, what's
  next, anything the next session must know — then fold anything
  load-bearing from it into PLAN.md itself.
