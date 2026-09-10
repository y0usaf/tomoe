#!/usr/bin/env bash
# Run from the working tree. Edit a .lisp file, rerun, see the change.
# Use inside the dev shell: nix develop ./lisp -c ./dev.sh --backend lisp
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
# The shipped policy, unless --bare or an explicit override says otherwise.
export TOMOE_LISP_BUILTINS="${TOMOE_LISP_BUILTINS:-$PWD/builtins/desktop.lisp}"
exec sbcl --noinform --load dev.lisp -- "$@"
