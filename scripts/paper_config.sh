# paper_config.sh — Fig.2–6 shared measurement policy.
#
# Inline: PAPER_INLINE_MAX is the HW ceiling (CX-5 ≈828). Throughput paths
# must NOT create QPs with that grant — use vt_inline_grant(size) so WQEs
# stay small (libhrd HRD_MAX_INLINE=60). See common.h.
#
# Per-figure inline ON/OFF (same roles as the paper):
#   Fig.2  WRITE/READ: no inline; WR-INLINE + ECHO: inline for that size
#   Fig.3  WRITE/READ: no inline
#   Fig.4  WR-UC-INLINE + SEND-UD: inline; WRITE-UC + READ: no inline
#   Fig.5  only +inlined bars
#   Fig.6  32 B, all inlined
#
# shellcheck shell=bash

# ---- HW ceiling (create-time grant is per-size via vt_inline_grant) ----
PAPER_INLINE_MAX="${PAPER_INLINE_MAX:-828}"
PAPER_INLINE_MAX_UD="${PAPER_INLINE_MAX_UD:-956}"

# ---- OLD (paper CX-3 256 B regime) — kept for reference, do not delete ----
# PAPER_INLINE_MAX="${PAPER_INLINE_MAX:-256}"
# PAPER_INLINE_MAX_UD="${PAPER_INLINE_MAX_UD:-256}"

PAPER_POSTLIST="${PAPER_POSTLIST:-64}"
# Fig.2–4 / Fig.6: selective signaling must stay < VT_SQ_DEPTH (128).
# unsig=256 with SQ=128 → ENOSPC before the first reclaim poll.
PAPER_UNSIG="${PAPER_UNSIG:-64}"
# Fig.5: ww/ws always signal the last WR of each window/postlist, so -Q only
# needs to be >1 for "+unsignalled"; keep ≤ SQ/2. (ws-echo uses 256 with
# HRD_Q_DEPTH=1024 — raise VT_SQ_DEPTH if matching that exactly.)
PAPER_UNSIG_ECHO="${PAPER_UNSIG_ECHO:-64}"
# Fig.6 with postlist: unsig >= postlist (one signal per doorbell batch).
PAPER_UNSIG_SCALE="${PAPER_UNSIG_SCALE:-64}"

PAPER_MSG_SIZE="${PAPER_MSG_SIZE:-32}"
PAPER_ECHO_WINDOW="${PAPER_ECHO_WINDOW:-64}"

PAPER_TPUT_SEC="${PAPER_TPUT_SEC:-5}"
PAPER_PASSIVE_SEC="${PAPER_PASSIVE_SEC:-40}"
# Fig.5: median of N independent trials (reduces short-run noise).
PAPER_FIG5_TRIALS="${PAPER_FIG5_TRIALS:-3}"

# Non-inline / READ / WRITE: into BW-limited region on 100G CX-5.
# shellcheck disable=SC2034
PAPER_SIZES_FULL=(4 8 16 32 64 128 256 512 1024 2048 4096)
# denser (kept):
# PAPER_SIZES_FULL=(4 8 16 32 64 128 256 512 828 1024 2048 4096)
# OLD paper axis:
# PAPER_SIZES_FULL=(4 8 16 32 64 128 256 512 1024)

# Inline-only curves (Fig.2 WR-INLINE / ECHO): stop at RC/UC HW max.
# shellcheck disable=SC2034
PAPER_SIZES_INLINE=(4 8 16 32 64 128 256 512 828)
# denser (kept):
# PAPER_SIZES_INLINE=(4 8 16 32 48 64 96 128 192 256 320 384 512 640 768 828)
# OLD:
# PAPER_SIZES_INLINE=(4 8 16 32 64 128 256)

# Fig.4 outbound: paper emphasizes small-payload INLINE WRITE > READ.
# Cap at RC/UC HW inline ceiling (828); denser mid-range for PIO steps.
# shellcheck disable=SC2034
PAPER_SIZES_OUT=(4 8 16 24 32 48 64 96 128 160 192 224 256 320 384 512 640 768 828)
# previous (past inline cliff / BW region):
# PAPER_SIZES_OUT=(4 8 16 32 64 128 256 512 828 956 1024 2048 4096)
# OLD paper-ish:
# PAPER_SIZES_OUT=(4 8 16 32 48 64 96 128 160 192 224 256)

# Fig.6 x-axis = paper N (#client procs = #server procs).
# Active QPs at RNICS = N*N (all-to-all). Out-SEND-UD stays at 1 QP.
# shellcheck disable=SC2034
PAPER_NQPS=(1 2 4 8 12 16)
# denser / CX-5 stress (QP count directly, not paper N):
# PAPER_NQPS=(1 2 4 8 16 32 64 128 256)
# OLD wrong (passed N as QP count without squaring):
# PAPER_NQPS=(1 2 4 8 16 32 64 128 256)

paper_assert_inline_sync() {
  local hdr="${BENCH_DIR:-.}/common.h"
  local v
  v=$(grep -E '^#define VT_MAX_INLINE[[:space:]]+[0-9]+' "$hdr" | head -1 |
      sed -n 's/#define VT_MAX_INLINE[[:space:]]*\([0-9]*\).*/\1/p')
  if [[ -z "$v" ]]; then
    log "WARN: cannot read VT_MAX_INLINE from $hdr"
    return 0
  fi
  if [[ "$v" -ne "$PAPER_INLINE_MAX" ]]; then
    log "ERROR: VT_MAX_INLINE=$v in common.h != PAPER_INLINE_MAX=$PAPER_INLINE_MAX"
    log "       Keep them equal to this NIC's RC/UC max_inline_data (≈828)."
    exit 1
  fi
}
