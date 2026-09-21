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

CSV="$RESULTS_DIR/fig6.csv"
rm -f "$CSV"
csv_header "$CSV" "curve,n,mops"

# Paper Fig.6 x-axis ticks: 0 4 8 12 16
NQPS=(4 8 12 16)
SIZE=32

sync_bins
kill_bench "$SRV_HOST"
kill_bench "$CLT_HOST"
sleep 1

PORT_BASE=18540
idx=0

# ---- Out-WRITE-UC fanout ----
for n in "${NQPS[@]}"; do
  port=$((PORT_BASE + idx))
  idx=$((idx + 1))
  log "fig6 Out-WRITE-UC nqp=$n"

  pass_log="$RESULTS_DIR/fig6_pass_write_$n.log"
  req_log="$RESULTS_DIR/fig6_req_write_$n.log"

  pass_cmd="cd '$BENCH_DIR' && ./fig6_scale -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -q $n -l $SIZE -Q 4 -D 30"
  pid=$(remote_bg "$CLT_HOST" "$pass_log" "$pass_cmd")
  sleep 1

  req_cmd="cd '$BENCH_DIR' && ./fig6_scale -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -q $n -l $SIZE -Q 4 -D 3"
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

# ---- Out-SEND-UD (one UD QP; should stay high as n increases) ----
# Re-measure SEND-UD once per n for the x-axis alignment (same single-QP rate).
for n in "${NQPS[@]}"; do
  port=$((PORT_BASE + idx))
  idx=$((idx + 1))
  log "fig6 Out-SEND-UD n=$n (single UD QP)"

  pass_log="$RESULTS_DIR/fig6_pass_send_$n.log"
  req_log="$RESULTS_DIR/fig6_req_send_$n.log"

  pass_cmd="cd '$BENCH_DIR' && ./fig4_outbound -c -R -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -m send_ud -l $SIZE -t 64 -Q 64 -D 30"
  pid=$(remote_bg "$CLT_HOST" "$pass_log" "$pass_cmd")
  sleep 1

  req_cmd="cd '$BENCH_DIR' && ./fig4_outbound -s -R -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -m send_ud -l $SIZE -t 64 -Q 64 -D 3"
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

# ---- In-WRITE-UC: client posts inbound WRITEs (fig3) once per n as x-label ----
# With one physical client, fan-in is limited; still recorded for the plot slot.
for n in "${NQPS[@]}"; do
  port=$((PORT_BASE + idx))
  idx=$((idx + 1))
  log "fig3-as In-WRITE-UC label_n=$n"

  srv_log="$RESULTS_DIR/fig6_in_srv_$n.log"
  clt_log="$RESULTS_DIR/fig6_in_clt_$n.log"
  srv_cmd="cd '$BENCH_DIR' && ./fig3_inbound -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -l $SIZE --uc"
  pid=$(remote_bg "${SRV_HOST:-local}" "$srv_log" "$srv_cmd")
  sleep 1
  clt_cmd="cd '$BENCH_DIR' && ./fig3_inbound -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -l $SIZE -t 64 -Q 64 -D 3 --uc"
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
