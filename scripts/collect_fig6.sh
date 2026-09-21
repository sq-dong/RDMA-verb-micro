#!/usr/bin/env bash
# Collect Fig.6 QP-scaling curves -> results/fig6.csv
# Out-WRITE-UC via fig6_scale; Out-SEND-UD via fig4 -m send_ud with -q approximated
# by repeating single-QP sender (UD needs no multi-QP). In-WRITE-UC via fig3
# multi-client is approximated by fig6 inbound sketch if enabled later; here we
# run Out-WRITE-UC fanout sweep and Out-SEND-UD (flat) for the paper's main story.
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
log "fig6 size=${SIZE}B inlined+unsignaled (paper); inline ceiling=${PAPER_INLINE_MAX}B"

sync_bins
kill_bench "$SRV_HOST"
kill_bench "$CLT_HOST"
sleep 1

PORT_BASE=18540
idx=0

# ---- Out-WRITE-UC fanout (inline UC WRITE, paper caption) ----
for n in "${NQPS[@]}"; do
  port=$((PORT_BASE + idx))
  idx=$((idx + 1))
  log "fig6 Out-WRITE-UC nqp=$n"

  pass_log="$RESULTS_DIR/fig6_pass_write_$n.log"
  req_log="$RESULTS_DIR/fig6_req_write_$n.log"

  pass_cmd="cd '$BENCH_DIR' && ./fig6_scale -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -q $n -l $SIZE -Q $PAPER_UNSIG_SCALE -D $PAPER_PASSIVE_SEC"
  pid=$(remote_bg "$CLT_HOST" "$pass_log" "$pass_cmd")
  sleep 1

  req_cmd="cd '$BENCH_DIR' && ./fig6_scale -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -q $n -l $SIZE -Q $PAPER_UNSIG_SCALE -D $PAPER_TPUT_SEC"
  set +e
  remote "${SRV_HOST:-local}" "$req_cmd" | tee "$req_log"
  rc=${PIPESTATUS[0]}
  set -e
  kill_pid "$pid"
  kill_bench "$CLT_HOST"
  sleep 0.5

  [[ $rc -eq 0 ]] || { log "WARN Out-WRITE n=$n"; continue; }
  line=$(grep -E '^fig6 Out-WRITE:' "$req_log" | tail -1 || true)
  mops=$(echo "$line" | sed -n 's/.*: \([0-9.]*\) Mops.*/\1/p')
  [[ -n "$mops" ]] || continue
  append_csv "$CSV" "Out-WRITE-UC,$n,$mops"
done

# ---- Out-SEND-UD (inlined UD SEND; flat vs n) ----
for n in "${NQPS[@]}"; do
  port=$((PORT_BASE + idx))
  idx=$((idx + 1))
  log "fig6 Out-SEND-UD n=$n (single UD QP, inlined)"

  pass_log="$RESULTS_DIR/fig6_pass_send_$n.log"
  req_log="$RESULTS_DIR/fig6_req_send_$n.log"

  pass_cmd="cd '$BENCH_DIR' && ./fig4_outbound -c -R -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -m send_ud -l $SIZE -t $PAPER_POSTLIST -Q $PAPER_UNSIG -D $PAPER_PASSIVE_SEC"
  pid=$(remote_bg "$CLT_HOST" "$pass_log" "$pass_cmd")
  sleep 1

  req_cmd="cd '$BENCH_DIR' && ./fig4_outbound -s -R -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -m send_ud -l $SIZE -t $PAPER_POSTLIST -Q $PAPER_UNSIG -D $PAPER_TPUT_SEC"
  set +e
  remote "${SRV_HOST:-local}" "$req_cmd" | tee "$req_log"
  rc=${PIPESTATUS[0]}
  set -e
  kill_pid "$pid"
  kill_bench "$CLT_HOST"
  sleep 0.5

  [[ $rc -eq 0 ]] || { log "WARN Out-SEND n=$n"; continue; }
  line=$(grep -E '^fig4 outbound:' "$req_log" | tail -1 || true)
  mops=$(echo "$line" | sed -n 's/.*: \([0-9.]*\) Mops.*/\1/p')
  [[ -n "$mops" ]] || continue
  append_csv "$CSV" "Out-SEND-UD,$n,$mops"
done

# ---- In-WRITE-UC: paper Fig.6 uses inlined UC WRITE (unlike Fig.3 DMA WRITE) ----
for n in "${NQPS[@]}"; do
  port=$((PORT_BASE + idx))
  idx=$((idx + 1))
  log "fig6 In-WRITE-UC label_n=$n (inlined ${SIZE}B UC)"

  srv_log="$RESULTS_DIR/fig6_in_srv_$n.log"
  clt_log="$RESULTS_DIR/fig6_in_clt_$n.log"
  srv_cmd="cd '$BENCH_DIR' && ./fig3_inbound -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -l $SIZE --uc"
  pid=$(remote_bg "${SRV_HOST:-local}" "$srv_log" "$srv_cmd")
  sleep 1
  clt_cmd="cd '$BENCH_DIR' && ./fig3_inbound -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -l $SIZE -t $PAPER_POSTLIST -Q $PAPER_UNSIG -D $PAPER_TPUT_SEC --uc"
  set +e
  remote "$CLT_HOST" "$clt_cmd" | tee "$clt_log"
  rc=${PIPESTATUS[0]}
  set -e
  kill_pid "$pid"
  kill_bench "$SRV_HOST"
  sleep 0.5
  [[ $rc -eq 0 ]] || continue
  line=$(grep -E '^fig3 inbound client:' "$clt_log" | tail -1 || true)
  mops=$(echo "$line" | sed -n 's/.*: \([0-9.]*\) Mops.*/\1/p')
  [[ -n "$mops" ]] || continue
  append_csv "$CSV" "In-WRITE-UC,$n,$mops"
done

log "wrote $CSV"
python3 "$SCRIPT_DIR/plot_paper_figs.py" --fig 6 --results-dir "$RESULTS_DIR"
cleanup_logs "$RESULTS_DIR"
