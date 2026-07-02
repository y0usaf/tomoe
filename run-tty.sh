#!/bin/sh
# Launch tomoe on the current TTY (run this from TTY2).
# Logs go to /tmp/tomoe.log — check there if anything goes wrong.
cd "$(dirname "$0")" || exit 1
BIN=target/release/tomoe
[ -x "$BIN" ] || BIN=target/debug/tomoe
echo "launching $BIN (logs: /tmp/tomoe.log)"
# nix develop clobbers SHELL with the stdenv's non-readline bash; restore the
# login shell so spawned terminals launch it instead. It also exports a
# lowercase `shell` variable pointing at the same bash — nushell's env table is
# case-insensitive, so `shell` clobbers the corrected SHELL when nu loads its
# environment. Unset it.
LOGIN_SHELL="$(getent passwd "$(id -un)" | cut -d: -f7)"
exec nix develop --command env -u shell SHELL="${LOGIN_SHELL:-$SHELL}" "$BIN" --backend tty "$@" >/tmp/tomoe.log 2>&1
