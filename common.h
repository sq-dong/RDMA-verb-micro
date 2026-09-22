/*
 * common.h — shared helpers for HERD Sec.3 (Fig.2–6) microbenchmarks.
 *
 * Inspired by rdma_bench/libhrd (hrd_conn.c, hrd.h) but without memcached:
 * we exchange QP metadata over a plain TCP socket instead of hrd_publish_*.
 */

#ifndef RDMA_VERB_TEST_COMMON_H
#define RDMA_VERB_TEST_COMMON_H

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define VT_DIE(msg)                                                            \
  do {                                                                         \
    fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, (msg),              \
            strerror(errno));                                                  \
    exit(1);                                                                   \
  } while (0)

#define VT_CHECK(x, msg)                                                       \
  do {                                                                         \
    if (!(x))                                                                  \
      VT_DIE(msg);                                                             \
  } while (0)

/*
 * Inline / WQE sizing — CRITICAL for message rate.
 *
 * rdma_bench/libhrd/hrd.h:
 *   "Small max_inline_data reduces the QP's max WQE size, which reduces the
 *    DMA size in doorbell method of WQE fetch."
 *   #define HRD_MAX_INLINE 60
 *
 * Creating every QP with the NIC's absolute max (~828B on CX-5) makes WQEs
 * enormous, shrinks effective SQ depth, and tanks outbound/ECHO Mops.
 *
 * Policy:
 *   VT_INLINE_WQE   — default create-time grant (libhrd-style), keep WQEs small
 *   VT_MAX_INLINE   — RC/UC hardware ceiling (for Fig.2 size sweeps only)
 *   VT_MAX_INLINE_UD — UD hardware ceiling
 *
 * Throughput binaries (fig3–6) create with vt_inline_grant(size, ud):
 *   no-inline runs → VT_INLINE_WQE
 *   inline runs    → clamp(size, VT_INLINE_WQE .. HW max)
 */
#define VT_INLINE_WQE 60
#define VT_MAX_INLINE 828
#define VT_MAX_INLINE_UD 956

#define VT_BUF_SIZE (8 * 1024 * 1024)
#define VT_SQ_DEPTH 128
#define VT_RQ_DEPTH 512
#define VT_CQ_DEPTH 1024
#define VT_MAX_QPS 512
#define VT_CACHELINE 64

/* Endpoint advertised over TCP (mirrors fields in hrd_qp_attr). */
struct vt_endpoint {
  uint32_t qpn;
  uint32_t psn;
  uint32_t rkey;
  uint32_t lid; /* 0 on pure RoCE */
  uint64_t addr;
  uint8_t gid[16];
  uint8_t gid_index;
  uint8_t is_roce; /* 1 if link layer is Ethernet */
  uint8_t pad[2];
};

struct vt_ctx {
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_mr *mr;
  uint8_t *buf;
  size_t buf_size;
  int port; /* 1-based IB/RoCE port */
  int gid_index;
  int is_roce;
  uint16_t lid;
  enum ibv_mtu active_mtu;
  union ibv_gid gid;
  char dev_name[64];
};

struct vt_qp {
  struct ibv_qp *qp;
  enum ibv_qp_type type;
  uint32_t local_psn;
  struct vt_endpoint local;
  struct vt_endpoint remote;
  struct ibv_ah *ah; /* UD only */
};

void vt_open_device(struct vt_ctx *v, const char *dev_name, int port,
                   int gid_index);
void vt_close_device(struct vt_ctx *v);
void vt_alloc_buf(struct vt_ctx *v, size_t size, int access);

/*
 * Create QP. max_inline is the *requested* create-time grant.
 * Prefer vt_inline_grant() rather than VT_MAX_INLINE.
 * On success, *actual_inline / *actual_sq (if non-NULL) report driver caps.
 */
struct ibv_qp *vt_create_qp(struct vt_ctx *v, enum ibv_qp_type type,
                            int max_inline, int *actual_inline, int *actual_sq);

/* Inline grant for this run: keep WQE small unless this size needs more. */
static inline int vt_inline_grant(int msg_size, int use_inline, int is_ud) {
  int hw = is_ud ? VT_MAX_INLINE_UD : VT_MAX_INLINE;
  if (!use_inline || msg_size <= 0)
    return VT_INLINE_WQE;
  int need = msg_size;
  if (need < VT_INLINE_WQE)
    need = VT_INLINE_WQE;
  if (need > hw)
    need = hw;
  return need;
}

void vt_fill_local_ep(struct vt_ctx *v, struct ibv_qp *qp, uint32_t psn,
                      struct vt_endpoint *ep);
void vt_qp_to_init(struct ibv_qp *qp, int port);
void vt_qp_to_rtr(struct vt_ctx *v, struct ibv_qp *qp,
                  const struct vt_endpoint *remote, enum ibv_qp_type type);
void vt_qp_to_rts(struct ibv_qp *qp, uint32_t local_psn);
void vt_create_ud_ah(struct vt_ctx *v, struct vt_qp *q);

/* TCP bootstrap: server listens, client connects; both exchange endpoints. */
int vt_tcp_listen(const char *ip, uint16_t port);
int vt_tcp_connect(const char *ip, uint16_t port);
void vt_tcp_exchange(int fd, const struct vt_endpoint *local,
                     struct vt_endpoint *remote);

static inline uint64_t vt_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void vt_poll_cq(struct ibv_cq *cq, int n);
int vt_poll_cq_one(struct ibv_cq *cq, struct ibv_wc *wc, int timeout_ms);

/* Selective signaling helpers (same constraints as rdma_bench README). */
static inline int vt_should_signal(uint64_t nb_tx, int unsig_batch) {
  return (nb_tx % (uint64_t)unsig_batch) == 0;
}

#endif /* RDMA_VERB_TEST_COMMON_H */
