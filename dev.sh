#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export TOMOE_SHELL="${TOMOE_SHELL:-$(command -v sh)}"
export TOMOE_BUILTINS="${TOMOE_BUILTINS:-$PWD/builtins/desktop.lisp}"
SBCL_HOME="$(dirname "$(readlink -f "$(command -v sbcl)")")/../lib/sbcl"
export SBCL_HOME

make -j"$(nproc)" SBCL_HOME="$SBCL_HOME" ${WLR_PROTOCOLS_XML:+WLR="$WLR_PROTOCOLS_XML/unstable"}

mkdir -p build/bin
printf '#!/bin/sh\ncd %s && exec build/tomoe-runtime --core "%s/sbcl.core" --noinform --load dev.lisp -- "$@"\n' "$PWD" "$SBCL_HOME" > build/bin/tomoe
chmod +x build/bin/tomoe
export PATH="$PWD/build/bin:$PATH"

exec build/tomoe-runtime --core "$SBCL_HOME/sbcl.core" --noinform --load dev.lisp -- "$@"
