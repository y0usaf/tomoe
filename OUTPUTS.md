# Output resolution and pixel mapping

## Status
Complete and installed for the next compositor launch. The active desktop was not restarted or reconfigured: PID 31689 still runs the old `/nix/store/i9wffpjp4346f6khib908f4spziv742x-tomoe-lisp-0.1.0` build at DP-4 3840x1080. No system rebuild/activation, commits, pushes, or tests were added.

## Goal and constraints
Add Tomoe/Niri-style output mode, refresh, logical position, and scale configuration, fractional-scale client support, and physical/logical inspection. Carry over the user's native panel configuration. Verify through Nix and real isolated sessions, update the existing user-profile package, and preserve the running desktop and concurrent pure-Lisp backend/dev-runner work.

## Delivered
- `lisp/src/api.lisp:78`: `configure-output` owned effect with preferred/max/exact mode, optional Hz refresh, logical position, and scale rounded to N/120. Validated immutable effect arguments contain only the existing bounded Lisp data types.
- `lisp/src/runtime.lisp:124`: materialize/commit output effects through the existing host write path. Last owner wins. Disconnected names remain configured for hotplug; unmount restores the earlier owner or baseline.
- `lisp/native/backend.c:233`: swapchain-manager backend-wide preflight, render buffers for the requested state, batch commit, and rollback on commit failure. Failed rollback stops the compositor. Output events report physical/logical size, refresh in mHz, scale in 120ths, transform, and advertised mode lists.
- Native protocol managers now include viewporter, fractional-scale-v1, xdg-output, and compositor v6. wlroots scene helpers handle client scale notification, buffer mapping, and input coordinates.
- `lisp/src/native.lisp:3`: typed FFI wrappers give wlroots its expected non-trapping C floating arithmetic and restore Lisp traps after every call. Native ABI is 2; additive control inspection fields retain wire version 1.
- `lisp/src/main.lisp:95`: optional `$XDG_CONFIG_HOME/tomoe-lisp/init.lisp` or `~/.config/tomoe-lisp/init.lisp`. Explicit `--config` overrides it; `--bare` skips it. Preserved concurrent `--backend lisp` changes.
- `lisp/backend/server.lisp`: the experimental pure-Lisp backend explicitly rejects output configuration; it has no hardware outputs yet. Other backend work is untouched.
- `lisp/README.md:63`: output configuration, logical versus physical mapping, protocol behavior, and limits.
- `/home/y0usaf/.config/tomoe-lisp/init.lisp:9`: DP-4 requests 5120x1440 at highest advertised refresh and scale 1; HDMI-A-2 requests 1920x1080@60 at logical x=5120. Existing `config.lisp` and `config-super.lisp` were preserved and are not implicitly loaded.
- Refreshed only Finix's `tomoe-lisp` input and upgraded only its existing profile entry. `~/tomoe-lisp` resolves to `/nix/store/apn3rrsin9133ccqcih0m93zckz38l8q-tomoe-lisp-0.1.0/bin/tomoe-lisp`.

## Evidence and decisions
- DP-4's EDID-preferred mode is the 3840x1080 compatibility mode. Tomoe explicitly requests 5120x1440; `/sys/class/drm/card2-DP-4/modes` confirms that size is advertised.
- Kept Lisp's logical-coordinate contract, like Niri, rather than converting policy and input to Tomoe's physical coordinates. At scale 1 the mapping is 1:1; fractional scales may resample edges and legacy integer-scale clients.
- Both source Nix build and `nix flake check --all-systems` exited zero after the final FFI correction. C/Lisp compilation was warning-free. x86_64 built, aarch64 evaluated. Git whitespace checks passed.
- Two real headless outputs accepted 2560x1440 at scale 1.25 with logical size 2048x1152, and 1920x1080@60 at x=2048, scale 1.
- A live output extension mapped/tiled a real Foot client on the 1.25-scale output. Unmount restored both outputs to their original 1280x720, scale 1, auto placement; layout reacted and runtime last-error remained NIL.
- Foot protocol logs show preferred_scale(150), correctly sized pixel buffers and logical viewport destinations, then preferred_scale(120) after unmount. No protocol errors. Foot closed its Wayland objects normally; its terminal child received SIGHUP from the close request, giving Foot exit 1. The compositor session exited zero and removed all its sockets.
- The first operational shutdown waited for Foot without servicing Wayland and timed out. Corrected the operational cleanup. The second exposed SBCL traps in wlroots viewport damage division on zero-sized buffers during unmap, traced to wlroots `types/wlr_compositor.c:258-264`. After fixing the FFI boundary, unmap/destruction succeeded and Lisp's `(:OVERFLOW :INVALID :DIVIDE-BY-ZERO)` traps remained enabled afterward.
- Packaged headless startup automatically loaded the user's `displays` extension with both disconnected names preserved. Packaged nested startup worked. `--bare` had no user or built-in extensions. All three compositor logs were empty; all exited zero and removed their sockets.
- `wayland-info` observed viewporter v1, fractional-scale-manager v1, xdg-output-manager v3, and logical output dimensions.
- `nix develop --command ./dev.sh --backend lisp --help` succeeded. Source-loaded dev runs emit existing forward-reference style warnings; packaged compilation does not.
- Finix `nix build --no-update-lock-file .#tomoe-lisp --no-link --print-out-paths` returned the final artifact. Lock comparison to the immediate pre-update snapshot found only `tomoe-lisp` changed, no added/removed nodes. Input NAR hash: `sha256-yWXQLeKGcfkPiHmWobsVk31CskkRyq1aQM9V3wXKw4A=`.
- Final profile manifest contains the final artifact. Active PID 31689, old executable path, output dimensions, and last-error NIL remained unchanged. No task-owned isolated processes, listeners, or runtime directories remain.

## Artifacts and concurrent state
Baselines and logs are under `/tmp/tomoe-lisp-outputs.8o0QJ6`, recorded by `/tmp/tomoe-lisp-outputs-backup-path`. Useful logs: `output-final-session.log`, `foot-final-wayland.log`, `packaged.inspect`, `packaged-nested.inspect`, `packaged-bare.inspect`, `wayland-info.log`, and `pure-backend-help.log`. Source workspace became a Git repository through concurrent work during this task; the observed base was `bc97214`. Preserve all unrelated edits and checkpoints.

## Remaining limits
The running desktop needs a compositor restart to use native ABI 2 and the default display policy; reloading the old process cannot replace its backend. Physical modesetting, hardware rollback, and physical input mapping remain unverified. Output rotation, mirroring, VRR controls, and pure-Lisp hardware output are not implemented. No further implementation or installation work is pending for this request.
