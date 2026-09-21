#!/usr/bin/env bash
# Collect Fig.4 outbound throughput -> results/fig4.csv
# Requester is the TCP server (-R), matching paper MS -> clients.
# Usage: ./scripts/collect_fig4.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

CSV="$RESULTS_DIR/fig4.csv"
rm -f "$CSV"
csv_header "$CSV" "curve,size,mops"

# Paper Fig.4 x-axis: 0 64 128 192 256
SIZES=(4 8 16 32 64 128 192 256)
declare -a JOBS=(
  "WR-UC-INLINE|-m write_uc"
  "WRITE-UC|-m write_uc --no-inline"
  "READ-RC|-m read"
  "SEND-UD|-m send_ud"
)

sync_bins
kill_bench "$SRV_HOST"
kill_bench "$CLT_HOST"
sleep 1

PORT_BASE=18520
idx=0
for job in "${JOBS[@]}"; do
  curve="${job%%|*}"
  flags="${job#*|}"
  for size in "${SIZES[@]}"; do
    # Inline curves only meaningful up to ~256B
    if [[ "$curve" == "WR-UC-INLINE" && "$size" -gt 256 ]]; then
      continue
    fi
    port=$((PORT_BASE + idx))
    idx=$((idx + 1))
    log "fig4 $curve size=$size"

    pass_log="$RESULTS_DIR/fig4_pass_${curve}_${size}.log"
    req_log="$RESULTS_DIR/fig4_req_${curve}_${size}.log"

    # Passive client first, then requester on server (-R).
    pass_cmd="cd '$BENCH_DIR' && ./fig4_outbound -c -R -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -l $size -t 64 -Q 64 -D 30 $flags"
    pid=$(remote_bg "$CLT_HOST" "$pass_log" "$pass_cmd")
    sleep 1

    req_cmd="cd '$BENCH_DIR' && ./fig4_outbound -s -R -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -l $size -t 64 -Q 64 -D 3 $flags"
    set +e
    remote "${SRV_HOST:-local}" "$req_cmd" | tee "$req_log"
    rc=${PIPESTATUS[0]}
    set -e
    kill_pid "$pid"
    kill_bench "$CLT_HOST"
    sleep 0.5

    [[ $rc -eq 0 ]] || { log "WARN fail $curve size=$size"; continue; }
    line=$(grep -E '^fig4 outbound:' "$req_log" | tail -1 || true)
    mops=$(echo "$line" | sed -n 's/.*: \([0-9.]*\) Mops.*/\1/p')
    [[ -n "$mops" ]] || { log "WARN parse $req_log"; continue; }
    append_csv "$CSV" "$curve,$size,$mops"
  done
done

log "wrote $CSV"
python3 "$SCRIPT_DIR/plot_paper_figs.py" --fig 4 --results-dir "$RESULTS_DIR"
cleanup_logs "$RESULTS_DIR"
