#!/usr/bin/env bash
# Collect Fig.5 ECHO bars (32B) -> results/fig5.csv
# Paper bars: SEND/SEND, WR/WR, WR/SEND × {basic, +unreliable, +unsignalled, +inlined}
# Usage: ./scripts/collect_fig5.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

CSV="$RESULTS_DIR/fig5.csv"
csv_header "$CSV" "echo_type,opt,mops"

SIZE=32
WINDOW=32
DURATION=3

# echo_type|binary_mode|opt_name|extra_flags
declare -a JOBS=(
  "SEND/SEND|ss|basic|--rc --no-inline -Q 1"
  "SEND/SEND|ss|+unreliable|--no-inline -Q 1"
  "SEND/SEND|ss|+unsignalled|--no-inline -Q 64"
  "SEND/SEND|ss|+inlined|-Q 64"
  "WR/WR|ww|basic|--rc --no-inline -Q 1"
  "WR/WR|ww|+unreliable|--no-inline -Q 1"
  "WR/WR|ww|+unsignalled|--no-inline -Q 64"
  "WR/WR|ww|+inlined|-Q 64"
  "WR/SEND|ws|basic|--rc --no-inline -Q 1"
  "WR/SEND|ws|+unreliable|--no-inline -Q 1"
  "WR/SEND|ws|+unsignalled|--no-inline -Q 64"
  "WR/SEND|ws|+inlined|-Q 64"
)

sync_bins
kill_bench "$SRV_HOST"
kill_bench "$CLT_HOST"
sleep 1

PORT_BASE=18530
idx=0
for job in "${JOBS[@]}"; do
  IFS='|' read -r etype mode opt flags <<<"$job"
  port=$((PORT_BASE + idx))
  idx=$((idx + 1))
  log "fig5 $etype $opt mode=$mode"

  srv_log="$RESULTS_DIR/fig5_srv_${etype//\//_}_${opt}.log"
  clt_log="$RESULTS_DIR/fig5_clt_${etype//\//_}_${opt}.log"

  srv_cmd="cd '$BENCH_DIR' && ./fig5_echo -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -m $mode -l $SIZE -w $WINDOW -D $((DURATION + 5)) $flags"
  pid=$(remote_bg "${SRV_HOST:-local}" "$srv_log" "$srv_cmd")
  sleep 1

  clt_cmd="cd '$BENCH_DIR' && ./fig5_echo -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -m $mode -l $SIZE -w $WINDOW -D $DURATION $flags"
  set +e
  remote "$CLT_HOST" "$clt_cmd" | tee "$clt_log"
  rc=${PIPESTATUS[0]}
  set -e
  kill_pid "$pid"
  kill_bench "$SRV_HOST"
  sleep 0.5

  [[ $rc -eq 0 ]] || { log "WARN fail $etype $opt"; continue; }
  line=$(grep -E '^fig5 .*ECHO:' "$clt_log" | tail -1 || true)
  mops=$(echo "$line" | sed -n 's/.*: \([0-9.]*\) Mops.*/\1/p')
  [[ -n "$mops" ]] || { log "WARN parse $clt_log"; continue; }
  append_csv "$CSV" "$etype,$opt,$mops"
done

log "wrote $CSV"
python3 "$SCRIPT_DIR/plot_paper_figs.py" --fig 5 --results-dir "$RESULTS_DIR"
