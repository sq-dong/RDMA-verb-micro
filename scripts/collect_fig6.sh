#!/usr/bin/env bash
# Collect Fig.6 QP-scaling curves -> results/fig6.csv
#
# Paper Sec.3.3 / Fig.6 — true multi-process all-to-all:
#   X-axis = N (#client procs = #server procs).
#   Out-WRITE: N MS procs × N client procs; each proc has N QPs → N² at RNICS.
#   In-WRITE:  N client procs post toward N MS procs (same mesh).
#   Out-SEND:  1 MS proc (-I 0) with 1 UD QP + N AHs; N client procs.
#
# Launch: one SSH per remote host forks N local workers (avoids MaxStartups).
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
# FIG6_ONLY=in-write|out-write|out-send — re-measure one curve, keep others in CSV.
FIG6_ONLY="${FIG6_ONLY:-}"
if [[ -z "$FIG6_ONLY" ]]; then
  rm -f "$CSV"
  csv_header "$CSV" "curve,n,mops"
elif [[ -f "$CSV" ]]; then
  # Drop rows for the curve we are about to replace.
  case "$FIG6_ONLY" in
    in-write)  grep -v '^In-WRITE-UC,' "$CSV" >"$CSV.tmp" || true ;;
    out-write) grep -v '^Out-WRITE-UC,' "$CSV" >"$CSV.tmp" || true ;;
    out-send)  grep -v '^Out-SEND-UD,' "$CSV" >"$CSV.tmp" || true ;;
    *) log "ERROR: FIG6_ONLY=$FIG6_ONLY (want in-write|out-write|out-send)"; exit 1 ;;
  esac
  mv "$CSV.tmp" "$CSV"
  csv_header "$CSV" "curve,n,mops"
else
  csv_header "$CSV" "curve,n,mops"
fi
trap 'cleanup_logs "$RESULTS_DIR"; ssh $SSH_OPTS "$CLT_HOST" "rm -rf \"$RESULTS_DIR\"/fig6_N* \"$RESULTS_DIR\"/fig6_clt_* \"$RESULTS_DIR\"/*.log" 2>/dev/null || true; kill_bench "$SRV_HOST"; kill_bench "$CLT_HOST"' EXIT

NQPS=("${PAPER_NQPS[@]}")
SIZE=$PAPER_MSG_SIZE
PL=$PAPER_POSTLIST_SCALE
UQ=$PAPER_UNSIG_SCALE
log "fig6 multi-proc all-to-all: size=${SIZE}B postlist=$PL unsig=$UQ N∈${NQPS[*]}${FIG6_ONLY:+ only=$FIG6_ONLY}"

sync_bins
kill_bench "$SRV_HOST"
kill_bench "$CLT_HOST"
sleep 1

PORT_BASE=18540
idx=0

# Sum Mops from any number of log files (pass expanded paths as "$@").
sum_mops_files() {
  local pattern=$1
  shift
  local total=0 f line m
  for f in "$@"; do
    [[ -f "$f" ]] || continue
    line=$(grep -E "$pattern" "$f" | tail -1 || true)
    m=$(echo "$line" | sed -n 's/.*: \([0-9.]*\) Mops.*/\1/p')
    [[ -n "$m" ]] || continue
    total=$(python3 -c "print(round(float('$total') + float('$m'), 4))")
  done
  printf '%s' "$total"
}

run_out_write() {
  local n=$1
  local port=$((PORT_BASE + idx * 64))
  idx=$((idx + 1))
  log "fig6 Out-WRITE-UC paper_N=$n (N×N procs)"

  local -a ms_pids=()
  local i clt_ssh mops
  local logdir="$RESULTS_DIR/fig6_N${n}_outwrite"
  mkdir -p "$logdir"

  for i in $(seq 0 $((n - 1))); do
    (cd "$BENCH_DIR" && ./fig6_scale -s -d "$SRV_DEV" -a "$SRV_IP" -p "$port" -x "$SRV_GID" \
      -M out-write -q "$n" -I "$i" -t "$PL" -l "$SIZE" -Q "$UQ" -D "$PAPER_TPUT_SEC") \
      >"$logdir/ms_$i.log" 2>&1 &
    ms_pids+=($!)
  done
  sleep 1

  # shellcheck disable=SC2086
  ssh $SSH_OPTS "$CLT_HOST" "cd '$BENCH_DIR' && mkdir -p '$logdir' && \
    for i in \$(seq 0 $((n - 1))); do \
      ./fig6_scale -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID \
        -M out-write -q $n -I \$i -t $PL -l $SIZE -Q $UQ -D $PAPER_PASSIVE_SEC \
        >$logdir/clt_\$i.log 2>&1 & \
    done; wait" &
  clt_ssh=$!

  for pid in "${ms_pids[@]}"; do wait "$pid" || true; done
  kill "$clt_ssh" 2>/dev/null || true
  kill_bench "$SRV_HOST"
  kill_bench "$CLT_HOST"
  sleep 0.4

  mops=$(sum_mops_files '^fig6 Out-WRITE:' "$logdir"/ms_*.log)
  if [[ -z "$mops" || "$mops" == "0" || "$mops" == "0.0" ]]; then
    log "WARN fail Out-WRITE n=$n (logs in $logdir)"
    return 0
  fi
  append_csv "$CSV" "Out-WRITE-UC,$n,$mops"
  log "fig6 Out-WRITE-UC N=$n total=${mops} Mops (from $(ls "$logdir"/ms_*.log 2>/dev/null | wc -l) procs)"
}

