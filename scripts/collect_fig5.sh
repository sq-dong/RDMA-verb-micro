#!/usr/bin/env bash
# Collect Fig.5 ECHO bars (32B) -> results/fig5.csv
# Paper bars: SEND/SEND, WR/WR, WR/SEND × {basic, +unreliable, +unsignalled, +inlined}
#
# Notes vs rdma_bench/{ww,ws,ss}-echo:
#   - ww-echo always inlines; we still sweep --no-inline for the first 3 bars.
#   - ws-echo uses NUM_WORKERS=5 + multi-client; this collector is 1:1 (one
#     client ↔ one server thread). Absolute WR/SEND and SEND/SEND Mops are
#     therefore much lower than the paper; relative bar order is what we check.
#   - Each bar = median of PAPER_FIG5_TRIALS runs (short echo runs are noisy).
#
# Usage: ./scripts/collect_fig5.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=paper_config.sh
source "$SCRIPT_DIR/paper_config.sh"
paper_assert_inline_sync

CSV="$RESULTS_DIR/fig5.csv"
rm -f "$CSV"
csv_header "$CSV" "echo_type,opt,mops"
# Always scrub trial logs — including early ERROR exits that skip the footer.
trap 'cleanup_logs "$RESULTS_DIR"' EXIT

SIZE=$PAPER_MSG_SIZE
WINDOW=$PAPER_ECHO_WINDOW
DURATION=$PAPER_FIG5_SEC
TRIALS=$PAPER_FIG5_TRIALS
WARMUP=$PAPER_FIG5_WARMUP_TRIALS
GAP=$PAPER_FIG5_GAP_SEC
TOTAL_RUNS=$((TRIALS + WARMUP))
log "fig5 size=${SIZE}B window=$WINDOW measure=${TRIALS} warmup=${WARMUP} sec=$DURATION unsig=$PAPER_UNSIG_ECHO"

# echo_type|binary_mode|opt_name|extra_flags
declare -a JOBS=(
  "SEND/SEND|ss|basic|--rc --no-inline -Q 1"
  "SEND/SEND|ss|+unreliable|--no-inline -Q 1"
  "SEND/SEND|ss|+unsignalled|--no-inline -Q $PAPER_UNSIG_ECHO"
  "SEND/SEND|ss|+inlined|-Q $PAPER_UNSIG_ECHO"
  "WR/WR|ww|basic|--rc --no-inline -Q 1"
  "WR/WR|ww|+unreliable|--no-inline -Q 1"
  "WR/WR|ww|+unsignalled|--no-inline -Q $PAPER_UNSIG_ECHO"
  "WR/WR|ww|+inlined|-Q $PAPER_UNSIG_ECHO"
  "WR/SEND|ws|basic|--rc --no-inline -Q 1"
  "WR/SEND|ws|+unreliable|--no-inline -Q 1"
  "WR/SEND|ws|+unsignalled|--no-inline -Q $PAPER_UNSIG_ECHO"
  "WR/SEND|ws|+inlined|-Q $PAPER_UNSIG_ECHO"
)

median_of() {
  # stdin: one float per line → stdout: median (ignore non-numeric noise)
  awk '/^[0-9]+(\.[0-9]+)?$/ { a[++n] = $1 + 0 }
    END {
      if (n == 0) { print ""; exit }
      # insertion sort (n is tiny)
      for (i = 2; i <= n; i++) {
        v = a[i]; j = i - 1
        while (j >= 1 && a[j] > v) { a[j + 1] = a[j]; j-- }
        a[j + 1] = v
      }
      if (n % 2) printf "%.2f\n", a[(n + 1) / 2]
      else printf "%.2f\n", (a[n / 2] + a[n / 2 + 1]) / 2
    }'
}

# stdout MUST be only the Mops float — client chatter goes to the log / stderr.
run_one_trial() {
  local etype=$1 mode=$2 opt=$3 flags=$4 port=$5 trial=$6
  local srv_log clt_log srv_cmd clt_cmd pid rc line mops
  srv_log="$RESULTS_DIR/fig5_srv_${etype//\//_}_${opt}_t${trial}.log"
  clt_log="$RESULTS_DIR/fig5_clt_${etype//\//_}_${opt}_t${trial}.log"

  srv_cmd="cd '$BENCH_DIR' && ./fig5_echo -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -m $mode -l $SIZE -w $WINDOW -D $((DURATION + 8)) $flags"
  pid=$(remote_bg "${SRV_HOST:-local}" "$srv_log" "$srv_cmd")
  sleep 1

  clt_cmd="cd '$BENCH_DIR' && timeout $((DURATION + 40))s ./fig5_echo -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -m $mode -l $SIZE -w $WINDOW -D $DURATION $flags"
  set +e
  # tee to stderr so `mops=$(run_one_trial …)` does not swallow banner/ECHO lines
  # (those sorted as 0 under sort -n → median 0.00 for every bar).
  remote "$CLT_HOST" "$clt_cmd" 2>&1 | tee "$clt_log" >&2
  rc=${PIPESTATUS[0]}
  set -e
  kill_pid "$pid"
  kill_bench "$SRV_HOST"
  kill_bench "$CLT_HOST"
  sleep 0.5

  if [[ $rc -eq 124 ]]; then
    log "WARN timeout $etype $opt trial=$trial"
    return 1
  fi
  if [[ $rc -ne 0 ]]; then
    log "WARN fail $etype $opt trial=$trial rc=$rc"
    return 1
  fi
  line=$(grep -E '^fig5 .*ECHO:' "$clt_log" | tail -1 || true)
  mops=$(echo "$line" | sed -n 's/.*: \([0-9.]*\) Mops.*/\1/p')
  if [[ -z "$mops" ]]; then
    log "WARN parse $clt_log trial=$trial"
    return 1
  fi
  printf '%s\n' "$mops"
  return 0
}

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
  log "fig5 $etype $opt mode=$mode (${WARMUP} warmup + ${TRIALS} measure)"

  samples=""
  t=0
  while [[ $t -lt $TOTAL_RUNS ]]; do
    t=$((t + 1))
    mops=$(run_one_trial "$etype" "$mode" "$opt" "$flags" "$port" "$t" || true)
    if [[ $t -le $WARMUP ]]; then
      log "  warmup $t: ${mops:-fail} Mops (discarded)"
    elif [[ -n "${mops:-}" ]]; then
      samples+="${mops}"$'\n'
      log "  trial $((t - WARMUP)): $mops Mops"
    fi
    sleep "$GAP"
  done

  med=$(printf '%s' "$samples" | median_of)
  if [[ -z "$med" ]]; then
    log "WARN no samples for $etype $opt"
    continue
  fi
  log "  median: $med Mops"
  append_csv "$CSV" "$etype,$opt,$med"
done

nrows=$(grep -cve '^\s*$' "$CSV" || true)
# header + 12 data rows
if [[ "$nrows" -lt 13 ]]; then
  log "ERROR: fig5.csv incomplete ($((nrows - 1))/12 bars). Check WARN lines above."
  exit 1
fi

log "wrote $CSV ($((nrows - 1)) bars, median of $TRIALS after $WARMUP warmup)"
python3 "$SCRIPT_DIR/plot_paper_figs.py" --fig 5 --results-dir "$RESULTS_DIR"
# logs removed by EXIT trap
