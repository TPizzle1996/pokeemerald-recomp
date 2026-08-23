#!/usr/bin/env bash
# R13-H3 focused State-v5 staged battle-script capture/restore cases.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$here/run_emerald_resource_state.sh" h3-state
