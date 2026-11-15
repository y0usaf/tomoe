#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
make -j"$(nproc)" ${WLR_PROTOCOLS_XML:+WLR="$WLR_PROTOCOLS_XML/unstable"}
exec build/tomoe "$@"
