#!/bin/sh
# Launch tomoe on the current TTY (run this from TTY2).
# Logs go to /tmp/tomoe.log — check there if anything goes wrong.
cd "$(dirname "$0")" || exit 1
PROJ="$PWD"
echo "building (logs: /tmp/tomoe.log)"
# nix develop clobbers SHELL with the stdenv's non-readline bash; restore the
# login shell so spawned terminals launch it instead. It also exports a
# lowercase `shell` variable pointing at the same bash — nushell's env table is
# case-insensitive, so `shell` clobbers the corrected SHELL when nu loads its
# environment. Unset it.
LOGIN_SHELL="$(getent passwd "$(id -un)" | cut -d: -f7)"
nix develop --command cargo build || exit $?
# Launch the binary we just built — a leftover release build predating recent
# commits must never shadow it (that's how a stale compositor without the
# data-control protocols ended up running).
BIN="$PROJ/target/debug/tomoe"
echo "launching $BIN"
# Screencast source picker; tomoe pushes this into the activation env for
# the bus-activated portal backend.
export TOMOE_PORTAL_CHOOSER="${TOMOE_PORTAL_CHOOSER:-$HOME/.config/scripts/portal-chooser.sh}"
# Launch from $HOME so spawned terminals open there instead of the project dir.
cd "$HOME"
exec nix develop "$PROJ" --command env -u shell SHELL="${LOGIN_SHELL:-$SHELL}" "$BIN" --backend tty "$@" >/tmp/tomoe.log 2>&1
