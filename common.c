/*
 * common.c — device open, QP state machine, TCP endpoint exchange, CQ poll.
 *
 * Pattern reference: rdma_bench/libhrd/hrd_conn.c
 *   - hrd_ctrl_blk_init / create RC|UC|UD QPs
 *   - modify QP INIT -> RTR -> RTS
 * Registry replaced: TCP instead of memcached publish/get.
 */

#include "common.h"

#include <netdb.h>

/*
 * Original helper (unused — logic inlined in vt_open_device). Kept fully commented:
 *
 * static struct ibv_device *find_dev(const char *name) {
 *   int n = 0;
 *   struct ibv_device **list = ibv_get_device_list(&n);
 *   VT_CHECK(list && n > 0, "ibv_get_device_list");
 *   struct ibv_device *dev = NULL;
 *   for (int i = 0; i < n; i++) {
 *     if (!name || strcmp(ibv_get_device_name(list[i]), name) == 0) {
 *       dev = list[i];
 *       break;
 *     }
 *   }
 *   VT_CHECK(dev, "RDMA device not found");
 *   // Caller must open the device before freeing the list, or strdup the name.
 *   // ibv_free_device_list(list);
 *   return dev;
 * }
 */

void vt_open_device(struct vt_ctx *v, const char *dev_name, int port,
                   int gid_index) {
  memset(v, 0, sizeof(*v));
  v->port = port;
  v->gid_index = gid_index;

  int n = 0;
  struct ibv_device **list = ibv_get_device_list(&n);
  VT_CHECK(list && n > 0, "no RDMA devices");
  struct ibv_device *dev = NULL;
  for (int i = 0; i < n; i++) {
    if (!dev_name || strcmp(ibv_get_device_name(list[i]), dev_name) == 0) {
      dev = list[i];
      strncpy(v->dev_name, ibv_get_device_name(list[i]), sizeof(v->dev_name) - 1);
      break;
    }
  }
  VT_CHECK(dev, "device name not found");
  v->ctx = ibv_open_device(dev);
  ibv_free_device_list(list);
  VT_CHECK(v->ctx, "ibv_open_device");

  v->pd = ibv_alloc_pd(v->ctx);
  VT_CHECK(v->pd, "ibv_alloc_pd");
  v->cq = ibv_create_cq(v->ctx, VT_CQ_DEPTH, NULL, NULL, 0);
  VT_CHECK(v->cq, "ibv_create_cq");

  struct ibv_port_attr pattr;
  VT_CHECK(ibv_query_port(v->ctx, (uint8_t)port, &pattr) == 0, "query_port");
  v->lid = pattr.lid;
  v->is_roce = (pattr.link_layer == IBV_LINK_LAYER_ETHERNET);
  VT_CHECK(ibv_query_gid(v->ctx, (uint8_t)port, gid_index, &v->gid) == 0,
           "query_gid");

  printf("vt: device=%s port=%d lid=%u roce=%d gid_index=%d\n", v->dev_name,
         port, v->lid, v->is_roce, gid_index);
}

void vt_close_device(struct vt_ctx *v) {
  if (v->mr)
    ibv_dereg_mr(v->mr);
  if (v->buf)
    free(v->buf);
  if (v->cq)
    ibv_destroy_cq(v->cq);
  if (v->pd)
    ibv_dealloc_pd(v->pd);
  if (v->ctx)
    ibv_close_device(v->ctx);
  memset(v, 0, sizeof(*v));
}

void vt_alloc_buf(struct vt_ctx *v, size_t size, int access) {
  v->buf_size = size;
  VT_CHECK(posix_memalign((void **)&v->buf, 4096, size) == 0, "memalign");
  memset(v->buf, 0, size);
  v->mr = ibv_reg_mr(v->pd, v->buf, size, access);
  VT_CHECK(v->mr, "ibv_reg_mr");
}

