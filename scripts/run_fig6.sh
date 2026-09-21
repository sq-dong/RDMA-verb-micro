#!/usr/bin/env bash
# Fig.6 QP fanout sweep (Out-WRITE-UC). Server is requester.
set -euo pipefail
SIP=${1:?server rdma ip}
DEV=${2:?local device}
ROLE=${3:?server|client}
GID=${4:-3}
flag_role="-c"
[[ "$ROLE" == "server" ]] && flag_role="-s"
for q in 1 2 4 8 16 32; do
  echo "=== nqp=$q ==="
  ./fig6_scale $flag_role -d "$DEV" -a "$SIP" -p 18540 -x "$GID" -q "$q" -l 32 -Q 4 -D 3
  sleep 1
done
