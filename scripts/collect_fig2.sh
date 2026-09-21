#!/usr/bin/env bash
# Collect Fig.2 latency curves -> results/fig2.csv then plot.
# Paper Fig.2 x-axis ticks: 4 8 16 32 64 128 256 512 1024
#   WRITE / READ: 4..1024
#   WR-INLINE / ECHO: 4..256 (inline limit in the paper)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

CSV="$RESULTS_DIR/fig2.csv"
rm -f "$CSV"
csv_header "$CSV" "mode,size,avg_us,min_us,max_us,rtt_us,half_rtt_us"

# Paper Fig.2 x-axis ticks: 4 8 16 32 64 128 256 512 1024
#   WRITE / WR-INLINE / ECHO: 4..256  (WRITE curve in paper stops with inline range;
#                                      WR-INLINE & ECHO cannot exceed inline max)
#   READ: 4..1024
SIZES_WRITE=(4 8 16 32 64 128 256 512 1024)
SIZES_READ=(4 8 16 32 64 128 256 512 1024)
SIZES_INLINE=(4 8 16 32 64 128 256)

run_one() {
  local mode=$1
  local size=$2
  local port=$3

  log "fig2 mode=$mode size=$size port=$port"

  local srv_log="$RESULTS_DIR/fig2_srv_${mode}_${size}.log"
  local clt_log="$RESULTS_DIR/fig2_clt_${mode}_${size}.log"

  local srv_cmd="cd '$BENCH_DIR' && ./fig2_latency -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -m $mode -l $size -n 5000 -w 500"
  local pid
  pid=$(remote_bg "${SRV_HOST:-local}" "$srv_log" "$srv_cmd")
  sleep 1

  # Hard timeout around the binary so a stuck size cannot freeze collect.
  # (Echo also retries inside the binary on UC drops.)
  local clt_timeout=120
  local clt_cmd="cd '$BENCH_DIR' && timeout ${clt_timeout}s ./fig2_latency -c -d $CLT_DEV -a $SRV_IP -p $port -x $CLT_GID -m $mode -l $size -n 5000 -w 500"
  set +e
  remote "$CLT_HOST" "$clt_cmd" | tee "$clt_log"
  local rc=${PIPESTATUS[0]}
  set -e

  kill_pid "$pid"
  kill_bench "$SRV_HOST"
  kill_bench "$CLT_HOST"
  sleep 0.5

  if [[ $rc -eq 124 ]]; then
    log "WARN: client timed out (${clt_timeout}s) for mode=$mode size=$size"
    return 0
  fi

  if [[ $rc -ne 0 ]]; then
    log "WARN: client failed for mode=$mode size=$size (see $clt_log)"
    return 0
  fi

  local line
  line=$(grep -E '^fig2 mode=' "$clt_log" | tail -1 || true)
  if [[ -z "$line" ]]; then
    log "WARN: no result line in $clt_log"
    return 0
  fi

  if [[ "$mode" == "echo" ]]; then
    local size_v rtt half minv maxv
    size_v=$(echo "$line" | sed -n 's/.*size=\([0-9]*\).*/\1/p')
    rtt=$(echo "$line" | sed -n 's/.*rtt_avg=\([0-9.]*\).*/\1/p')
    half=$(echo "$line" | sed -n 's/.*rtt\/2=\([0-9.]*\).*/\1/p')
    minv=$(echo "$line" | sed -n 's/.*min_rtt=\([0-9.]*\).*/\1/p')
    maxv=$(echo "$line" | sed -n 's/.*max_rtt=\([0-9.]*\).*/\1/p')
    append_csv "$CSV" "echo,$size_v,$half,$minv,$maxv,$rtt,$half"
    append_csv "$CSV" "echo_rtt,$size_v,$rtt,$minv,$maxv,$rtt,"
  else
    local mode_v size_v avg minv maxv
    mode_v=$(echo "$line" | sed -n 's/.*mode=\([^ ]*\).*/\1/p')
    size_v=$(echo "$line" | sed -n 's/.*size=\([0-9]*\).*/\1/p')
    avg=$(echo "$line" | sed -n 's/.*avg=\([0-9.]*\).*/\1/p')
    minv=$(echo "$line" | sed -n 's/.*min=\([0-9.]*\).*/\1/p')
    maxv=$(echo "$line" | sed -n 's/.*max=\([0-9.]*\).*/\1/p')
    append_csv "$CSV" "$mode_v,$size_v,$avg,$minv,$maxv,,"
  fi
}

sync_bins
kill_bench "$SRV_HOST"
kill_bench "$CLT_HOST"
sleep 1

PORT_BASE=18500
idx=0

for size in "${SIZES_WRITE[@]}"; do
  run_one write "$size" $((PORT_BASE + idx)); idx=$((idx + 1))
done
for size in "${SIZES_INLINE[@]}"; do
  run_one write_inl "$size" $((PORT_BASE + idx)); idx=$((idx + 1))
done
for size in "${SIZES_READ[@]}"; do
  run_one read "$size" $((PORT_BASE + idx)); idx=$((idx + 1))
done
for size in "${SIZES_INLINE[@]}"; do
  run_one echo "$size" $((PORT_BASE + idx)); idx=$((idx + 1))
done

log "wrote $CSV"
python3 "$SCRIPT_DIR/plot_paper_figs.py" --fig 2 --results-dir "$RESULTS_DIR"
cleanup_logs "$RESULTS_DIR"
