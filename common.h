/*
 * common.h — shared helpers for HERD Sec.3 (Fig.2–6) microbenchmarks.
 *
 * Inspired by rdma_bench/libhrd (hrd_conn.c, hrd.h) but without memcached:
 * we exchange QP metadata over a plain TCP socket instead of hrd_publish_*.
 *
 * Original libhrd-style registry path (kept for reference, not used here):
 *
 *   char srv_name[HRD_QP_NAME_SIZE];
 *   sprintf(srv_name, "server-%d", srv_gid);
 *   char clt_name[HRD_QP_NAME_SIZE];
 *   sprintf(clt_name, "client-%d", clt_gid);
 *
 *   hrd_publish_conn_qp(cb, 0, srv_name);
 *   printf("main: Server %s published. Waiting for client %s\n", srv_name,
 *          clt_name);
 *
 *   struct hrd_qp_attr *clt_qp = NULL;
 *   while (clt_qp == NULL) {
 *     clt_qp = hrd_get_published_qp(clt_name);
 *     if (clt_qp == NULL) {
 *       usleep(200000);
 *     }
 *   }
 *
 *   printf("main: Server %s found client! Connecting..\n", srv_name);
 *   hrd_connect_qp(cb, 0, clt_qp);
 *   hrd_publish_ready(srv_name);
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

#define VT_MAX_INLINE 256
#define VT_BUF_SIZE (2 * 1024 * 1024)
#define VT_SQ_DEPTH 128
#define VT_RQ_DEPTH 128
#define VT_CQ_DEPTH 256
#define VT_MAX_QPS 64
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

struct ibv_qp *vt_create_qp(struct vt_ctx *v, enum ibv_qp_type type,
                            int max_inline);
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
