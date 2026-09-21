#!/usr/bin/env bash
# Shared helpers for automated Fig.2–6 collection over SSH + local RDMA.
#
# Environment (override as needed):
#   SRV_HOST   SSH host for the paper "MS" machine (default: local shell)
#   CLT_HOST   SSH host for a client machine (default: server03)
#   SRV_DEV / CLT_DEV   mlx5 device names
#   SRV_IP              RDMA IPv4 of the server NIC (passed to -a)
#   SRV_GID / CLT_GID   RoCEv2 GID indices from show_gids
#   BENCH_DIR           path to rdma_verb_test on both machines
#   RESULTS_DIR         where CSV/PNG go (default: $BENCH_DIR/results)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH_DIR="${BENCH_DIR:-$ROOT}"
RESULTS_DIR="${RESULTS_DIR:-$BENCH_DIR/results}"
mkdir -p "$RESULTS_DIR"

SRV_HOST="${SRV_HOST:-}"
CLT_HOST="${CLT_HOST:-server03}"
CLT_HOST2="${CLT_HOST2:-}"
SRV_DEV="${SRV_DEV:-mlx5_0}"
CLT_DEV="${CLT_DEV:-mlx5_3}"
CLT_DEV2="${CLT_DEV2:-mlx5_1}"
SRV_IP="${SRV_IP:-10.0.0.20}"
SRV_GID="${SRV_GID:-4}"
CLT_GID="${CLT_GID:-3}"
CLT_GID2="${CLT_GID2:-3}"
SSH_OPTS="${SSH_OPTS:--o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no}"

# Clients used for multi-machine sweeps (CLT_HOST always; CLT_HOST2 if set).
client_hosts() {
  echo "$CLT_HOST"
  if [[ -n "${CLT_HOST2:-}" && "$CLT_HOST2" != "none" ]]; then
    echo "$CLT_HOST2"
  fi
}

client_dev_for() {
  local host=$1
  if [[ "$host" == "$CLT_HOST" ]]; then
    echo "$CLT_DEV"
  else
    echo "${CLT_DEV2:-$CLT_DEV}"
  fi
}

client_gid_for() {
  local host=$1
  if [[ "$host" == "$CLT_HOST" ]]; then
    echo "$CLT_GID"
  else
    echo "${CLT_GID2:-$CLT_GID}"
  fi
}

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

# Run a command on host (empty SRV_HOST / "local" => local bash).
remote() {
  local host=$1
  shift
  if [[ -z "$host" || "$host" == "local" || "$host" == "localhost" ]]; then
    bash -lc "$*"
  else
    # shellcheck disable=SC2086
    ssh $SSH_OPTS "$host" "$*"
  fi
}

remote_bg() {
  local host=$1
  local outfile=$2
  shift 2
  if [[ -z "$host" || "$host" == "local" || "$host" == "localhost" ]]; then
    bash -lc "$*" >"$outfile" 2>&1 &
    echo $!
  else
    # shellcheck disable=SC2086
    ssh $SSH_OPTS "$host" "$*" >"$outfile" 2>&1 &
    echo $!
  fi
}

kill_pid() {
  local pid=${1:-}
  [[ -n "$pid" ]] || return 0
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

# Best-effort: kill leftover binaries on a host.
# IMPORTANT: do NOT use `pkill -f 'fig2_latency|...'` via `bash -lc "..."`.
# That pattern appears in the bash command line itself, so pkill SIGTERMs the
# helper shell (exit 143) and `set -e` aborts the collector ("Terminated").
kill_bench() {
  local host=$1
  local cmd
  # -x: match executable name only (not collect_fig2.sh / ssh argv).
  cmd='pkill -x fig2_latency 2>/dev/null || true; '
  cmd+='pkill -x fig3_inbound 2>/dev/null || true; '
  cmd+='pkill -x fig4_outbound 2>/dev/null || true; '
  cmd+='pkill -x fig5_echo 2>/dev/null || true; '
  cmd+='pkill -x fig6_scale 2>/dev/null || true; true'
  remote "$host" "$cmd" || true
}

sync_bins() {
  # Ensure binaries exist locally; copy to every client.
  # Kill leftovers first — scp cannot overwrite a running ELF ("Text file busy").
  kill_bench "${SRV_HOST:-local}"
  local h
  for h in $(client_hosts); do
    kill_bench "$h"
  done
  sleep 0.5

  make -C "$BENCH_DIR" -j"$(nproc)" >/dev/null
  for h in $(client_hosts); do
    if [[ -z "$h" || "$h" == "local" || "$h" == "localhost" ]]; then
      continue
    fi
    # shellcheck disable=SC2086
    ssh $SSH_OPTS "$h" "mkdir -p '$BENCH_DIR'"
    # shellcheck disable=SC2086
    scp $SSH_OPTS -q \
      "$BENCH_DIR"/fig2_latency \
      "$BENCH_DIR"/fig3_inbound \
      "$BENCH_DIR"/fig4_outbound \
      "$BENCH_DIR"/fig5_echo \
      "$BENCH_DIR"/fig6_scale \
      "$h:$BENCH_DIR/"
  done
}

csv_header() {
  local file=$1
  local header=$2
  if [[ ! -f "$file" ]]; then
    echo "$header" >"$file"
  fi
}

append_csv() {
  local file=$1
  shift
  # shellcheck disable=SC2145
  echo "$*" >>"$file"
}

# Remove intermediate logs after plots are written (keep CSV, png, pdf).
cleanup_logs() {
  local dir=${1:-$RESULTS_DIR}
  rm -f "$dir"/*.log
  log "removed logs under $dir (csv/png/pdf kept)"
}
