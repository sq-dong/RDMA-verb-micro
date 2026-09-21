#!/usr/bin/env bash
# Run all collectors (Fig.2–6) then plot everything.
# Usage: ./scripts/collect_all.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

# Fresh CSVs for a full sweep
rm -f "$RESULTS_DIR"/fig{2,3,4,5,6}.csv

"$SCRIPT_DIR/collect_fig2.sh"
"$SCRIPT_DIR/collect_fig3.sh"
"$SCRIPT_DIR/collect_fig4.sh"
"$SCRIPT_DIR/collect_fig5.sh"
"$SCRIPT_DIR/collect_fig6.sh"

python3 "$SCRIPT_DIR/plot_paper_figs.py" --fig all --results-dir "$RESULTS_DIR"
log "done. Figures under $RESULTS_DIR"