struct ibv_qp *vt_create_qp(struct vt_ctx *v, enum ibv_qp_type type,
                            int max_inline) {
  struct ibv_qp_init_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.send_cq = v->cq;
  attr.recv_cq = v->cq;
  attr.qp_type = type;
  attr.cap.max_send_wr = VT_SQ_DEPTH;
  attr.cap.max_recv_wr = VT_RQ_DEPTH;
  attr.cap.max_send_sge = 1;
  attr.cap.max_recv_sge = 1;
  /* Clamp to type max: RC/UC fail above ~828; UD can take ~956 on mlx5_0. */
  if (type == IBV_QPT_UD) {
    if (max_inline > VT_MAX_INLINE_UD)
      max_inline = VT_MAX_INLINE_UD;
  } else if (max_inline > VT_MAX_INLINE) {
    max_inline = VT_MAX_INLINE;
  }
  attr.cap.max_inline_data = (uint32_t)max_inline;
  struct ibv_qp *qp = ibv_create_qp(v->pd, &attr);
  VT_CHECK(qp, "ibv_create_qp");
  return qp;
}

void vt_fill_local_ep(struct vt_ctx *v, struct ibv_qp *qp, uint32_t psn,
                      struct vt_endpoint *ep) {
  memset(ep, 0, sizeof(*ep));
  ep->qpn = qp->qp_num;
  ep->psn = psn;
  ep->rkey = v->mr->rkey;
  ep->addr = (uint64_t)(uintptr_t)v->buf;
  ep->lid = v->lid;
  memcpy(ep->gid, v->gid.raw, 16);
  ep->gid_index = (uint8_t)v->gid_index;
  ep->is_roce = (uint8_t)v->is_roce;
}

void vt_qp_to_init(struct ibv_qp *qp, int port) {
  struct ibv_qp_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_INIT;
  attr.pkey_index = 0;
  attr.port_num = (uint8_t)port;
  attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                         IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
  int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
  if (qp->qp_type == IBV_QPT_UD) {
    /* UD uses qkey instead of access flags. */
    attr.qkey = 0x11111111;
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY;
  }
  VT_CHECK(ibv_modify_qp(qp, &attr, flags) == 0, "QP -> INIT");
}

void vt_qp_to_rtr(struct vt_ctx *v, struct ibv_qp *qp,
                  const struct vt_endpoint *remote, enum ibv_qp_type type) {
  if (type == IBV_QPT_UD) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    VT_CHECK(ibv_modify_qp(qp, &attr, IBV_QP_STATE) == 0, "UD QP -> RTR");
    return;
  }

  struct ibv_qp_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = IBV_MTU_1024;
  attr.dest_qp_num = remote->qpn;
  attr.rq_psn = remote->psn;
  attr.max_dest_rd_atomic = (type == IBV_QPT_RC) ? 16 : 0;
  attr.min_rnr_timer = 12;

  attr.ah_attr.port_num = (uint8_t)v->port;
  attr.ah_attr.sl = 0;
  attr.ah_attr.src_path_bits = 0;
  if (v->is_roce || remote->is_roce) {
    attr.ah_attr.is_global = 1;
    attr.ah_attr.dlid = 0;
    memcpy(attr.ah_attr.grh.dgid.raw, remote->gid, 16);
    attr.ah_attr.grh.sgid_index = (uint8_t)v->gid_index;
    attr.ah_attr.grh.hop_limit = 1;
  } else {
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = (uint16_t)remote->lid;
  }

  int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
              IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
  if (type == IBV_QPT_UC) {
    /* UC has no RD atomic / RNR fields. */
    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN;
  }
  VT_CHECK(ibv_modify_qp(qp, &attr, flags) == 0, "QP -> RTR");
}

