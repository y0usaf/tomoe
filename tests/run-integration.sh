#!/usr/bin/env bash
# Run the end-to-end check from the working tree or inside a Nix build.
# Needs sbcl, a C compiler, pkg-config, wayland-scanner and TOMOE_BIN.
set -euo pipefail

# This directory holds the driver, the fixtures and the client. In the tree it
# sits in <lisp>/tests; a Nix check copies just this directory, so neither the
# tree root nor the build defaults below may assume a parent layout.
tests="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$tests")"
build="${TOMOE_TEST_BUILD:-$root/build/tests}"

export TOMOE_BIN="${TOMOE_BIN:-$root/build/tomoe}"
[ -x "$TOMOE_BIN" ] || {
  echo "run-integration: $TOMOE_BIN is not executable; build it or set TOMOE_BIN" >&2
  exit 1
}

# The saved image is run directly here, so it needs the shim the wrapper sets.
if [ -z "${TOMOE_BACKEND_LIB:-}" ] && [ -f "$root/build/libtomoe-backend.so" ]; then
  export TOMOE_BACKEND_LIB="$root/build/libtomoe-backend.so"
fi

if [ -z "${TOMOE_TEST_CLIENT:-}" ]; then
  mkdir -p "$build"
  export TOMOE_TEST_CLIENT="$build/client"
  bash "$tests/build-client.sh" "$TOMOE_TEST_CLIENT"
fi
[ -x "$TOMOE_TEST_CLIENT" ] || {
  echo "run-integration: $TOMOE_TEST_CLIENT is not executable" >&2
  exit 1
}

# A fresh 0700 runtime directory per run: the compositor requires one, and its
# sockets must not land in the caller's session.
owned=0
if [ -n "${TOMOE_TEST_RUNTIME_DIR:-}" ]; then
  export XDG_RUNTIME_DIR="$TOMOE_TEST_RUNTIME_DIR"
else
  export XDG_RUNTIME_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tomoe-runtime-XXXXXX")"
  owned=1
fi
chmod 700 "$XDG_RUNTIME_DIR"

# The child gets its own process group so this cleanup reaps the compositor and
# every client even when the check is interrupted.
cleaned=0
cleanup() {
  status=$?
  if [ "$cleaned" = 1 ]; then return 0; fi
  cleaned=1
  if [ -n "${child:-}" ]; then
    kill -TERM -- "-$child" 2>/dev/null || true
    wait "$child" 2>/dev/null || true
  fi
  if [ "$owned" = 1 ]; then rm -rf "$XDG_RUNTIME_DIR"; fi
  return "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

set -m
sbcl --script "$tests/integration.lisp" &
child=$!
wait "$child"
