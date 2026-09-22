#!/usr/bin/env bash
# Collect Fig.6 QP-scaling curves -> results/fig6.csv
#
# Paper Sec.3.3 / Fig.6:
#   N client procs + N server procs, all-to-all ⇒ N² QPs at RNICS.
#   X-axis = N (1..16).  We approximate with one process owning N² QPs.
#   Out-SEND-UD keeps 1 UD QP (datagram scales by design).
#   -t POSTLIST amortizes doorbells so low-N is not stuck at ~5 Mops.
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

NQPS=("${PAPER_NQPS[@]}")
SIZE=$PAPER_MSG_SIZE
PL=$PAPER_POSTLIST
log "fig6 paper-N all-to-all (QPs=N*N); size=${SIZE}B postlist=$PL unsig=${PAPER_UNSIG_SCALE}"

sync_bins
kill_bench "$SRV_HOST"
kill_bench "$CLT_HOST"
sleep 1

PORT_BASE=18540
idx=0

fig6_nqp() {
  local n=$1
  local mode=$2
  if [[ "$mode" == "out-send-ud" ]]; then
    echo 1
    return
  fi
  local q=$((n * n))
  if [[ $q -gt 512 ]]; then
    q=512
  fi
  echo "$q"
}

run_fig6_pair() {
  local curve=$1
  local mode=$2
  local n=$3
  local q
  q=$(fig6_nqp "$n" "$mode")
  local port=$((PORT_BASE + idx))
  idx=$((idx + 1))
  log "fig6 $curve paper_N=$n nqp=$q mode=$mode"

  local pass_log="$RESULTS_DIR/fig6_pass_${curve//\//_}_${n}.log"
  local req_log="$RESULTS_DIR/fig6_req_${curve//\//_}_${n}.log"

  local pass_host="$CLT_HOST"
  local req_host="${SRV_HOST:-local}"
  local pass_cmd="cd '$BENCH_DIR' && ./fig6_scale -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -M $mode -q $q -t $PL -l $SIZE -Q $PAPER_UNSIG_SCALE -D $PAPER_PASSIVE_SEC"
  local req_cmd="cd '$BENCH_DIR' && ./fig6_scale -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -M $mode -q $q -t $PL -l $SIZE -Q $PAPER_UNSIG_SCALE -D $PAPER_TPUT_SEC"

  if [[ "$mode" == "in-write" ]]; then
    pass_host="${SRV_HOST:-local}"
    req_host="$CLT_HOST"
    pass_cmd="cd '$BENCH_DIR' && ./fig6_scale -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -M $mode -q $q -t $PL -l $SIZE -Q $PAPER_UNSIG_SCALE -D $PAPER_PASSIVE_SEC"
    req_cmd="cd '$BENCH_DIR' && ./fig6_scale -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -M $mode -q $q -t $PL -l $SIZE -Q $PAPER_UNSIG_SCALE -D $PAPER_TPUT_SEC"
  fi

  pid=$(remote_bg "$pass_host" "$pass_log" "$pass_cmd")
  sleep 1

  set +e
  remote "$req_host" "$req_cmd" | tee "$req_log"
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
  # CSV n = paper N (process count), not QP count
  append_csv "$CSV" "$curve,$n,$mops"
  log "fig6 $curve N=$n nqp=$q total=${mops} Mops"
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
cleanup_logs "$RESULTS_DIR"
