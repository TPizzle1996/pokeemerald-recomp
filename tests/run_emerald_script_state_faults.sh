#!/usr/bin/env bash
# R13-G4 field-script State-v5 refusal matrix.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$here/run_emerald_resource_state.sh" g4-faults
