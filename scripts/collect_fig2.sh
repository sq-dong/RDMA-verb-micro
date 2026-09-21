#!/usr/bin/env bash
# Collect Fig.2 latency curves -> results/fig2.csv then optional plot.
# Usage:
#   ./scripts/collect_fig2.sh
# Env: see scripts/lib.sh (SRV_IP, CLT_HOST, SRV_DEV, CLT_DEV, *_GID, BENCH_DIR)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

CSV="$RESULTS_DIR/fig2.csv"
csv_header "$CSV" "mode,size,avg_us,min_us,max_us,rtt_us,half_rtt_us"

SIZES=(2 8 16 32 64 128 256)
# Paper also shows larger for READ/WRITE; keep up to 256 for inline/ECHO.
MODES=(write write_inl read echo)

sync_bins
kill_bench "$SRV_HOST"
kill_bench "$CLT_HOST"
sleep 1

PORT_BASE=18500
idx=0
for mode in "${MODES[@]}"; do
  for size in "${SIZES[@]}"; do
    port=$((PORT_BASE + idx))
    idx=$((idx + 1))
    log "fig2 mode=$mode size=$size port=$port"

    srv_log="$RESULTS_DIR/fig2_srv_${mode}_${size}.log"
    clt_log="$RESULTS_DIR/fig2_clt_${mode}_${size}.log"

    srv_cmd="cd '$BENCH_DIR' && ./fig2_latency -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -m $mode -l $size -n 5000 -w 500"
    pid=$(remote_bg "${SRV_HOST:-local}" "$srv_log" "$srv_cmd")
    sleep 1

    clt_cmd="cd '$BENCH_DIR' && ./fig2_latency -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -m $mode -l $size -n 5000 -w 500"
    set +e
    remote "$CLT_HOST" "$clt_cmd" | tee "$clt_log"
    rc=${PIPESTATUS[0]}
    set -e

    kill_pid "$pid"
    kill_bench "$SRV_HOST"
    sleep 0.5

    if [[ $rc -ne 0 ]]; then
      log "WARN: client failed for mode=$mode size=$size (see $clt_log)"
      continue
    fi

    line=$(grep -E '^fig2 mode=' "$clt_log" | tail -1 || true)
    if [[ -z "$line" ]]; then
      log "WARN: no result line in $clt_log"
      continue
    fi

    if [[ "$mode" == "echo" ]]; then
      # fig2 mode=echo size=%d  rtt_avg=%.3f us  rtt/2=%.3f us  min_rtt=%.3f max_rtt=%.3f
      size_v=$(echo "$line" | sed -n 's/.*size=\([0-9]*\).*/\1/p')
      rtt=$(echo "$line" | sed -n 's/.*rtt_avg=\([0-9.]*\).*/\1/p')
      half=$(echo "$line" | sed -n 's/.*rtt\/2=\([0-9.]*\).*/\1/p')
      minv=$(echo "$line" | sed -n 's/.*min_rtt=\([0-9.]*\).*/\1/p')
      maxv=$(echo "$line" | sed -n 's/.*max_rtt=\([0-9.]*\).*/\1/p')
      append_csv "$CSV" "echo,$size_v,$half,$minv,$maxv,$rtt,$half"
      append_csv "$CSV" "echo_rtt,$size_v,$rtt,$minv,$maxv,$rtt,"
    else
      # fig2 mode=%s size=%d iters=%d  avg=%.3f us  min=%.3f  max=%.3f
      mode_v=$(echo "$line" | sed -n 's/.*mode=\([^ ]*\).*/\1/p')
      size_v=$(echo "$line" | sed -n 's/.*size=\([0-9]*\).*/\1/p')
      avg=$(echo "$line" | sed -n 's/.*avg=\([0-9.]*\).*/\1/p')
      minv=$(echo "$line" | sed -n 's/.*min=\([0-9.]*\).*/\1/p')
      maxv=$(echo "$line" | sed -n 's/.*max=\([0-9.]*\).*/\1/p')
      append_csv "$CSV" "$mode_v,$size_v,$avg,$minv,$maxv,,"
    fi
  done
done

log "wrote $CSV"
python3 "$SCRIPT_DIR/plot_paper_figs.py" --fig 2 --results-dir "$RESULTS_DIR"
