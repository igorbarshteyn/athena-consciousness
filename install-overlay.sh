#!/usr/bin/env bash
# Separate entry point: never copy over stock Athena's install.sh.
set -Eeuo pipefail
OVERLAY_ROOT="$(cd -- "$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")" && pwd -P)"
exec bash "$OVERLAY_ROOT/patches/install-consciousness.sh" "$@"
