#!/usr/bin/env bash
# R13-G4 focused State-v5 staged-script capture/restore cases.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$here/run_emerald_resource_state.sh" g4-state
