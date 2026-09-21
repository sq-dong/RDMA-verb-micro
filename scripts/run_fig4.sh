#!/usr/bin/env bash
# Fig.4 outbound modes. Requester must pass -R on BOTH sides when MS is TCP server.
set -euo pipefail
SIP=${1:?server rdma ip}
DEV=${2:?local device}
ROLE=${3:?server|client}
GID=${4:-3}
flag_role="-c"
[[ "$ROLE" == "server" ]] && flag_role="-s"
for m in write_uc write_rc read send_ud; do
  echo "=== outbound $m ==="
  ./fig4_outbound $flag_role -R -d "$DEV" -a "$SIP" -p 18520 -x "$GID" -m "$m" -l 32 -t 64 -Q 64 -D 3
  sleep 1
done
