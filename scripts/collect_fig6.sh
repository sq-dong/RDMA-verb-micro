#!/usr/bin/env bash
# Collect Fig.6 QP-scaling curves -> results/fig6.csv
#
# Paper Sec.3.3 / Fig.6:
#   X-axis = N (#client procs = #server procs).
#   Out-WRITE: N² QPs both sides (requester cache pressure).
#   In-WRITE:  requester N QPs, responder N² QPs (extras self-paired).
#   Out-SEND:  1 UD QP + N AHs / N remote UD QPs.
#   Inline + nearly-unsignaled (postlist=1, unsig=4 ≈ sender-scalability).
#
# Usage: ./scripts/collect_fig6.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=paper_config.sh
source "$SCRIPT_DIR/paper_config.sh"
paper_assert_inline_sync

CSV="$RESULTS_DIR/fig6.csv"
rm -f "$CSV"
csv_header "$CSV" "curve,n,mops"
trap 'cleanup_logs "$RESULTS_DIR"' EXIT

NQPS=("${PAPER_NQPS[@]}")
SIZE=$PAPER_MSG_SIZE
PL=$PAPER_POSTLIST_SCALE
UQ=$PAPER_UNSIG_SCALE
log "fig6 paper-N: Out-WRITE N²/N², In-WRITE N/N², Out-SEND 1+N AH; size=${SIZE}B postlist=$PL unsig=$UQ"

sync_bins
kill_bench "$SRV_HOST"
kill_bench "$CLT_HOST"
sleep 1

PORT_BASE=18540
idx=0

run_fig6_pair() {
  local curve=$1
  local mode=$2
  local n=$3
  local port=$((PORT_BASE + idx))
  idx=$((idx + 1))
  log "fig6 $curve paper_N=$n mode=$mode"

  local pass_log="$RESULTS_DIR/fig6_pass_${curve//\//_}_${n}.log"
  local req_log="$RESULTS_DIR/fig6_req_${curve//\//_}_${n}.log"

  # -q is always paper N; binary derives local QP counts.
  local pass_host="$CLT_HOST"
  local req_host="${SRV_HOST:-local}"
  local pass_cmd="cd '$BENCH_DIR' && ./fig6_scale -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -M $mode -q $n -t $PL -l $SIZE -Q $UQ -D $PAPER_PASSIVE_SEC"
  local req_cmd="cd '$BENCH_DIR' && ./fig6_scale -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -M $mode -q $n -t $PL -l $SIZE -Q $UQ -D $PAPER_TPUT_SEC"

  if [[ "$mode" == "in-write" ]]; then
    pass_host="${SRV_HOST:-local}"
    req_host="$CLT_HOST"
    pass_cmd="cd '$BENCH_DIR' && ./fig6_scale -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -M $mode -q $n -t $PL -l $SIZE -Q $UQ -D $PAPER_PASSIVE_SEC"
    req_cmd="cd '$BENCH_DIR' && ./fig6_scale -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -M $mode -q $n -t $PL -l $SIZE -Q $UQ -D $PAPER_TPUT_SEC"
  fi

  pid=$(remote_bg "$pass_host" "$pass_log" "$pass_cmd")
  sleep 1.2

  set +e
  remote "$req_host" "$req_cmd" 2>&1 | tee "$req_log" >&2
  rc=${PIPESTATUS[0]}
  set -e
  kill_pid "$pid"
  kill_bench "$SRV_HOST"
  kill_bench "$CLT_HOST"
  sleep 0.5

  [[ $rc -eq 0 ]] || { log "WARN fail $curve n=$n"; return 0; }

  local line mops
  case "$mode" in
    out-write) line=$(grep -E '^fig6 Out-WRITE:' "$req_log" | tail -1 || true) ;;
    in-write)  line=$(grep -E '^fig6 In-WRITE:' "$req_log" | tail -1 || true) ;;
    out-send-ud) line=$(grep -E '^fig6 Out-SEND:' "$req_log" | tail -1 || true) ;;
  esac
  mops=$(echo "$line" | sed -n 's/.*: \([0-9.]*\) Mops.*/\1/p')
  [[ -n "$mops" ]] || { log "WARN parse $req_log"; return 0; }
  append_csv "$CSV" "$curve,$n,$mops"
  log "fig6 $curve N=$n total=${mops} Mops"
}

for n in "${NQPS[@]}"; do
  run_fig6_pair "Out-WRITE-UC" "out-write" "$n"
done
for n in "${NQPS[@]}"; do
  run_fig6_pair "Out-SEND-UD" "out-send-ud" "$n"
done
for n in "${NQPS[@]}"; do
  run_fig6_pair "In-WRITE-UC" "in-write" "$n"
done

log "wrote $CSV"
python3 "$SCRIPT_DIR/plot_paper_figs.py" --fig 6 --results-dir "$RESULTS_DIR"
