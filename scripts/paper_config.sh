# paper_config.sh — Fig.2–6 shared measurement policy.
#
# CURRENT MODE: HW-native inline on this ConnectX-5 (mlx5_0), measured via
# ibv_create_qp binary search:
#   RC/UC max_inline_data ≈ 828 B
#   UD     max_inline_data ≈ 956 B
# Soft ceiling for RC/UC paths = PAPER_INLINE_MAX (= VT_MAX_INLINE in common.h).
# UD paths may use PAPER_INLINE_MAX_UD.
#
# Axis: extend past the paper's 1024/256 so CX-5 shows the message-rate → BW
# cliff; inline curves still stop at the real HW inline grant.
#
# Per-figure inline ON/OFF (same roles as the paper, cliff at HW max):
#   Fig.2  WRITE/READ: no inline; WR-INLINE + ECHO: inline ≤ RC max
#   Fig.3  WRITE/READ: no inline
#   Fig.4  WR-UC-INLINE + SEND-UD: inline; WRITE-UC + READ: no inline
#   Fig.5  only +inlined bars
#   Fig.6  32 B, all inlined
#
# shellcheck shell=bash

# ---- CURRENT (HW-native CX-5) ----
PAPER_INLINE_MAX="${PAPER_INLINE_MAX:-828}"
PAPER_INLINE_MAX_UD="${PAPER_INLINE_MAX_UD:-956}"

# ---- OLD (paper CX-3 256 B regime) — kept for reference, do not delete ----
# PAPER_INLINE_MAX="${PAPER_INLINE_MAX:-256}"
# PAPER_INLINE_MAX_UD="${PAPER_INLINE_MAX_UD:-256}"

PAPER_POSTLIST="${PAPER_POSTLIST:-64}"
PAPER_UNSIG="${PAPER_UNSIG:-64}"
PAPER_UNSIG_SCALE="${PAPER_UNSIG_SCALE:-4}"

PAPER_MSG_SIZE="${PAPER_MSG_SIZE:-32}"
PAPER_ECHO_WINDOW="${PAPER_ECHO_WINDOW:-64}"

PAPER_TPUT_SEC="${PAPER_TPUT_SEC:-3}"
PAPER_PASSIVE_SEC="${PAPER_PASSIVE_SEC:-30}"

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

# Fig.4 outbound sweep; inline curves filtered to ≤ PAPER_INLINE_MAX(_UD).
# shellcheck disable=SC2034
PAPER_SIZES_OUT=(4 8 16 32 64 128 256 512 828 956 1024 2048 4096)
# denser (kept):
# PAPER_SIZES_OUT=(4 8 16 32 48 64 96 128 192 256 320 384 512 640 768 828 956 1024 2048 4096)
# OLD:
# PAPER_SIZES_OUT=(4 8 16 32 48 64 96 128 160 192 224 256)
# even older paper-ish:
# PAPER_SIZES_OUT=(4 8 16 32 64 128 192 256)

# Fig.6 x-axis = QPs *per process* (paper N).  CX-3 thrashed by ~16;
# CX-5 QP cache is much larger — extend until Out-WRITE drops.
# shellcheck disable=SC2034
PAPER_NQPS=(1 2 4 8 16 32 64 128 256)
# previous (wrong N² attempt / dense low range):
# PAPER_NQPS=(1 2 4 6 8 10 12 14 16 18 20 22 24)
# OLD paper ticks only:
# PAPER_NQPS=(4 8 12 16)

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
