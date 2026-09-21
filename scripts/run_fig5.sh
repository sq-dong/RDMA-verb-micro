#!/usr/bin/env bash
# Fig.5 ECHO modes. Start -s first on MS.
set -euo pipefail
SIP=${1:?server rdma ip}
DEV=${2:?local device}
ROLE=${3:?server|client}
GID=${4:-3}
flag_role="-c"
[[ "$ROLE" == "server" ]] && flag_role="-s"
for m in ww ws ss; do
  echo "=== echo $m ==="
  ./fig5_echo $flag_role -d "$DEV" -a "$SIP" -p 18530 -x "$GID" -m "$m" -l 32 -w 32 -Q 64 -D 3
  sleep 1
done