run_in_write() {
  local n=$1
  local port=$((PORT_BASE + idx * 64))
  idx=$((idx + 1))
  log "fig6 In-WRITE-UC paper_N=$n (N×N procs)"

  local -a ms_pids=()
  local i mops summary
  local logdir="$RESULTS_DIR/fig6_N${n}_inwrite"
  mkdir -p "$logdir"

  for i in $(seq 0 $((n - 1))); do
    (cd "$BENCH_DIR" && ./fig6_scale -s -d "$SRV_DEV" -a "$SRV_IP" -p "$port" -x "$SRV_GID" \
      -M in-write -q "$n" -I "$i" -t "$PL" -l "$SIZE" -Q "$UQ" -D "$PAPER_PASSIVE_SEC") \
      >"$logdir/ms_$i.log" 2>&1 &
    ms_pids+=($!)
  done
  sleep 1

  # Remote clients: per-id logs (\$i must expand on remote, not locally).
  # shellcheck disable=SC2086
  summary=$(ssh $SSH_OPTS "$CLT_HOST" bash -s <<EOF
set -e
cd '$BENCH_DIR'
LOGDIR='$logdir'
mkdir -p "\$LOGDIR"
rm -f "\$LOGDIR"/clt_*.log "\$LOGDIR"/summary.txt
for i in \$(seq 0 $((n - 1))); do
  ./fig6_scale -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID \
    -M in-write -q $n -I \$i -t $PL -l $SIZE -Q $UQ -D $PAPER_TPUT_SEC \
    >"\$LOGDIR/clt_\$i.log" 2>&1 &
done
wait
grep -h '^fig6 In-WRITE:' "\$LOGDIR"/clt_*.log || true
EOF
)

  for pid in "${ms_pids[@]}"; do kill_pid "$pid" 2>/dev/null || true; done
  kill_bench "$SRV_HOST"
  kill_bench "$CLT_HOST"
  sleep 0.4

  # Prefer SSH-captured lines; fall back to any local copies.
  printf '%s\n' "$summary" >"$logdir/summary.txt"
  # Lines look like: fig6 In-WRITE: 1.23 Mops  N=...
  mops=$(printf '%s\n' "$summary" | awk '
    /^fig6 In-WRITE:/ {
      for (i = 1; i <= NF; i++)
        if ($i == "Mops" && i > 1) { s += $(i - 1); break }
    }
    END { if (s != "") printf "%.2f", s }')
  if [[ -z "$mops" || "$mops" == "0" || "$mops" == "0.00" ]]; then
    log "WARN fail In-WRITE n=$n (summary: $(wc -l <"$logdir/summary.txt") lines)"
    head -5 "$logdir/summary.txt" >&2 || true
    return 0
  fi
  append_csv "$CSV" "In-WRITE-UC,$n,$mops"
  log "fig6 In-WRITE-UC N=$n total=${mops} Mops"
}

run_out_send() {
  local n=$1
  local port=$((PORT_BASE + idx * 64))
  idx=$((idx + 1))
  log "fig6 Out-SEND-UD paper_N=$n (1 MS + N clients)"

  local logdir="$RESULTS_DIR/fig6_N${n}_outsend"
  mkdir -p "$logdir"
  local req_log="$logdir/ms_0.log"
  local ms_pid clt_ssh mops

  (cd "$BENCH_DIR" && ./fig6_scale -s -d "$SRV_DEV" -a "$SRV_IP" -p "$port" -x "$SRV_GID" \
    -M out-send-ud -q "$n" -I 0 -t "$PL" -l "$SIZE" -Q "$UQ" -D "$PAPER_TPUT_SEC") \
    >"$req_log" 2>&1 &
  ms_pid=$!
  sleep 0.8

  # shellcheck disable=SC2086
  ssh $SSH_OPTS "$CLT_HOST" "cd '$BENCH_DIR' && mkdir -p '$logdir' && \
    for i in \$(seq 0 $((n - 1))); do \
      ./fig6_scale -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID \
        -M out-send-ud -q $n -I \$i -t $PL -l $SIZE -Q $UQ -D $PAPER_PASSIVE_SEC \
        >$logdir/clt_\$i.log 2>&1 & \
    done; wait" &
  clt_ssh=$!

  wait "$ms_pid" || true
  kill "$clt_ssh" 2>/dev/null || true
  kill_bench "$SRV_HOST"
  kill_bench "$CLT_HOST"
  sleep 0.4

  mops=$(sum_mops_files '^fig6 Out-SEND:' "$req_log")
  if [[ -z "$mops" || "$mops" == "0" || "$mops" == "0.0" ]]; then
    log "WARN fail Out-SEND n=$n"
    return 0
  fi
  append_csv "$CSV" "Out-SEND-UD,$n,$mops"
  log "fig6 Out-SEND-UD N=$n total=${mops} Mops"
}

if [[ -z "$FIG6_ONLY" || "$FIG6_ONLY" == "out-write" ]]; then
  for n in "${NQPS[@]}"; do
    run_out_write "$n"
  done
fi
if [[ -z "$FIG6_ONLY" || "$FIG6_ONLY" == "out-send" ]]; then
  for n in "${NQPS[@]}"; do
    run_out_send "$n"
  done
fi
if [[ -z "$FIG6_ONLY" || "$FIG6_ONLY" == "in-write" ]]; then
  for n in "${NQPS[@]}"; do
    run_in_write "$n"
  done
fi

# Keep per-N dirs for debugging this run; still scrub loose *.log via trap.
log "wrote $CSV"
python3 "$SCRIPT_DIR/plot_paper_figs.py" --fig 6 --results-dir "$RESULTS_DIR"
cat "$CSV"
