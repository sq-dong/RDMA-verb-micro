/*
 * fig5_echo.c — HERD Sec.3 Fig.5: ECHO throughput (32B messages in the paper).
 *
 * Templates:
 *   ww  (WRITE/WRITE): rdma_bench/ww-echo/{client,server}.c
 *   ws  (WRITE/SEND):  rdma_bench/ws-echo/{client,worker}.c  — HERD choice
 *   ss  (SEND/SEND):   symmetric messaging (not a separate dir; patterned on
 *                      ws-echo SENDs both ways over UD)
 *
 * Optimizations (paper Fig.5 bars): always on by default here —
 *   UC/UD, selective signaling (-Q), inline (disable with --no-inline).
 * To approximate "basic" (leftmost bar), use --rc --no-inline -Q 1.
 *
 * Window rule from ww-echo/README (kept as comment):
 *
 *   // Server's window must be equal to client's window. The client sprays
 *   // W_c WRITEs into the server's buffer and the server replies with W_s
 *   // WRITEs. For this setup to measure ECHO performance, W_s = W_c is
 *   // required.
 *
 *   // The WRITE sizes used by servers and clients can be different. A server
 *   // only waits for conn_buf[0] to become non-zero as a signal of arrival of
 *   // W_c WRITEs from the client, which will happen regardless of client's
 *   // param.size.
 */

#include "common.h"

#include <getopt.h>

struct cfg {
  int is_server;
  char *dev;
  char *ip;
  uint16_t port;
  int gid_index;
  int size;
  int window;
  int unsig;
  int duration;
  int use_inline;
  int use_uc; /* for WRITE path; 0 => RC "basic" */
  char mode[8];
};

static void usage(const char *a) {
  fprintf(stderr,
          "Usage: %s -s|-c -d DEV -a IP [-p PORT] [-m ww|ws|ss] [-l SIZE] "
          "[-w WINDOW] [-Q UNSIG] [-D SEC] [--no-inline] [--rc]\n",
          a);
  exit(1);
}

static void parse(int argc, char **argv, struct cfg *c) {
  memset(c, 0, sizeof(*c));
  c->port = 18530;
  c->gid_index = 3;
  c->size = 32;
  c->window = 32;
  c->unsig = 64;
  c->duration = 5;
  c->use_inline = 1;
  c->use_uc = 1;
  c->is_server = -1;
  strcpy(c->mode, "ws");
  static struct option longopts[] = {{"no-inline", no_argument, 0, 1001},
                                     {"rc", no_argument, 0, 1002},
                                     {0, 0, 0, 0}};
  int opt;
  while ((opt = getopt_long(argc, argv, "scd:a:p:x:l:w:Q:D:m:h", longopts,
                            NULL)) != -1) {
    switch (opt) {
    case 's':
      c->is_server = 1;
      break;
    case 'c':
      c->is_server = 0;
      break;
    case 'd':
      c->dev = optarg;
      break;
    case 'a':
      c->ip = optarg;
      break;
    case 'p':
      c->port = (uint16_t)atoi(optarg);
      break;
    case 'x':
      c->gid_index = atoi(optarg);
      break;
    case 'l':
      c->size = atoi(optarg);
      break;
    case 'w':
      c->window = atoi(optarg);
      break;
    case 'Q':
      c->unsig = atoi(optarg);
      break;
    case 'D':
      c->duration = atoi(optarg);
      break;
    case 'm':
      strncpy(c->mode, optarg, sizeof(c->mode) - 1);
      break;
    case 1001:
      c->use_inline = 0;
      break;
    case 1002:
      c->use_uc = 0;
      break;
    default:
      usage(argv[0]);
    }
  }
  if (c->is_server < 0 || !c->dev || !c->ip)
    usage(argv[0]);
}

static void post_one_recv(struct ibv_qp *qp, struct vt_ctx *v, uint64_t id) {
  struct ibv_sge sge = {.addr = (uintptr_t)v->buf,
                        .length = (uint32_t)v->buf_size,
                        .lkey = v->mr->lkey};
  struct ibv_recv_wr wr = {.wr_id = id, .sg_list = &sge, .num_sge = 1};
  struct ibv_recv_wr *bad = NULL;
  VT_CHECK(ibv_post_recv(qp, &wr, &bad) == 0, "post_recv");
}

