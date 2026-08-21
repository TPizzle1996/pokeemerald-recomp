#!/usr/bin/env bash
# R13-G4 mandatory real process-A/process-B relocation gate.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$here/run_emerald_resource_state.sh" g4-cross
