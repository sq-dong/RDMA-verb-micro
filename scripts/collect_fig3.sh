#!/usr/bin/env bash
# Collect Fig.3 inbound throughput -> results/fig3.csv
# Usage: ./scripts/collect_fig3.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

CSV="$RESULTS_DIR/fig3.csv"
csv_header "$CSV" "curve,size,mops"

SIZES=(2 8 16 32 64 128 256)
# Paper curves: WRITE-UC, WRITE-RC, READ-RC
declare -a JOBS=(
  "WRITE-UC|--uc"
  "WRITE-RC|--rc"
  "READ-RC|--read"
)

sync_bins
kill_bench "$SRV_HOST"
kill_bench "$CLT_HOST"
sleep 1

PORT_BASE=18510
idx=0
for job in "${JOBS[@]}"; do
  curve="${job%%|*}"
  flags="${job#*|}"
  for size in "${SIZES[@]}"; do
    port=$((PORT_BASE + idx))
    idx=$((idx + 1))
    log "fig3 $curve size=$size"

    srv_log="$RESULTS_DIR/fig3_srv_${curve}_${size}.log"
    clt_log="$RESULTS_DIR/fig3_clt_${curve}_${size}.log"

    srv_cmd="cd '$BENCH_DIR' && ./fig3_inbound -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -l $size $flags"
    pid=$(remote_bg "${SRV_HOST:-local}" "$srv_log" "$srv_cmd")
    sleep 1

    clt_cmd="cd '$BENCH_DIR' && ./fig3_inbound -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -l $size -t 64 -Q 64 -D 3 $flags"
    set +e
    remote "$CLT_HOST" "$clt_cmd" | tee "$clt_log"
    rc=${PIPESTATUS[0]}
    set -e
    kill_pid "$pid"
    kill_bench "$SRV_HOST"
    sleep 0.5

    [[ $rc -eq 0 ]] || { log "WARN fail $curve size=$size"; continue; }
    line=$(grep -E '^fig3 inbound client:' "$clt_log" | tail -1 || true)
    mops=$(echo "$line" | sed -n 's/.*: \([0-9.]*\) Mops.*/\1/p')
    [[ -n "$mops" ]] || { log "WARN parse $clt_log"; continue; }
    append_csv "$CSV" "$curve,$size,$mops"
  done
done

log "wrote $CSV"
python3 "$SCRIPT_DIR/plot_paper_figs.py" --fig 3 --results-dir "$RESULTS_DIR"