/* Both sides post RQ WRs before this returns, so the first UD SEND is not dropped. */
static void tcp_ready(int fd, int is_server) {
  char b = 1;
  if (is_server) {
    VT_CHECK(read(fd, &b, 1) == 1, "ready");
    VT_CHECK(write(fd, &b, 1) == 1, "ready");
  } else {
    VT_CHECK(write(fd, &b, 1) == 1, "ready");
    VT_CHECK(read(fd, &b, 1) == 1, "ready");
  }
}

/*
 * SEND and RECV share one CQ. A blind vt_poll_cq() will steal a RECV CQE,
 * and the echo loop will steal the SEND CQE the next signal-wait expects.
 * Either way the peer spins forever (what hung collect_fig5 on SEND/SEND).
 * Count both kinds here and always stop at `deadline`.
 */
struct cq_credit {
  uint64_t signaled;
  uint64_t reaped;
  int pending_recvs; /* RECVs observed while reaping a SEND; not dropped */
};

/* 1 = RECV (repost on rqp), 2 = SEND, 0 = deadline, -1 = error. */
static int poll_progress(struct ibv_cq *cq, struct ibv_qp *rqp, struct vt_ctx *v,
                         uint64_t deadline, struct cq_credit *cr) {
  while (vt_ns() < deadline) {
    struct ibv_wc wc;
    int r = ibv_poll_cq(cq, 1, &wc);
    if (r == 0)
      continue;
    if (r < 0)
      return -1;
    if (wc.status != IBV_WC_SUCCESS) {
      fprintf(stderr, "CQE %s opcode=%u\n", ibv_wc_status_str(wc.status),
              wc.opcode);
      return -1;
    }
    if (wc.opcode & IBV_WC_RECV) {
      if (rqp)
        post_one_recv(rqp, v, wc.wr_id);
      cr->pending_recvs++;
      return 1;
    }
    cr->reaped++;
    return 2;
  }
  return 0;
}

/* Reap one signaled SEND CQE, ignoring (and reposting) RECVs along the way.
 * 0 = caught up or one SEND reaped, -1 = deadline/error with credit still owed.
 */
static int reap_send(struct ibv_cq *cq, struct ibv_qp *rqp, struct vt_ctx *v,
                     uint64_t deadline, struct cq_credit *cr) {
  if (cr->signaled <= cr->reaped)
    return 0;
  while (vt_ns() < deadline) {
    int k = poll_progress(cq, rqp, v, deadline, cr);
    if (k == 2)
      return 0;
    if (k == 1)
      continue;
    return -1;
  }
  return -1;
}

static int wait_recv(struct ibv_cq *cq, struct ibv_qp *rqp, struct vt_ctx *v,
                     uint64_t deadline, struct cq_credit *cr) {
  while (vt_ns() < deadline) {
    if (cr->pending_recvs > 0) {
      cr->pending_recvs--;
      return 1;
    }
    int k = poll_progress(cq, rqp, v, deadline, cr);
    if (k == 1) {
      cr->pending_recvs--;
      return 1;
    }
    if (k <= 0)
      return k;
  }
  return 0;
}