void vt_qp_to_rts(struct ibv_qp *qp, uint32_t local_psn) {
  struct ibv_qp_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTS;
  attr.sq_psn = local_psn;

  if (qp->qp_type == IBV_QPT_UD) {
    VT_CHECK(ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN) == 0,
             "UD QP -> RTS");
    return;
  }

  attr.timeout = 14;
  attr.retry_cnt = 7;
  attr.rnr_retry = 7;
  attr.max_rd_atomic = (qp->qp_type == IBV_QPT_RC) ? 16 : 0;

  int flags = IBV_QP_STATE | IBV_QP_SQ_PSN;
  if (qp->qp_type == IBV_QPT_RC) {
    flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
             IBV_QP_MAX_QP_RD_ATOMIC;
  }
  /* UC: only STATE + SQ_PSN (no timeout/retry). */
  VT_CHECK(ibv_modify_qp(qp, &attr, flags) == 0, "QP -> RTS");
}

void vt_create_ud_ah(struct vt_ctx *v, struct vt_qp *q) {
  struct ibv_ah_attr ah_attr;
  memset(&ah_attr, 0, sizeof(ah_attr));
  ah_attr.port_num = (uint8_t)v->port;
  ah_attr.sl = 0;
  ah_attr.src_path_bits = 0;
  if (v->is_roce || q->remote.is_roce) {
    ah_attr.is_global = 1;
    ah_attr.dlid = 0;
    memcpy(ah_attr.grh.dgid.raw, q->remote.gid, 16);
    ah_attr.grh.sgid_index = (uint8_t)v->gid_index;
    ah_attr.grh.hop_limit = 1;
  } else {
    ah_attr.is_global = 0;
    ah_attr.dlid = (uint16_t)q->remote.lid;
  }
  q->ah = ibv_create_ah(v->pd, &ah_attr);
  VT_CHECK(q->ah, "ibv_create_ah");
}

int vt_tcp_listen(const char *ip, uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  VT_CHECK(fd >= 0, "socket");
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  VT_CHECK(inet_pton(AF_INET, ip, &addr.sin_addr) == 1, "inet_pton");
  VT_CHECK(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0, "bind");
  VT_CHECK(listen(fd, 8) == 0, "listen");
  int cfd = accept(fd, NULL, NULL);
  VT_CHECK(cfd >= 0, "accept");
  close(fd);
  return cfd;
}

int vt_tcp_connect(const char *ip, uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  VT_CHECK(fd >= 0, "socket");
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  VT_CHECK(inet_pton(AF_INET, ip, &addr.sin_addr) == 1, "inet_pton");
  for (int i = 0; i < 60; i++) {
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
      return fd;
    sleep(1);
  }
  VT_DIE("connect timeout");
  return -1;
}

void vt_tcp_exchange(int fd, const struct vt_endpoint *local,
                     struct vt_endpoint *remote) {
  VT_CHECK(write(fd, local, sizeof(*local)) == (ssize_t)sizeof(*local),
           "tcp write ep");
  VT_CHECK(read(fd, remote, sizeof(*remote)) == (ssize_t)sizeof(*remote),
           "tcp read ep");
}

void vt_poll_cq(struct ibv_cq *cq, int n) {
  struct ibv_wc wc;
  int got = 0;
  while (got < n) {
    int r = ibv_poll_cq(cq, 1, &wc);
    if (r < 0)
      VT_DIE("poll_cq");
    if (r == 0)
      continue;
    if (wc.status != IBV_WC_SUCCESS) {
      fprintf(stderr, "CQE status %s\n", ibv_wc_status_str(wc.status));
      exit(1);
    }
    got++;
  }
}

int vt_poll_cq_one(struct ibv_cq *cq, struct ibv_wc *wc, int timeout_ms) {
  uint64_t t0 = vt_ns();
  while (1) {
    int r = ibv_poll_cq(cq, 1, wc);
    if (r < 0)
      return -1;
    if (r == 1) {
      if (wc->status != IBV_WC_SUCCESS) {
        fprintf(stderr, "CQE status %s\n", ibv_wc_status_str(wc->status));
        return -1;
      }
      return 0;
    }
    if ((vt_ns() - t0) / 1000000ull > (uint64_t)timeout_ms)
      return -1;
  }
}
