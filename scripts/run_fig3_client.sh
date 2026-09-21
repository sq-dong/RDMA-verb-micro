#!/usr/bin/env bash
# Fig.3 inbound WRITE-UC size sweep (client side). Server: ./fig3_inbound -s ...
set -euo pipefail
SIP=${1:?server rdma ip}
DEV=${2:?local device}
GID=${3:-3}
for s in 2 8 16 32 64 128 256; do
  echo "=== inbound WRITE-UC size=$s ==="
  ./fig3_inbound -c -d "$DEV" -a "$SIP" -p 18510 -x "$GID" -l "$s" -t 64 -Q 64 --uc -D 3
  sleep 1
done
