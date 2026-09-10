# Finix integration checkpoint

## Status
Complete. The compositor is installed in the Nix user profile and launchable through `~/tomoe-lisp`. It also remains exposed through `~/finix#tomoe-lisp` and selected for a future system package installation. No system rebuild, system activation, commit, push, or active desktop change was performed.

## Goal and constraints
Make the Lisp compositor usable through `~/finix` alongside original Tomoe. Preserve unrelated Finix edits. Keep application source outside Finix, keep the compositor's pinned wlroots dependency, and do not add tests. The request did not authorize the full ship workflow or system activation.

## Delivered
- `/home/y0usaf/finix/flake.nix:197`: local `tomoe-lisp` input at `path:/home/y0usaf/dev/sandbox/tomoe-v2/lisp`, retaining its own nixpkgs pin.
- `/home/y0usaf/finix/modules/outputs.nix:63`: exposes `packages.x86_64-linux.tomoe-lisp` for immediate `nix run` without activation.
- `/home/y0usaf/finix/modules/desktop/session/ui/tomoe-lisp/default.nix`: opt-in `user.ui.tomoe-lisp.enable` installs the source flake's package. No services, login/session settings, or portals changed.
- `/home/y0usaf/finix/modules/hosts/y0usaf-desktop/ui.nix:5`: enables the package on this desktop only. Original Tomoe remains enabled; the Framework remains opted out.
- `/home/y0usaf/finix/modules/desktop/session/ui/tomoe-lisp/README.md`: nested/control commands, explicit DRM launch, local source update instructions, and prototype limits.
- `/home/y0usaf/finix/flake.lock`: added only `tomoe-lisp`, its `nixpkgs_9` node, and the root input mapping. All pre-existing lock nodes remain unchanged. New input NAR hash is `sha256-PopfE5xPwf36VIpmejybVPcUcXcsRuI3V8a6WrvbP7M=`.
- Marked only the two new module files intent-to-add so Git-backed Nix evaluation can see them. No file contents were staged or committed.

## Verification
- Baseline original Tomoe enable option evaluated to true before edits.
- `nix build --no-update-lock-file /home/y0usaf/finix#tomoe-lisp --no-link --print-out-paths` exited zero and reused `/nix/store/73ay4jcnb10qh86icsi1w6fw2khwxf3m-tomoe-lisp-0.1.0`, the compositor artifact previously verified with real clients.
- `nix run --no-update-lock-file /home/y0usaf/finix#tomoe-lisp -- --version` exited zero and printed `tomoe-lisp 0.1.0, wire 1, native ABI 1`.
- Follow-up explicitly requested an installed package launchable from home. `nix profile add --no-update-lock-file /home/y0usaf/finix#tomoe-lisp` succeeded. The previously empty user profile now contains an active `tomoe-lisp` entry at the same verified store path.
- The current session PATH omits user-profile binaries, and Rush startup files are managed store symlinks. Added only `/home/y0usaf/tomoe-lisp -> .nix-profile/bin/tomoe-lisp`, without changing the managed shell configuration.
- From `/home/y0usaf`, `./tomoe-lisp --version` exited zero with version 0.1.0. The home launcher resolves to the installed package.
- Desktop option evaluation returned `{ "tomoe": true, "tomoe-lisp": true }`.
- Desktop `environment.systemPackages` evaluation contains the exact package above. Framework enable option evaluated to false.
- Finix's Alejandra formatter check passed for the new module, outputs module, and desktop UI module. Focused `git diff --check` passed.
- Lock audit found two added nodes, zero removed nodes, and only the root mapping changed among existing nodes. Only the `tomoe-lisp` root input was added.
- Parallel Nix evaluations reported benign ignored SQLite cache contention; all commands succeeded. One pre-existing package evaluation deprecation warning mentioned `system` versus `stdenv.hostPlatform.system`.
- No full-system build or switch was run. Hardware DRM and physical input retain the earlier verification gaps.

## Usage
`~/tomoe-lisp --backend nested`

From home, `./tomoe-lisp --backend nested` is equivalent. This is a stable link through the Nix user profile, not a copied binary or shell wrapper. The current session still lacks that profile's bin directory in PATH, so use the explicit launcher path.

`nix run ~/finix#tomoe-lisp -- --backend nested` also remains available. The local checkout must remain at the declared input path. The package will enter the system profile only on a later authorized activation; the user-profile installation already works.

## Context and artifacts
- Live OS is Finix with Finit, hostname `y0usaf-desktop`, active desktop original Tomoe.
- Finix has extensive unrelated pre-existing changes, including `flake.nix` and `flake.lock`; they were preserved.
- Read repository layout docs plus unslop, anti-slop, and ship guidance. No AGENTS.md files were found along the Finix path or under its module tree.
- Temporary original-lock backup location is recorded in `/tmp/tomoe-finix-lock-backup-path`. It exists only to audit preservation of the user's lock edits.

## Pending
None for integration setup. Activation was not authorized or performed.
