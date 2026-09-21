#!/usr/bin/env bash
# setup_machine.sh — source this before collect_fig*.sh
#
# Usage (on the server / MS machine, typically smartx-server02):
#   source ./scripts/setup_machine.sh
#
# Edit the values below if your RDMA IPs / GID indices change.
# Check with: show_gids | awk 'NR==1 || /10\.0\.0\./'

# Path to this repo on every machine (NFS or same absolute path after scp)
export BENCH_DIR="${BENCH_DIR:-$HOME/rdma_verb_test}"
export RESULTS_DIR="${RESULTS_DIR:-$BENCH_DIR/results}"

# ---- Server (paper MS) : smartx-server02 ----
# Empty / local => binaries run in this shell, not over SSH.
export SRV_HOST="${SRV_HOST:-local}"
export SRV_DEV="${SRV_DEV:-mlx5_0}"
export SRV_IP="${SRV_IP:-10.0.0.20}"
export SRV_GID="${SRV_GID:-4}"   # RoCEv2 GID index for 10.0.0.20

# ---- Client 1 : server03 ----
export CLT_HOST="${CLT_HOST:-server03}"
export CLT_DEV="${CLT_DEV:-mlx5_3}"
export CLT_IP="${CLT_IP:-10.0.0.21}"
export CLT_GID="${CLT_GID:-3}"   # RoCEv2 GID for client1 RDMA IP — verify with show_gids

# ---- Client 2 (optional, for Fig.3 / Fig.4 / Fig.6 multi-client) : thoth ----
# Leave CLT_HOST2 empty to use a single client only.
export CLT_HOST2="${CLT_HOST2:-thoth}"
export CLT_DEV2="${CLT_DEV2:-mlx5_1}"
export CLT_IP2="${CLT_IP2:-10.0.0.22}"
export CLT_GID2="${CLT_GID2:-3}" # verify on thoth after assigning 10.0.0.22

export SSH_OPTS="${SSH_OPTS:--o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no}"

echo "setup_machine.sh loaded:"
echo "  BENCH_DIR=$BENCH_DIR"
echo "  SRV: host=$SRV_HOST dev=$SRV_DEV ip=$SRV_IP gid=$SRV_GID"
echo "  CLT1: host=$CLT_HOST dev=$CLT_DEV ip=$CLT_IP gid=$CLT_GID"
if [[ -n "${CLT_HOST2:-}" ]]; then
  echo "  CLT2: host=$CLT_HOST2 dev=$CLT_DEV2 ip=$CLT_IP2 gid=$CLT_GID2"
else
  echo "  CLT2: (disabled)"
fi
echo
echo "Which figures want multiple clients (paper Sec.3):"
echo "  Fig.2 latency     — 1 client enough"
echo "  Fig.3 inbound     — many clients hit one server (use CLT1+CLT2)"
echo "  Fig.4 outbound    — one server proc per client (use CLT1+CLT2)"
echo "  Fig.5 ECHO        — 1 client enough"
echo "  Fig.6 QP scaling  — N client procs (spread over CLT1+CLT2)"
echo
echo "Next: ./scripts/collect_fig2.sh   # etc."
