# paper_config.sh — HERD SIGCOMM'14 Sec.3 shared measurement policy for Fig.2–6.
#
# ConnectX-5 HW can grant max_inline_data ≫ 256, but the paper's Apt ConnectX-3
# limit is 256 B. All binaries gate IBV_SEND_INLINE with VT_MAX_INLINE (common.h),
# which MUST stay equal to PAPER_INLINE_MAX below. Do not raise it to the NIC max
# or Fig.2 WR-INLINE / Fig.4 WR-UC-INLINE / Fig.5–6 will leave the paper regime.
#
# Per-figure inline (matches paper text / captions — intentional differences):
#   Fig.2  write WRITE / READ: no inline; WR-INLINE + ECHO: inline, size ≤ 256
#   Fig.3  · inbound WRITE / READ: no inline (DMA WRITE)
#   Fig.4  · WR-UC-INLINE + SEND-UD: inline ≤ 256; WRITE-UC + READ: no inline
#   Fig.5  · +inlined bars: inline; basic/+unreliable/+unsignalled: --no-inline
#   Fig.6  · 32 B, inlined + unsignaled (paper caption)
#
# shellcheck shell=bash

# Soft inline ceiling (= paper CX-3 max; keep in sync with VT_MAX_INLINE in common.h).
PAPER_INLINE_MAX="${PAPER_INLINE_MAX:-256}"

# Shared selective-signaling / postlist (rdma_bench UNSIG_BATCH / postlist rule).
PAPER_POSTLIST="${PAPER_POSTLIST:-64}"
PAPER_UNSIG="${PAPER_UNSIG:-64}"
PAPER_UNSIG_SCALE="${PAPER_UNSIG_SCALE:-4}" # Fig.6 multi-QP (paper-style small batch)

# Fixed payload for Fig.5 bars and Fig.6 QP scaling.
PAPER_MSG_SIZE="${PAPER_MSG_SIZE:-32}"
PAPER_ECHO_WINDOW="${PAPER_ECHO_WINDOW:-32}"

# Duration (seconds) for throughput clients.
PAPER_TPUT_SEC="${PAPER_TPUT_SEC:-3}"
PAPER_PASSIVE_SEC="${PAPER_PASSIVE_SEC:-30}"

# Payload sweeps (paper axis ticks).
# shellcheck disable=SC2034
PAPER_SIZES_FULL=(4 8 16 32 64 128 256 512 1024)   # Fig.2 WRITE/READ, Fig.3
# shellcheck disable=SC2034
PAPER_SIZES_INLINE=(4 8 16 32 64 128 256)           # Fig.2 WR-INLINE / ECHO
# Fig.4 outbound: paper ticks 0/64/128/192/256; keep fine grid below 64 so the
# PIO / WQE-BB staircase (paper: ~64 B steps on CX-3) is visible on CX-5 too.
# shellcheck disable=SC2034
PAPER_SIZES_OUT=(4 8 16 32 48 64 96 128 160 192 224 256)
# Paper Fig.6 ticks are 0/4/8/12/16; denser sweep so the Out-WRITE drop shows clearly.
# shellcheck disable=SC2034
PAPER_NQPS=(1 2 4 6 8 10 12 14 16)

paper_assert_inline_sync() {
  local hdr="${BENCH_DIR:-.}/common.h"
  local v
  v=$(sed -n 's/#define VT_MAX_INLINE[[:space:]]*\([0-9]*\).*/\1/p' "$hdr" | head -1)
  if [[ -z "$v" ]]; then
    log "WARN: cannot read VT_MAX_INLINE from $hdr"
    return 0
  fi
  if [[ "$v" -ne "$PAPER_INLINE_MAX" ]]; then
    log "ERROR: VT_MAX_INLINE=$v in common.h != PAPER_INLINE_MAX=$PAPER_INLINE_MAX"
    log "       Keep them equal so Fig.2–6 share the paper's 256 B inline regime."
    exit 1
  fi
}
