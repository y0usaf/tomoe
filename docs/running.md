# Running Tomoe

How to start Tomoe, what the shipped desktop does out of the box, and how it
shares a session with X11 programs and D-Bus.

## Run

From the repository root:

```sh
nix build
nix run .
```

`nix flake check` boots NixOS VMs (it needs the `kvm` system feature) and
drives the built compositor headless through its CLI and sockets:
`integration` maps a window under the shipped builtins, `clean-unmount` mounts,
exercises and unmounts every builtin and example and fails on any state,
process or file left behind, and `bare` starts and quits with no builtins.

The default backend is `auto`. It opens a nested window when a parent Wayland
display is available; otherwise it uses DRM for a direct session from a TTY.
An explicit `WAYLAND_DISPLAY` takes priority. When it is unset or empty, startup
looks for a live `wayland-N` socket under `XDG_RUNTIME_DIR`, skipping stale
sockets and lock files. No manual environment export is needed for discovery.

`--backend nested` uses the same display discovery but fails if no parent is
available. It never falls back to hardware. The compositor creates
`tomoe-0` under `XDG_RUNTIME_DIR`. It does not change your systemd, D-Bus,
display-manager, or surrounding desktop environment.

Explicit modes:

```sh
nix run . -- --backend nested
nix run . -- --backend headless
nix run . -- --backend drm
nix run . -- --bare --backend headless
```

DRM mode takes direct control of outputs and input devices. Run it from an
appropriate TTY with seat access. It is the author's daily session on an NVIDIA RTX 4090;
other GPUs see less testing.
`--bare` loads no extensions. Clients can still map and render at their own
initial size, without focus policy or shortcuts.

`XDG_RUNTIME_DIR` must be an owned directory with mode `0700`. Do not put live
runtime sockets inside this flake's source directory. Nix cannot copy sockets
into a source archive. `--socket NAME` permits independent instances. A socket
left behind by an exit that skipped cleanup is reclaimed on the next start; a
name a live instance answers on is refused.

The package includes Foot as its terminal and Fuzzel as its launcher. Shipped
bindings use `:mod`, which is Super unless `(settings :mod ...)` says otherwise:

| Binding | Command |
| --- | --- |
| Super+Return | Open a terminal |
| Super+d | Open Fuzzel |
| Super+j | Focus the next window |
| Super+k | Focus the previous window |
| Super+f | Toggle fullscreen for the focused window |
| Super+1 through Super+9 | Switch workspace |
| Super+Shift+1 through Super+Shift+9 | Move the focused window to a workspace |
| Super+q | Ask the focused client to close |
| Super+Shift+slash | Show every binding that has a description |
| Super+Shift+e | Quit, after a confirmation dialog |

Holding Super with the left mouse button moves the window under the pointer;
Super with the right button resizes it. Both drags end when the button is
released. The default WM has nine workspaces and an eight-pixel gap. It tiles
the active workspace inside the first output's usable area, repeatedly splitting
the longer remaining side in half (dwindle). Workspace lists retain insertion
order; switching selects the destination's last window. Outputs and all window
coordinates are available to Lisp, so a replacement can use the others.

The WM consumes `:workspace`, `:fullscreen`, and `:focus` rule properties when a
window first maps. An inactive destination stays hidden; its fullscreen and
focus properties are ignored. An active fullscreen rule focuses the window even
when `:focus` is explicitly `nil`. Later metadata
changes do not repeat admission. An xdg buffer detach retains the admitted
window, workspace and rule state; remapping the same role does not readmit it.
Role destruction ends that lifetime. Reload preserves workspace order and state.

Client fullscreen requests are denied by default; unfullscreen requests are
always honored. Maximize requests are acknowledged while retaining tiled
geometry. Configure these defaults with an ordinary owner:

```lisp
(define-extension "desktop-settings" (:reads ()) (snapshot state event)
  (declare (ignore snapshot event))
  (values state
          (list (publish-state :wm-settings
                               '(:gaps 8 :workspace-count 9 :honor-client-fullscreen t)))
          nil))
```

The WM publishes `:wm-state` in `:data`, with `:active` and a `:workspaces` list
of `(:id N :windows COUNT)` records. This is available through snapshots and
`inspect`, and as the JSON `wm_state` method/event. The shell service facade
remains absent.

## X11 clients

X11 clients run through xwayland-satellite, which presents each X11 window to
Tomoe as an ordinary xdg toplevel. At startup the host picks the first display
whose `/tmp/.X<n>-lock` file is absent or names a dead process and exports it
as `DISPLAY` to this process and its children, never to systemd, D-Bus or the
surrounding session. The shipped `xwayland` extension runs `tomoe xwayland` on
that display as a `service`, restarting it when it exits: it takes the lock,
listens on the display's sockets, and on the first X11 connection starts
`xwayland-satellite -listenfd` as its child, so Xwayland only starts once an
X11 client connects. Satellite gets a private `XDG_RUNTIME_DIR` inside the
session's, holding the Wayland socket it opens for Xwayland. `tomoe xwayland`
passes SIGTERM, SIGINT and SIGHUP on to satellite, kills it if it is still
running half a second later, and once it exits removes that directory, the lock
and the socket; stopped before any client, it removes the lock and the socket.
`--bare` has no X11 until a policy declares that service. The
compositor puts its own `bin`, `xwayland-satellite` and the D-Bus tools from
its package first on the `PATH` it hands its children.

To policy, an X11 window is an xdg window: its title and app id come from
satellite, `place` sends a configure, and fullscreen and maximize go through
xdg state. Menus, tooltips and other override-redirect windows are satellite's
popups and subsurfaces. Satellite's own stderr passes through to this
compositor's stderr.

## Session bus

Before its notification, tray and media services start, the host settles the
session bus its children share and exports it as `DBUS_SESSION_BUS_ADDRESS`.
An inherited address is kept unless it is `disabled:`, which Chromium-based
programs export when none is set, or this instance's own socket. Otherwise a
bus answering at `$XDG_RUNTIME_DIR/bus` is exported. With neither, the host
runs `dbus-daemon --session` from its package on `tomoe.NAME.bus` under
`XDG_RUNTIME_DIR` and waits up to two seconds for it to listen, so a session
started from a TTY needs no `dbus-run-session` wrapper and its clients never
fall back to X11 autolaunch. That daemon inherits the exported
`WAYLAND_DISPLAY`, `DISPLAY`, `XDG_CURRENT_DESKTOP` and `TOMOE_SOCKET`, so
services it activates, such as portals, join this session. The bus belongs to
the session, including under `--bare`; reload and unmount leave it running. On
exit the host stops the daemon and removes its socket and the transient service
directory it created; a daemon that a killed run left on that socket is killed
on the next start. A daemon that cannot start is reported on stderr, and the
host runs on with `DBUS_SESSION_BUS_ADDRESS` unset.
