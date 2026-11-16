# Working here

## Landing a change

Everything lands on `main` through `./land.sh --push BRANCH`, which checks the exact
commit it pushes:

```sh
./land.sh my-branch            # rebase onto origin/main, run the smoke check, stop
./land.sh --push my-branch     # the same, then push main and delete the branch
./land.sh --push 0108501d      # a commit, when there is no branch name
```

The branch is rebased onto `origin/main`, `checks.<system>.smoke` runs on the
result, and only that commit is pushed, to `origin` and `forgejo`. If the rebase
conflicts or the smoke check fails, nothing is pushed.

The smoke check is one VM, a few minutes: it builds the compositor, starts it,
draws windows, takes keyboard and pointer input, and survives a policy reload
(`integration`, `pointer-protocols` and `livepatch` in `tests/default.nix`).

Before a commit is pinned in finix, `./gate.sh REV` runs it with the config finix
generates against that exact build: a private headless instance on its own socket,
runtime directory and home, at 5120x1440@240, with test windows, dark then light,
every rail theme, rapid focus switches and moves while windows close and open
mid-animation, a reload, and a config made for another build, which must be
refused. It fails on any extension failure, duplicate timer, lost focus or
unplaced window. Pin only a revision it passed. Nothing it does reaches the
running desktop. Everything slower is for a speed claim, not for landing:

```sh
nix flake check            # every aggregate VM, ~30 min
nix build -L .#legacyPackages.x86_64-linux.suites.timing     # one suite, its own VM
nix build -L .#legacyPackages.x86_64-linux.suites.memory
nix build -L .#legacyPackages.x86_64-linux.suites.<name>     # any suite in tests/
```

Timing gates need a quiet machine, so run them alone and compare against
`origin/main` on the same machine state.

## The VM lock

`$XDG_RUNTIME_DIR/agent/tomoe-vm.lock` is a shared/exclusive barrier on the one test
VM. Every build, eval or check on this machine takes a lease on it, Tomoe or not:
`flock -s` for builds and VMs that are not measuring anything, `flock -x` only for a
timing measurement or a benchmark.

`flock` lets shared holders starve a waiting exclusive gate, so the lock also has a
queue marker: before asking for `flock -x`, create
`$XDG_RUNTIME_DIR/agent/tomoe-vm.pending/<your-agent-name>` and remove it as soon as
the lock is granted (and on any exit); before each new `flock -s`, wait until that
directory is empty — for at most `LAND_PENDING_WAIT` (10 minutes), because a steady
stream of exclusive gates must not starve shared work forever. `land.sh` does all of
this for you.

## Rules

- Gate the exact commit you push. Never push a commit that was not the one checked.
- `origin` and `forgejo` both get `main`; delete your branch once it lands
  (`land.sh --push` does that).
- If something breaks on `main`, revert it rather than leaving it red.
- Update `AGENTS.md` and the README when behaviour changes; this file is short on
  purpose.
