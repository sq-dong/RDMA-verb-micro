#!/usr/bin/env bash
# Collect Fig.3 inbound throughput -> results/fig3.csv
# Paper: many clients issue verbs to one server machine (inbound fan-in).
# If CLT_HOST2 is set (via setup_machine.sh), run one client process on each
# host in parallel and sum their Mops (symbolic multi-client).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

CSV="$RESULTS_DIR/fig3.csv"
rm -f "$CSV"
csv_header "$CSV" "curve,size,mops"

# Paper Fig.3 x-axis: 4 8 16 32 64 128 256 512 1024
SIZES=(4 8 16 32 64 128 256 512 1024)
# Paper Fig.3 WRITE is not inlined (inline is Fig.2 WR-INLINE and Fig.4 outbound).
declare -a JOBS=(
  "WRITE-UC|--uc --no-inline"
  "WRITE-RC|--rc --no-inline"
  "READ-RC|--read --no-inline"
)

mapfile -t CLIENTS < <(client_hosts)
log "fig3 clients: ${CLIENTS[*]}"

sync_bins
kill_bench "$SRV_HOST"
for h in "${CLIENTS[@]}"; do kill_bench "$h"; done
sleep 1

PORT_BASE=18510
idx=0
for job in "${JOBS[@]}"; do
  curve="${job%%|*}"
  flags="${job#*|}"
  for size in "${SIZES[@]}"; do
    log "fig3 $curve size=$size (n_clients=${#CLIENTS[@]})"

    declare -a srv_pids=()
    declare -a clt_logs=()
    ci=0
    for h in "${CLIENTS[@]}"; do
      port=$((PORT_BASE + idx + ci))
      srv_log="$RESULTS_DIR/fig3_srv_${curve}_${size}_c${ci}.log"
      clt_log="$RESULTS_DIR/fig3_clt_${curve}_${size}_c${ci}.log"
      clt_logs+=("$clt_log")

      srv_cmd="cd '$BENCH_DIR' && ./fig3_inbound -s -d $SRV_DEV -a $SRV_IP -p $port -x $SRV_GID -l $size $flags"
      pid=$(remote_bg "${SRV_HOST:-local}" "$srv_log" "$srv_cmd")
      srv_pids+=("$pid")
      ci=$((ci + 1))
    done
    sleep 1

    # Launch all clients (background), then wait.
    declare -a clt_pids=()
    ci=0
    for h in "${CLIENTS[@]}"; do
      port=$((PORT_BASE + idx + ci))
      cdev=$(client_dev_for "$h")
      cgid=$(client_gid_for "$h")
      clt_log="${clt_logs[$ci]}"
      clt_cmd="cd '$BENCH_DIR' && ./fig3_inbound -c -d $cdev -a $SRV_IP -p $port -x $cgid -l $size -t 64 -Q 64 -D 3 $flags"
      # shellcheck disable=SC2086
      if [[ -z "$h" || "$h" == "local" || "$h" == "localhost" ]]; then
        bash -lc "$clt_cmd" >"$clt_log" 2>&1 &
      else
        ssh $SSH_OPTS "$h" "$clt_cmd" >"$clt_log" 2>&1 &
      fi
      clt_pids+=($!)
      ci=$((ci + 1))
    done

    total=0
    ok=1
    for pid in "${clt_pids[@]}"; do
      if ! wait "$pid"; then ok=0; fi
    done
    for pid in "${srv_pids[@]}"; do kill_pid "$pid"; done
    kill_bench "$SRV_HOST"
    sleep 0.5

    idx=$((idx + ${#CLIENTS[@]}))

    if [[ $ok -ne 1 ]]; then
      log "WARN fail $curve size=$size"
      continue
    fi

    sum=0
    for clt_log in "${clt_logs[@]}"; do
      line=$(grep -E '^fig3 inbound client:' "$clt_log" | tail -1 || true)
      mops=$(echo "$line" | sed -n 's/.*: \([0-9.]*\) Mops.*/\1/p')
      if [[ -z "$mops" ]]; then
        log "WARN parse $clt_log"
        ok=0
        break
      fi
      sum=$(python3 -c "print(round($sum + $mops, 4))")
    done
    [[ $ok -eq 1 ]] || continue
    append_csv "$CSV" "$curve,$size,$sum"
    log "fig3 $curve size=$size total=${sum} Mops"
  done
done

log "wrote $CSV"
python3 "$SCRIPT_DIR/plot_paper_figs.py" --fig 3 --results-dir "$RESULTS_DIR"
cleanup_logs "$RESULTS_DIR"