/* -------- WRITE/WRITE ECHO (ww-echo) -------- */
static void run_ww(struct cfg *c) {
  enum ibv_qp_type qpt = c->use_uc ? IBV_QPT_UC : IBV_QPT_RC;
  struct vt_ctx v;
  vt_open_device(&v, c->dev, 1, c->gid_index);
  vt_alloc_buf(&v, VT_BUF_SIZE,
               IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                   IBV_ACCESS_REMOTE_READ);
  struct ibv_qp *qp = vt_create_qp(&v, qpt, VT_MAX_INLINE);
  uint32_t psn = (uint32_t)(vt_ns() & 0xffffff);
  struct vt_endpoint local, remote;
  vt_fill_local_ep(&v, qp, psn, &local);
  vt_qp_to_init(qp, v.port);
  int fd = c->is_server ? vt_tcp_listen(c->ip, c->port)
                        : vt_tcp_connect(c->ip, c->port);
  vt_tcp_exchange(fd, &local, &remote);
  vt_qp_to_rtr(&v, qp, &remote, qpt);
  vt_qp_to_rts(qp, psn);
  tcp_ready(fd, c->is_server);
  close(fd);

  volatile uint8_t *flag = v.buf;
  *flag = 0;
  uint8_t *payload = v.buf + VT_CACHELINE;
  memset(payload, 1, (size_t)c->size);

  int stride = VT_CACHELINE;
  while (stride < c->size)
    stride += VT_CACHELINE;
  VT_CHECK(stride * c->window <= (int)VT_BUF_SIZE, "window");

  struct ibv_send_wr wr[128], *bad;
  struct ibv_sge sgl[128];
  VT_CHECK(c->window <= 128, "window");

  uint64_t nb_tx = 0, echos = 0;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c->duration * 1000000000ull;
  struct cq_credit cr = {0};

  /*
   * Reap previously posted signaled SENDs before the next batch.
   * Do not poll while the batch is still unposted: with -Q 1 every WR is
   * signaled, and vt_poll_cq() inside the build loop waits forever for a
   * CQE that has not been posted yet (hung WR/WR basic).
   */
  if (c->is_server) {
    printf("fig5 ww server window=%d size=%d\n", c->window, c->size);
    fflush(stdout);
    uint64_t lim = deadline + 2000000000ull;
    while (vt_ns() < lim) {
      while (*flag == 0) {
        if (vt_ns() >= lim)
          goto done_ww;
      }
      *flag = 0;
      while (cr.signaled > cr.reaped) {
        if (reap_send(v.cq, NULL, &v, lim, &cr) != 0)
          goto done_ww;
      }
      int batch_sig = 0;
      for (int w = 0; w < c->window; w++) {
        int do_sig = vt_should_signal(nb_tx, c->unsig);
        memset(&wr[w], 0, sizeof(wr[w]));
        memset(&sgl[w], 0, sizeof(sgl[w]));
        wr[w].opcode = IBV_WR_RDMA_WRITE;
        wr[w].num_sge = 1;
        wr[w].next = (w == c->window - 1) ? NULL : &wr[w + 1];
        wr[w].sg_list = &sgl[w];
        wr[w].send_flags = do_sig ? IBV_SEND_SIGNALED : 0;
        if (c->use_inline && c->size <= VT_MAX_INLINE)
          wr[w].send_flags |= IBV_SEND_INLINE;
        payload[0] = 1;
        sgl[w].addr = (uintptr_t)payload;
        sgl[w].length = (uint32_t)c->size;
        sgl[w].lkey = v.mr->lkey;
        wr[w].wr.rdma.remote_addr = remote.addr + (uint64_t)(stride * w);
        wr[w].wr.rdma.rkey = remote.rkey;
        if (do_sig)
          batch_sig++;
        nb_tx++;
      }
      VT_CHECK(ibv_post_send(qp, &wr[0], &bad) == 0, "post");
      cr.signaled += (uint64_t)batch_sig;
      echos += (uint64_t)c->window;
    }
  } else {
    printf("fig5 ww client window=%d size=%d\n", c->window, c->size);
    fflush(stdout);
    while (vt_ns() < deadline) {
      *flag = 0;
      while (cr.signaled > cr.reaped) {
        if (reap_send(v.cq, NULL, &v, deadline, &cr) != 0)
          goto done_ww;
      }
      int batch_sig = 0;
      for (int w = 0; w < c->window; w++) {
        int do_sig = vt_should_signal(nb_tx, c->unsig);
        memset(&wr[w], 0, sizeof(wr[w]));
        memset(&sgl[w], 0, sizeof(sgl[w]));
        wr[w].opcode = IBV_WR_RDMA_WRITE;
        wr[w].num_sge = 1;
        wr[w].next = (w == c->window - 1) ? NULL : &wr[w + 1];
        wr[w].sg_list = &sgl[w];
        wr[w].send_flags = do_sig ? IBV_SEND_SIGNALED : 0;
        if (c->use_inline && c->size <= VT_MAX_INLINE)
          wr[w].send_flags |= IBV_SEND_INLINE;
        payload[0] = 1;
        sgl[w].addr = (uintptr_t)payload;
        sgl[w].length = (uint32_t)c->size;
        sgl[w].lkey = v.mr->lkey;
        wr[w].wr.rdma.remote_addr = remote.addr + (uint64_t)(stride * w);
        wr[w].wr.rdma.rkey = remote.rkey;
        if (do_sig)
          batch_sig++;
        nb_tx++;
      }
      VT_CHECK(ibv_post_send(qp, &wr[0], &bad) == 0, "post");
      cr.signaled += (uint64_t)batch_sig;
      while (*flag == 0) {
        if (vt_ns() >= deadline)
          goto done_ww;
      }
      echos += (uint64_t)c->window;
    }
  }

done_ww:
  if (!c->is_server) {
    double sec = (vt_ns() - t0) / 1e9;
    printf("fig5 ww ECHO: %.2f Mops (completed windows*%d)\n",
           echos / sec / 1e6, c->window);
    fflush(stdout);
  }
  ibv_destroy_qp(qp);
  vt_close_device(&v);
}

