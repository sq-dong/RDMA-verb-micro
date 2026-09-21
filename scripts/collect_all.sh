#!/usr/bin/env bash
# Run all collectors (Fig.2–6). Each collector plots then deletes its logs/CSVs.
# Usage: ./scripts/collect_all.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

"$SCRIPT_DIR/collect_fig2.sh"
"$SCRIPT_DIR/collect_fig3.sh"
"$SCRIPT_DIR/collect_fig4.sh"
"$SCRIPT_DIR/collect_fig5.sh"
"$SCRIPT_DIR/collect_fig6.sh"

log "done. Figures (png/pdf) under $RESULTS_DIR"
