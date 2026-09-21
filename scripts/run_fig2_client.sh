#!/usr/bin/env bash
# Sweep Fig.2 sizes. Run server once in another terminal (echo mode needs own pair).
# Usage: ./scripts/run_fig2_client.sh <server_ip> <dev> [gid]
set -euo pipefail
SIP=${1:?server rdma ip}
DEV=${2:?local mlx device}
GID=${3:-3}
PORT=18500
for m in write write_inl read; do
  for s in 2 8 16 32 64 128 256; do
    echo "=== mode=$m size=$s ==="
    ./fig2_latency -c -d "$DEV" -a "$SIP" -p "$PORT" -x "$GID" -m "$m" -l "$s" -n 5000 -w 500 || true
    sleep 1
  done
done
echo "For echo: start server with -m echo, then:"
echo "  ./fig2_latency -c -d $DEV -a $SIP -p $((PORT+1)) -x $GID -m echo -l 64"