/*
 * WRITE request + UD SEND response (ws-echo / HERD).
 *
 * Aligns with rdma_bench/ws-echo:
 *   Client: postlist WRITEs; keep ~RQ credits outstanding; poll dgram CQ
 *           one-for-one once the pipeline is full (not 1-RTT ping-pong).
 *   Server: scan uint64 slots for new seq; batch UD SENDs with postlist.
 * Separate CQs for connected WRITE and UD SEND/RECV (shared CQ mixes poorly).
 */
static struct ibv_qp *qp_on_cq(struct vt_ctx *v, struct ibv_cq *cq,
                               enum ibv_qp_type type, int max_inline) {
  struct ibv_qp_init_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.send_cq = cq;
  attr.recv_cq = cq;
  attr.qp_type = type;
  attr.cap.max_send_wr = VT_SQ_DEPTH;
  attr.cap.max_recv_wr = VT_RQ_DEPTH;
  attr.cap.max_send_sge = 1;
  attr.cap.max_recv_sge = 1;
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

static void run_ws(struct cfg *c) {
  enum ibv_qp_type conn_t = c->use_uc ? IBV_QPT_UC : IBV_QPT_RC;
  int postlist = c->window > 0 ? c->window : 16;
  if (postlist > 64)
    postlist = 64;
  /* One cacheline slot per concurrent request visible to the server. */
  int nslot = postlist;
  int stride = VT_CACHELINE;
  VT_CHECK(stride * nslot + c->size + 64 <= (int)VT_BUF_SIZE, "ws buf");

  struct vt_ctx v;
  vt_open_device(&v, c->dev, 1, c->gid_index);
  vt_alloc_buf(&v, VT_BUF_SIZE,
               IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                   IBV_ACCESS_REMOTE_READ);

  struct ibv_cq *conn_cq = ibv_create_cq(v.ctx, VT_CQ_DEPTH, NULL, NULL, 0);
  struct ibv_cq *dgram_cq = ibv_create_cq(v.ctx, VT_CQ_DEPTH, NULL, NULL, 0);
  VT_CHECK(conn_cq && dgram_cq, "ws cq");
  struct ibv_qp *cqp = qp_on_cq(&v, conn_cq, conn_t, VT_MAX_INLINE);
  struct ibv_qp *dqp = qp_on_cq(&v, dgram_cq, IBV_QPT_UD, VT_MAX_INLINE_UD);

  uint32_t cpsn = (uint32_t)(vt_ns() & 0xffffff);
  uint32_t dpsn = (uint32_t)((vt_ns() >> 8) & 0xffffff);
  struct vt_endpoint clocal, cremote, dlocal, dremote;
  vt_fill_local_ep(&v, cqp, cpsn, &clocal);
  vt_fill_local_ep(&v, dqp, dpsn, &dlocal);

  vt_qp_to_init(cqp, v.port);
  vt_qp_to_init(dqp, v.port);

  int fd = c->is_server ? vt_tcp_listen(c->ip, c->port)
                        : vt_tcp_connect(c->ip, c->port);
  vt_tcp_exchange(fd, &clocal, &cremote);
  vt_tcp_exchange(fd, &dlocal, &dremote);

  vt_qp_to_rtr(&v, cqp, &cremote, conn_t);
  vt_qp_to_rts(cqp, cpsn);
  vt_qp_to_rtr(&v, dqp, &dremote, IBV_QPT_UD);
  vt_qp_to_rts(dqp, dpsn);

  struct vt_qp dvq;
  memset(&dvq, 0, sizeof(dvq));
  dvq.qp = dqp;
  dvq.remote = dremote;
  vt_create_ud_ah(&v, &dvq);

  memset(v.buf, 0, VT_BUF_SIZE);
  uint8_t *payload = v.buf + (size_t)stride * (size_t)nslot;
  memset(payload, 1, (size_t)c->size);

  struct ibv_send_wr wr[64], *bad;
  struct ibv_sge sgl[64];
  uint64_t nb_tx = 0, echos = 0;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c->duration * 1000000000ull;
  const int pipeline = VT_RQ_DEPTH - 8; /* ws-echo uses 512; RQ caps us */

  if (c->is_server) {
    printf("fig5 ws server postlist=%d size=%d\n", postlist, c->size);
    fflush(stdout);
    tcp_ready(fd, 1);
    close(fd);
    uint64_t last_req[64];
    memset(last_req, 0, sizeof(last_req));
    uint64_t nb_dgram = 0, sig_posted = 0, sig_reaped = 0;
    uint64_t srv_deadline = deadline + 2000000000ull;

    while (vt_ns() < srv_deadline) {
      while (sig_posted > sig_reaped) {
        struct ibv_wc wc;
        if (ibv_poll_cq(dgram_cq, 1, &wc) == 0)
          break;
        if (wc.status != IBV_WC_SUCCESS)
          goto ws_cleanup;
        sig_reaped++;
      }

      int nnew = 0;
      for (int s = 0; s < nslot; s++) {
        uint64_t cur = *(volatile uint64_t *)(v.buf + stride * s);
        if (cur == last_req[s])
          continue;
        last_req[s] = cur;

        memset(&wr[nnew], 0, sizeof(wr[nnew]));
        memset(&sgl[nnew], 0, sizeof(sgl[nnew]));
        wr[nnew].opcode = IBV_WR_SEND;
        wr[nnew].num_sge = 1;
        wr[nnew].sg_list = &sgl[nnew];
        wr[nnew].next = NULL;
        int do_sig = (nb_dgram % (uint64_t)c->unsig == 0);
        wr[nnew].send_flags = do_sig ? IBV_SEND_SIGNALED : 0;
        if (c->use_inline && c->size <= VT_MAX_INLINE_UD)
          wr[nnew].send_flags |= IBV_SEND_INLINE;
        wr[nnew].wr.ud.ah = dvq.ah;
        wr[nnew].wr.ud.remote_qpn = dremote.qpn;
        wr[nnew].wr.ud.remote_qkey = 0x11111111;
        sgl[nnew].addr = (uintptr_t)payload;
        sgl[nnew].length = (uint32_t)c->size;
        sgl[nnew].lkey = v.mr->lkey;
        if (nnew > 0)
          wr[nnew - 1].next = &wr[nnew];
        if (do_sig)
          sig_posted++;
        nb_dgram++;
        nnew++;
        if (nnew >= postlist)
          break;
      }
      if (nnew > 0) {
        VT_CHECK(ibv_post_send(dqp, &wr[0], &bad) == 0, "ud send batch");
        echos += (uint64_t)nnew;
      }
    }
  ws_cleanup:
  } else {
    printf("fig5 ws client postlist=%d pipeline=%d size=%d\n", postlist,
           pipeline, c->size);
    fflush(stdout);
    for (int i = 0; i < VT_RQ_DEPTH - 1; i++)
      post_one_recv(dqp, &v, (uint64_t)i);
    tcp_ready(fd, 0);
    close(fd);

    uint64_t req_seq = 0;
    int slot_i = 0;
    uint64_t outstanding = 0; /* WRITEs posted waiting for UD RECV */

    while (vt_ns() < deadline) {
      for (int i = 0; i < postlist; i++) {
        /* Credit: once pipeline is full, wait for one RECV before more WRITEs. */
        if ((int)outstanding >= pipeline) {
          struct ibv_wc wc;
          while (ibv_poll_cq(dgram_cq, 1, &wc) == 0) {
            if (vt_ns() >= deadline)
              goto ws_client_done;
          }
          if (wc.status != IBV_WC_SUCCESS)
            goto ws_client_done;
          post_one_recv(dqp, &v, wc.wr_id);
          outstanding--;
          echos++;
        }

        memset(&wr[i], 0, sizeof(wr[i]));
        memset(&sgl[i], 0, sizeof(sgl[i]));
        wr[i].opcode = IBV_WR_RDMA_WRITE;
        wr[i].num_sge = 1;
        wr[i].next = (i == postlist - 1) ? NULL : &wr[i + 1];
        wr[i].sg_list = &sgl[i];
        wr[i].send_flags =
            (nb_tx % (uint64_t)c->unsig == 0) ? IBV_SEND_SIGNALED : 0;
        if (nb_tx % (uint64_t)c->unsig == 0 && nb_tx > 0)
          vt_poll_cq(conn_cq, 1);
        if (c->use_inline && c->size <= VT_MAX_INLINE)
          wr[i].send_flags |= IBV_SEND_INLINE;

        req_seq++;
        /* WRITE a new seq into the remote slot (ws-echo last_req style). */
        *(uint64_t *)payload = req_seq;
        sgl[i].addr = (uintptr_t)payload;
        sgl[i].length = (uint32_t)(c->size < 8 ? 8 : c->size);
        sgl[i].lkey = v.mr->lkey;
        wr[i].wr.rdma.remote_addr =
            cremote.addr + (uint64_t)(stride * slot_i);
        wr[i].wr.rdma.rkey = cremote.rkey;

        slot_i++;
        if (slot_i >= nslot)
          slot_i = 0;
        nb_tx++;
        outstanding++;
      }
      VT_CHECK(ibv_post_send(cqp, &wr[0], &bad) == 0, "write postlist");
    }
  ws_client_done:
    /* Drain remaining RECVs briefly so the last window counts. */
    uint64_t drain_end = vt_ns() + 500000000ull;
    while (outstanding > 0 && vt_ns() < drain_end) {
      struct ibv_wc wc;
      if (ibv_poll_cq(dgram_cq, 1, &wc) == 1 && wc.status == IBV_WC_SUCCESS) {
        post_one_recv(dqp, &v, wc.wr_id);
        outstanding--;
        echos++;
      }
    }
    double sec = (vt_ns() - t0) / 1e9;
    printf("fig5 ws ECHO: %.2f Mops (postlist=%d)\n", echos / sec / 1e6,
           postlist);
    fflush(stdout);
  }

  if (dvq.ah)
    ibv_destroy_ah(dvq.ah);
  ibv_destroy_qp(cqp);
  ibv_destroy_qp(dqp);
  ibv_destroy_cq(conn_cq);
  ibv_destroy_cq(dgram_cq);
  vt_close_device(&v);
}

/* SEND/SEND over UD both ways (simplified ss). */
static void run_ss(struct cfg *c) {
  struct vt_ctx v;
  vt_open_device(&v, c->dev, 1, c->gid_index);
  vt_alloc_buf(&v, VT_BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);
  struct ibv_qp *qp = vt_create_qp(&v, IBV_QPT_UD, VT_MAX_INLINE_UD);
  uint32_t psn = (uint32_t)(vt_ns() & 0xffffff);
  struct vt_endpoint local, remote;
  vt_fill_local_ep(&v, qp, psn, &local);
  vt_qp_to_init(qp, v.port);
  int fd = c->is_server ? vt_tcp_listen(c->ip, c->port)
                        : vt_tcp_connect(c->ip, c->port);
  vt_tcp_exchange(fd, &local, &remote);
  vt_qp_to_rtr(&v, qp, &remote, IBV_QPT_UD);
  vt_qp_to_rts(qp, psn);
  struct vt_qp vq = {.qp = qp, .remote = remote};
  vt_create_ud_ah(&v, &vq);

  for (int i = 0; i < VT_RQ_DEPTH / 2; i++)
    post_one_recv(qp, &v, (uint64_t)i);
  tcp_ready(fd, c->is_server);
  close(fd);

  uint8_t *payload = v.buf + VT_CACHELINE;
  memset(payload, 1, (size_t)c->size);
  struct ibv_send_wr wr, *bad;
  struct ibv_sge sge;
  uint64_t nb_tx = 0, echos = 0;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c->duration * 1000000000ull;

  struct cq_credit cr = {0};

  if (c->is_server) {
    printf("fig5 ss server\n");
    fflush(stdout);
    uint64_t lim = deadline + 2000000000ull;
    while (vt_ns() < lim) {
      if (wait_recv(v.cq, qp, &v, lim, &cr) != 1)
        break;
      int do_sig = vt_should_signal(nb_tx, c->unsig);
      if (do_sig && reap_send(v.cq, qp, &v, lim, &cr) != 0)
        break;
      memset(&wr, 0, sizeof(wr));
      memset(&sge, 0, sizeof(sge));
      wr.opcode = IBV_WR_SEND;
      wr.sg_list = &sge;
      wr.num_sge = 1;
      wr.send_flags = do_sig ? IBV_SEND_SIGNALED : 0;
      if (c->use_inline && c->size <= VT_MAX_INLINE_UD)
        wr.send_flags |= IBV_SEND_INLINE;
      wr.wr.ud.ah = vq.ah;
      wr.wr.ud.remote_qpn = remote.qpn;
      wr.wr.ud.remote_qkey = 0x11111111;
      sge.addr = (uintptr_t)payload;
      sge.length = (uint32_t)c->size;
      sge.lkey = v.mr->lkey;
      VT_CHECK(ibv_post_send(qp, &wr, &bad) == 0, "send");
      if (do_sig)
        cr.signaled++;
      nb_tx++;
      echos++;
    }
  } else {
    printf("fig5 ss client\n");
    fflush(stdout);
    int win = c->window > 0 ? c->window : 1;
    if (win > 32)
      win = 32;
    while (vt_ns() < deadline) {
      int posted = 0;
      for (int w = 0; w < win && vt_ns() < deadline; w++) {
        int do_sig = vt_should_signal(nb_tx, c->unsig);
        if (do_sig && reap_send(v.cq, qp, &v, deadline, &cr) != 0)
          goto ss_client_done;
        memset(&wr, 0, sizeof(wr));
        memset(&sge, 0, sizeof(sge));
        wr.opcode = IBV_WR_SEND;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.send_flags = do_sig ? IBV_SEND_SIGNALED : 0;
        if (c->use_inline && c->size <= VT_MAX_INLINE_UD)
          wr.send_flags |= IBV_SEND_INLINE;
        wr.wr.ud.ah = vq.ah;
        wr.wr.ud.remote_qpn = remote.qpn;
        wr.wr.ud.remote_qkey = 0x11111111;
        sge.addr = (uintptr_t)payload;
        sge.length = (uint32_t)c->size;
        sge.lkey = v.mr->lkey;
        VT_CHECK(ibv_post_send(qp, &wr, &bad) == 0, "send");
        if (do_sig)
          cr.signaled++;
        nb_tx++;
        posted++;
      }
      for (int w = 0; w < posted; w++) {
        if (wait_recv(v.cq, qp, &v, deadline, &cr) != 1)
          goto ss_client_done;
        echos++;
      }
    }
  ss_client_done:
    double sec = (vt_ns() - t0) / 1e9;
    printf("fig5 ss ECHO: %.2f Mops\n", echos / sec / 1e6);
    fflush(stdout);
  }

  if (vq.ah)
    ibv_destroy_ah(vq.ah);
  ibv_destroy_qp(qp);
  vt_close_device(&v);
}

int main(int argc, char **argv) {
  struct cfg c;
  parse(argc, argv, &c);
  if (strcmp(c.mode, "ww") == 0)
    run_ww(&c);
  else if (strcmp(c.mode, "ws") == 0)
    run_ws(&c);
  else if (strcmp(c.mode, "ss") == 0)
    run_ss(&c);
  else {
    fprintf(stderr, "unknown mode %s\n", c.mode);
    return 1;
  }
  return 0;
}
