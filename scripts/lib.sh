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
SRV_DEV="${SRV_DEV:-mlx5_0}"
CLT_DEV="${CLT_DEV:-mlx5_3}"
SRV_IP="${SRV_IP:-10.0.0.20}"
SRV_GID="${SRV_GID:-4}"
CLT_GID="${CLT_GID:-3}"
SSH_OPTS="${SSH_OPTS:--o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no}"

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
kill_bench() {
  local host=$1
  remote "$host" "pkill -f 'fig2_latency|fig3_inbound|fig4_outbound|fig5_echo|fig6_scale' || true"
}

sync_bins() {
  # Ensure binaries exist locally; copy to client if remote.
  make -C "$BENCH_DIR" -j"$(nproc)" >/dev/null
  if [[ -n "$CLT_HOST" && "$CLT_HOST" != "local" && "$CLT_HOST" != "localhost" ]]; then
    # shellcheck disable=SC2086
    ssh $SSH_OPTS "$CLT_HOST" "mkdir -p '$BENCH_DIR'"
    # shellcheck disable=SC2086
    scp $SSH_OPTS -q \
      "$BENCH_DIR"/fig2_latency \
      "$BENCH_DIR"/fig3_inbound \
      "$BENCH_DIR"/fig4_outbound \
      "$BENCH_DIR"/fig5_echo \
      "$BENCH_DIR"/fig6_scale \
      "$CLT_HOST:$BENCH_DIR/"
  fi
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
