#!/usr/bin/env bash
# Collect Fig.6 QP-scaling curves -> results/fig6.csv
#
# Paper: all-to-all among N processes ⇒ N² QPs at RNICS.
# Binary takes -q N (paper's N) and creates N² connected QPs for
# Out-WRITE / In-WRITE; Out-SEND-UD stays at 1 QP.
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
log "fig6 paper N (connected ⇒ N² QPs); size=${SIZE}B inline+unsig=${PAPER_UNSIG_SCALE}"

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
  log "fig6 $curve N=$n mode=$mode (connected QPs=$((n * n)))"

  local pass_log="$RESULTS_DIR/fig6_pass_${curve//\//_}_${n}.log"
  local req_log="$RESULTS_DIR/fig6_req_${curve//\//_}_${n}.log"

  local pass_host="$CLT_HOST"
  local req_host="${SRV_HOST:-local}"
  local pass_cmd="cd '$BENCH_DIR' && ./fig6_scale -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -M $mode -q $n -l $SIZE -Q $PAPER_UNSIG_SCALE -D $PAPER_PASSIVE_SEC"
  local req_cmd="cd '$BENCH_DIR' && ./fig6_scale -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -M $mode -q $n -l $SIZE -Q $PAPER_UNSIG_SCALE -D $PAPER_TPUT_SEC"

  if [[ "$mode" == "in-write" ]]; then
    pass_host="${SRV_HOST:-local}"
    req_host="$CLT_HOST"
    pass_cmd="cd '$BENCH_DIR' && ./fig6_scale -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -M $mode -q $n -l $SIZE -Q $PAPER_UNSIG_SCALE -D $PAPER_PASSIVE_SEC"
    req_cmd="cd '$BENCH_DIR' && ./fig6_scale -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -M $mode -q $n -l $SIZE -Q $PAPER_UNSIG_SCALE -D $PAPER_TPUT_SEC"
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
cleanup_logs "$RESULTS_DIR"
