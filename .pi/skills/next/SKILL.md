---
name: next
description: Review the last completed PLAN.md step, then implement the next open item in the parity plan. Use at the start of a fresh session to continue working through tomoe's plan.
---

# next

You are continuing work on tomoe — a Wayland compositor (Smithay +
embedded Lua) targeting **niri's performance, Hyprland's features,
ShojiWM's configurability**. The previous session's context is gone:
DESIGN.md, PLAN.md, ARCHITECTURE.md, and git history are your memory.

## 1. Review the past step

- Read `DESIGN.md` in full — vision, locked-decisions table, and the
  core-as-mechanism / dogfooded-extension-surface split (all WM policy
  ships as Lua on the public API).
- Read `PLAN.md` — the concrete gap list. The next open item is the
  first unchecked box.
- Read `ARCHITECTURE.md` (generated, always fresh) instead of exploring
  the codebase by hand.
- Check recent commits (`git log --oneline -10` plus the last few diffs)
  to see what actually landed. If work landed but PLAN.md wasn't updated,
  fix PLAN.md first.
- Sanity-check the tree: `git status` clean, `nix flake check` passes. If
  the last step left things broken or half-finished, finishing it IS the
  next step.

## 2. Implement the next step

- **References win.** `ref/` (niri, Hyprland, ShojiWM) is the source of
  truth for patterns — read the corresponding module before porting a
  behavior, and match the reference's shape unless a locked-decision row
  says otherwise.
- **Core is mechanism, policy is Lua.** Workspaces, tiling, focus order,
  rules — all WM policy ships as Lua on the same public surface users
  get. If a built-in can't be expressed through the API, grow the API,
  don't bypass it. Native fallback (no hooks → full-screen map) must
  keep working.
- **Code standard**: typed errors per layer, no `unwrap`/`expect` in
  library crates, cross-cutting mechanisms written once.
- **Debug launches MUST be time-boxed** (per CLAUDE.md): never launch the
  tty backend unbounded — `timeout 5 tomoe-session > /tmp/tomoe-debug.log
  2>&1; echo "exit: $?"` and warn the user the screen will blank.
- Verify through the flake (`nix flake check`; `cargo fmt` / `clippy` are
  the sanctioned exceptions). `git add` new files before building — the
  flake only sees tracked files. After structural changes, rerun
  `scripts/gen-arch.sh` inside `nix develop` and commit the result, or
  `checks.arch-fresh` fails.

## 3. Close the loop

- Update PLAN.md: check the item off (or note the slice landed and what
  remains). Update DESIGN.md decision rows as needed.
- Commit in the style of the existing history: component prefix,
  imperative, body explains shape/why.
- End your reply with a one-paragraph handoff — what landed, what's next,
  anything the next session must know — then fold anything load-bearing
  from it into PLAN.md itself.
