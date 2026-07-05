---
name: next
description: Review the last completed PLAN.md step, then implement the next open item in milestone order. Use at the start of a fresh session to continue working through the tomoe parity plan.
---

You are continuing work on tomoe's parity plan. The previous session's context
is gone — PLAN.md and git history are your memory.

## 1. Review the past step

- Read `PLAN.md` in full. Read `DESIGN.md` if the next item touches
  architecture, coordinates, rendering, or the Lua API.
- Look at recent commits (`git log --oneline -15` and the diffs of the last
  few) to see what the previous session actually landed.
- Cross-check: does PLAN.md reflect that work? If an item was completed but
  not checked off (or "Where we are" is stale), update PLAN.md first.
- Sanity-check the tree: `git status` is clean and `nix build` succeeds
  before starting new work. If the last step left the build broken or
  half-finished, finishing/fixing it IS the next step.

## 2. Implement the next step

- The next step is the first unchecked item in milestone order (M1 → M6),
  including any pending items in a milestone's *Accept* line. If an item is
  too large for one session, take a coherent slice and say so in the commit
  and in PLAN.md.
- Before touching an area, read the `ref/` docs PLAN.md cites for it (the
  "Standing lessons" section at the bottom, plus any inline references on
  the item itself). Study the corresponding implementation in `ref/niri`,
  `ref/ShojiWM`, etc. before designing your own.
- Follow the design canon in `~/Dev/design/doctrines/` (this is a personal
  project); divergences go in DESIGN.md's conformance table.
- Verify through the flake: `nix build` / `nix flake check` (`cargo fmt` /
  `clippy` are the sanctioned exceptions). If the item needs a live session
  to verify (real outputs, portals, games), do every check that's possible
  headlessly and record what remains as a pending verification note in
  PLAN.md rather than claiming acceptance.

## 3. Close the loop

- Update PLAN.md: check the item off (or note the slice landed and what
  remains), and update "Where we are" if the milestone state changed.
- Commit the work with a message in the style of the existing history
  (component prefix, imperative, explains the shape/why).
- End your reply with a one-paragraph handoff: what landed, what's next,
  and anything the next session must know that isn't in PLAN.md yet —
  then fold anything load-bearing from that paragraph into PLAN.md itself.
