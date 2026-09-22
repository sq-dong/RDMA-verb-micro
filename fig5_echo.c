/*
 * fig5_echo.c — HERD Sec.3 Fig.5: ECHO throughput (32B in the paper).
 *
 * Templates (rdma_bench):
 *   ww  WRITE/WRITE : ww-echo/{client,server}.c
 *   ws  WRITE/SEND  : ws-echo/{client,worker}.c  — HERD choice
 *   ss  SEND/SEND   : UD both ways (RC SEND for "basic" with --rc)
 *
 * Optimizations (paper bars):
 *   basic         RC (or signaled, no inline)
 *   +unreliable   UC / UD
 *   +unsignalled  selective signaling (-Q)
 *   +inlined      IBV_SEND_INLINE
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
  int use_uc; /* WRITE path / UD for SEND; 0 => RC "basic" */
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

static void post_one_recv(struct ibv_qp *qp, struct vt_ctx *v, uint64_t id,
                          size_t len) {
  struct ibv_sge sge = {.addr = (uintptr_t)v->buf,
                        .length = (uint32_t)len,
                        .lkey = v->mr->lkey};
  struct ibv_recv_wr wr = {.wr_id = id, .sg_list = &sge, .num_sge = 1};
  struct ibv_recv_wr *bad = NULL;
  VT_CHECK(ibv_post_recv(qp, &wr, &bad) == 0, "post_recv");
}

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

/*
 * WRITE/WRITE ECHO — matches ww-echo:
 *   Client posts a window of inlined WRITEs, waits for conn_buf[0] != 0.
 *   Server waits for flag, posts a window of response WRITEs.
 *   Window sizes equal; signal only WR[0] of each batch (ww-echo style).
 */
static void run_ww(struct cfg *c) {
  enum ibv_qp_type qpt = c->use_uc ? IBV_QPT_UC : IBV_QPT_RC;
  int inl = vt_inline_grant(c->size, c->use_inline, 0);

  struct vt_ctx v;
  vt_open_device(&v, c->dev, 1, c->gid_index);
  vt_alloc_buf(&v, VT_BUF_SIZE,
               IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                   IBV_ACCESS_REMOTE_READ);
  struct ibv_qp *qp = vt_create_qp(&v, qpt, inl, NULL, NULL);
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

  int stride = VT_CACHELINE;
  while (stride < c->size)
    stride += VT_CACHELINE;
  VT_CHECK(stride * c->window <= (int)VT_BUF_SIZE, "window");

  uint8_t *req_buf = malloc((size_t)c->size);
  uint8_t *resp_buf = malloc((size_t)c->size);
  VT_CHECK(req_buf && resp_buf, "malloc");
  memset(req_buf, 1, (size_t)c->size);
  memset(resp_buf, 1, (size_t)c->size);

  struct ibv_send_wr wr[128], *bad;
  struct ibv_sge sgl[128];
  struct ibv_wc wc;
  VT_CHECK(c->window <= 128, "window");

  uint64_t echos = 0;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c->duration * 1000000000ull;
  /* Server runs a little longer so the client can finish. */
  uint64_t lim = c->is_server ? deadline + 2000000000ull : deadline;

  if (c->is_server) {
    printf("fig5 ww server window=%d size=%d inl=%d\n", c->window, c->size,
           inl);
    fflush(stdout);
    while (vt_ns() < lim) {
      while (*flag == 0) {
        if (vt_ns() >= lim)
          goto done_ww;
      }
      *flag = 0;

      for (int w = 0; w < c->window; w++) {
        memset(&wr[w], 0, sizeof(wr[w]));
        memset(&sgl[w], 0, sizeof(sgl[w]));
        wr[w].opcode = IBV_WR_RDMA_WRITE;
        wr[w].num_sge = 1;
        wr[w].next = (w == c->window - 1) ? NULL : &wr[w + 1];
        wr[w].sg_list = &sgl[w];
        /* -Q 1 ⇒ all signaled (basic); else ww-echo: only WR[0]. */
        if (c->unsig <= 1)
          wr[w].send_flags = IBV_SEND_SIGNALED;
        else
          wr[w].send_flags = (w == 0) ? IBV_SEND_SIGNALED : 0;
        if (c->use_inline && c->size <= inl)
          wr[w].send_flags |= IBV_SEND_INLINE;
        sgl[w].addr = (uintptr_t)resp_buf;
        sgl[w].length = (uint32_t)c->size;
        sgl[w].lkey = v.mr->lkey;
        wr[w].wr.rdma.remote_addr = remote.addr + (uint64_t)(stride * w);
        wr[w].wr.rdma.rkey = remote.rkey;
      }
      VT_CHECK(ibv_post_send(qp, &wr[0], &bad) == 0, "post");
      vt_poll_cq(v.cq, c->unsig <= 1 ? c->window : 1);
      echos += (uint64_t)c->window;
    }
  } else {
    printf("fig5 ww client window=%d size=%d inl=%d\n", c->window, c->size,
           inl);
    fflush(stdout);
    while (vt_ns() < deadline) {
      *flag = 0;
      for (int w = 0; w < c->window; w++) {
        memset(&wr[w], 0, sizeof(wr[w]));
        memset(&sgl[w], 0, sizeof(sgl[w]));
        wr[w].opcode = IBV_WR_RDMA_WRITE;
        wr[w].num_sge = 1;
        wr[w].next = (w == c->window - 1) ? NULL : &wr[w + 1];
        wr[w].sg_list = &sgl[w];
        if (c->unsig <= 1)
          wr[w].send_flags = IBV_SEND_SIGNALED;
        else
          wr[w].send_flags = (w == 0) ? IBV_SEND_SIGNALED : 0;
        if (c->use_inline && c->size <= inl)
          wr[w].send_flags |= IBV_SEND_INLINE;
        /* Nonzero first byte so server sees a request. */
        req_buf[0] = 1;
        sgl[w].addr = (uintptr_t)req_buf;
        sgl[w].length = (uint32_t)c->size;
        sgl[w].lkey = v.mr->lkey;
        wr[w].wr.rdma.remote_addr = remote.addr + (uint64_t)(stride * w);
        wr[w].wr.rdma.rkey = remote.rkey;
      }
      VT_CHECK(ibv_post_send(qp, &wr[0], &bad) == 0, "post");
      vt_poll_cq(v.cq, c->unsig <= 1 ? c->window : 1);
      while (*flag == 0) {
        if (vt_ns() >= deadline)
          goto done_ww;
      }
      echos += (uint64_t)c->window;
    }
  }

done_ww:
  (void)wc;
  if (!c->is_server) {
    double sec = (vt_ns() - t0) / 1e9;
    printf("fig5 ww ECHO: %.2f Mops (completed windows*%d)\n",
           echos / sec / 1e6, c->window);
    fflush(stdout);
  }
  free(req_buf);
  free(resp_buf);
  ibv_destroy_qp(qp);
  vt_close_device(&v);
}

/*
 * WRITE request + UD SEND response — matches ws-echo.
 *
 * Key details from ws-echo/client.c + worker.c:
 *   - Separate req_buf[i] per postlist entry (never share one buffer).
 *   - Single slot at server; client increments seq; server does last_req++
 *     on any change (catch-up), not last_req = cur.
 *   - Client keeps ~512 RECV credits; after fill, poll 1 RECV per new WRITE.
 *   - Separate CQs for connected WRITE and UD SEND/RECV.
 */
static void run_ws(struct cfg *c) {
  enum ibv_qp_type conn_t = c->use_uc ? IBV_QPT_UC : IBV_QPT_RC;
  int postlist = c->window > 0 ? c->window : 16;
  if (postlist > 64)
    postlist = 64;
  int inl_c = vt_inline_grant(c->size, c->use_inline, 0);
  int inl_d = vt_inline_grant(c->size, c->use_inline, 1);
  /* Pipeline depth like ws-echo (nb_tx >= 512 before polling RECVs). */
  const int pipeline = 512;

  struct vt_ctx v;
  vt_open_device(&v, c->dev, 1, c->gid_index);
  vt_alloc_buf(&v, VT_BUF_SIZE,
               IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                   IBV_ACCESS_REMOTE_READ);

  struct ibv_cq *conn_cq = ibv_create_cq(v.ctx, VT_CQ_DEPTH, NULL, NULL, 0);
  struct ibv_cq *dgram_cq = ibv_create_cq(v.ctx, VT_CQ_DEPTH, NULL, NULL, 0);
  VT_CHECK(conn_cq && dgram_cq, "ws cq");
  /* Steal default cq pointer unused; QPs bind to dedicated CQs. */
  struct ibv_qp *cqp = qp_on_cq(&v, conn_cq, conn_t, inl_c);
  struct ibv_qp *dqp = qp_on_cq(&v, dgram_cq, IBV_QPT_UD, inl_d);

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

  /* One request slot (cacheline) — same as ws-echo single client→worker slot. */
  memset(v.buf, 0, VT_BUF_SIZE);
  volatile uint64_t *req_slot = (volatile uint64_t *)v.buf;

  uint8_t *resp_buf = malloc((size_t)c->size);
  VT_CHECK(resp_buf, "malloc");
  memset(resp_buf, 1, (size_t)c->size);

  /* Per-postlist request buffers (ws-echo req_buf[i]). */
  long long *req_bufs[64];
  for (int i = 0; i < postlist; i++) {
    req_bufs[i] = malloc((size_t)c->size < 8 ? 8 : (size_t)c->size);
    VT_CHECK(req_bufs[i], "req_buf");
    memset(req_bufs[i], 1, (size_t)c->size < 8 ? 8 : (size_t)c->size);
  }

  struct ibv_send_wr wr[64], *bad;
  struct ibv_sge sgl[64];
  struct ibv_wc wc;
  uint64_t nb_tx = 0, echos = 0;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c->duration * 1000000000ull;

  if (c->is_server) {
    printf("fig5 ws server postlist=%d size=%d\n", postlist, c->size);
    fflush(stdout);
    tcp_ready(fd, 1);
    close(fd);

    long long last_req = 0;
    uint64_t nb_dgram = 0;
    uint64_t srv_deadline = deadline + 2000000000ull;

    while (vt_ns() < srv_deadline) {
      int nnew = 0;
      /*
       * Catch-up like ws-echo worker:
       *   if (slot == last_req) continue; else last_req++;
       * Multiple WRITEs may collapse into one visible value; we still emit
       * one SEND per expected sequence step so client credits stay matched.
       */
      while (nnew < postlist) {
        long long cur = (long long)(*req_slot);
        if (cur == last_req)
          break;
        last_req++;

        memset(&wr[nnew], 0, sizeof(wr[nnew]));
        memset(&sgl[nnew], 0, sizeof(sgl[nnew]));
        wr[nnew].opcode = IBV_WR_SEND;
        wr[nnew].num_sge = 1;
        wr[nnew].sg_list = &sgl[nnew];
        wr[nnew].next = NULL;
        int do_sig = vt_should_signal(nb_dgram, c->unsig);
        wr[nnew].send_flags = do_sig ? IBV_SEND_SIGNALED : 0;
        if (do_sig && nb_dgram > 0)
          vt_poll_cq(dgram_cq, 1);
        if (c->use_inline && c->size <= inl_d)
          wr[nnew].send_flags |= IBV_SEND_INLINE;
        wr[nnew].wr.ud.ah = dvq.ah;
        wr[nnew].wr.ud.remote_qpn = dremote.qpn;
        wr[nnew].wr.ud.remote_qkey = 0x11111111;
        sgl[nnew].addr = (uintptr_t)resp_buf;
        sgl[nnew].length = (uint32_t)c->size;
        sgl[nnew].lkey = v.mr->lkey;
        if (nnew > 0)
          wr[nnew - 1].next = &wr[nnew];
        nb_dgram++;
        nnew++;
      }
      if (nnew > 0)
        VT_CHECK(ibv_post_send(dqp, &wr[0], &bad) == 0, "ud send batch");
    }
  } else {
    printf("fig5 ws client postlist=%d pipeline=%d size=%d\n", postlist,
           pipeline, c->size);
    fflush(stdout);

    /*
     * ws-echo: post one RECV per request from the start (no big pre-fill).
     * RQ fills up to `pipeline`, then we poll+repost one-for-one.
     */
    tcp_ready(fd, 0);
    close(fd);

    long long req_seq = 0;

    while (vt_ns() < deadline) {
      for (int i = 0; i < postlist; i++) {
        /* After pipeline fill: one RECV completion per new request. */
        if ((int)nb_tx >= pipeline) {
          while (ibv_poll_cq(dgram_cq, 1, &wc) == 0) {
            if (vt_ns() >= deadline)
              goto ws_client_done;
          }
          if (wc.status != IBV_WC_SUCCESS)
            goto ws_client_done;
          echos++;
        }
        /* Always replenish RQ (ws-echo). */
        post_one_recv(dqp, &v, (uint64_t)i, VT_BUF_SIZE);

        memset(&wr[i], 0, sizeof(wr[i]));
        memset(&sgl[i], 0, sizeof(sgl[i]));
        wr[i].opcode = IBV_WR_RDMA_WRITE;
        wr[i].num_sge = 1;
        wr[i].next = (i == postlist - 1) ? NULL : &wr[i + 1];
        wr[i].sg_list = &sgl[i];
        wr[i].send_flags =
            vt_should_signal(nb_tx, c->unsig) ? IBV_SEND_SIGNALED : 0;
        if (vt_should_signal(nb_tx, c->unsig) && nb_tx > 0)
          vt_poll_cq(conn_cq, 1);
        if (c->use_inline && c->size <= inl_c)
          wr[i].send_flags |= IBV_SEND_INLINE;

        req_seq++;
        req_bufs[i][0] = req_seq;
        sgl[i].addr = (uintptr_t)req_bufs[i];
        sgl[i].length = (uint32_t)(c->size < 8 ? 8 : c->size);
        sgl[i].lkey = v.mr->lkey;
        /* Always the same remote slot (ws-echo). */
        wr[i].wr.rdma.remote_addr = cremote.addr;
        wr[i].wr.rdma.rkey = cremote.rkey;
        nb_tx++;
      }
      VT_CHECK(ibv_post_send(cqp, &wr[0], &bad) == 0, "write postlist");
    }
  ws_client_done:
    /* Drain remaining RECVs for in-flight WRITEs. */
    uint64_t drain_end = vt_ns() + 500000000ull;
    while (echos < nb_tx && vt_ns() < drain_end) {
      if (ibv_poll_cq(dgram_cq, 1, &wc) == 1 && wc.status == IBV_WC_SUCCESS)
        echos++;
    }
    double sec = (vt_ns() - t0) / 1e9;
    printf("fig5 ws ECHO: %.2f Mops (postlist=%d)\n", echos / sec / 1e6,
           postlist);
    fflush(stdout);
  }

  for (int i = 0; i < postlist; i++)
    free(req_bufs[i]);
  free(resp_buf);
  if (dvq.ah)
    ibv_destroy_ah(dvq.ah);
  ibv_destroy_qp(cqp);
  ibv_destroy_qp(dqp);
  ibv_destroy_cq(conn_cq);
  ibv_destroy_cq(dgram_cq);
  vt_close_device(&v);
}

/*
 * SEND/SEND: UD by default (+unreliable). With --rc use RC SEND both ways
 * for the paper "basic" bar.
 */
static void run_ss(struct cfg *c) {
  int use_ud = c->use_uc; /* --rc => RC SEND */
  enum ibv_qp_type qpt = use_ud ? IBV_QPT_UD : IBV_QPT_RC;
  int inl = vt_inline_grant(c->size, c->use_inline, use_ud);

  struct vt_ctx v;
  vt_open_device(&v, c->dev, 1, c->gid_index);
  vt_alloc_buf(&v, VT_BUF_SIZE,
               IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                   IBV_ACCESS_REMOTE_READ);
  struct ibv_qp *qp = vt_create_qp(&v, qpt, inl, NULL, NULL);
  uint32_t psn = (uint32_t)(vt_ns() & 0xffffff);
  struct vt_endpoint local, remote;
  vt_fill_local_ep(&v, qp, psn, &local);
  vt_qp_to_init(qp, v.port);
  int fd = c->is_server ? vt_tcp_listen(c->ip, c->port)
                        : vt_tcp_connect(c->ip, c->port);
  vt_tcp_exchange(fd, &local, &remote);
  vt_qp_to_rtr(&v, qp, &remote, qpt);
  vt_qp_to_rts(qp, psn);

  struct vt_qp vq;
  memset(&vq, 0, sizeof(vq));
  vq.qp = qp;
  vq.remote = remote;
  if (use_ud)
    vt_create_ud_ah(&v, &vq);

  for (int i = 0; i < VT_RQ_DEPTH / 2; i++)
    post_one_recv(qp, &v, (uint64_t)i, VT_BUF_SIZE);
  tcp_ready(fd, c->is_server);
  close(fd);

  uint8_t *payload = malloc((size_t)c->size);
  VT_CHECK(payload, "malloc");
  memset(payload, 1, (size_t)c->size);

  struct ibv_send_wr wr, *bad;
  struct ibv_sge sge;
  struct ibv_wc wc;
  uint64_t nb_tx = 0, echos = 0;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c->duration * 1000000000ull;
  int win = c->window > 0 ? c->window : 32;
  if (win > 64)
    win = 64;

  if (c->is_server) {
    printf("fig5 ss server qpt=%s win=%d\n", use_ud ? "UD" : "RC", win);
    fflush(stdout);
    uint64_t lim = deadline + 2000000000ull;
    while (vt_ns() < lim) {
      /* Collect up to `win` RECVs, then reply with same count. */
      int got = 0;
      while (got < win && vt_ns() < lim) {
        int r = ibv_poll_cq(v.cq, 1, &wc);
        if (r == 0)
          continue;
        if (wc.status != IBV_WC_SUCCESS)
          goto ss_done;
        if (wc.opcode & IBV_WC_RECV) {
          post_one_recv(qp, &v, wc.wr_id, VT_BUF_SIZE);
          got++;
        }
        /* Ignore SEND CQEs here; reaped via selective signal below. */
      }
      if (got == 0)
        continue;
      for (int i = 0; i < got; i++) {
        int do_sig = vt_should_signal(nb_tx, c->unsig);
        if (do_sig && nb_tx > 0) {
          /* Reap SEND CQE only (skip any RECV). */
          for (;;) {
            int r = ibv_poll_cq(v.cq, 1, &wc);
            if (r == 0)
              continue;
            if (wc.status != IBV_WC_SUCCESS)
              goto ss_done;
            if (wc.opcode & IBV_WC_RECV)
              post_one_recv(qp, &v, wc.wr_id, VT_BUF_SIZE);
            else
              break;
          }
        }
        memset(&wr, 0, sizeof(wr));
        memset(&sge, 0, sizeof(sge));
        wr.opcode = IBV_WR_SEND;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.send_flags = do_sig ? IBV_SEND_SIGNALED : 0;
        if (c->use_inline && c->size <= inl)
          wr.send_flags |= IBV_SEND_INLINE;
        if (use_ud) {
          wr.wr.ud.ah = vq.ah;
          wr.wr.ud.remote_qpn = remote.qpn;
          wr.wr.ud.remote_qkey = 0x11111111;
        }
        sge.addr = (uintptr_t)payload;
        sge.length = (uint32_t)c->size;
        sge.lkey = v.mr->lkey;
        VT_CHECK(ibv_post_send(qp, &wr, &bad) == 0, "send");
        nb_tx++;
      }
    }
  } else {
    printf("fig5 ss client qpt=%s win=%d\n", use_ud ? "UD" : "RC", win);
    fflush(stdout);
    while (vt_ns() < deadline) {
      for (int i = 0; i < win; i++) {
        int do_sig = vt_should_signal(nb_tx, c->unsig);
        if (do_sig && nb_tx > 0) {
          for (;;) {
            int r = ibv_poll_cq(v.cq, 1, &wc);
            if (r == 0) {
              if (vt_ns() >= deadline)
                goto ss_client_done;
              continue;
            }
            if (wc.status != IBV_WC_SUCCESS)
              goto ss_client_done;
            if (wc.opcode & IBV_WC_RECV) {
              post_one_recv(qp, &v, wc.wr_id, VT_BUF_SIZE);
              echos++;
            } else
              break;
          }
        }
        memset(&wr, 0, sizeof(wr));
        memset(&sge, 0, sizeof(sge));
        wr.opcode = IBV_WR_SEND;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.send_flags = do_sig ? IBV_SEND_SIGNALED : 0;
        if (c->use_inline && c->size <= inl)
          wr.send_flags |= IBV_SEND_INLINE;
        if (use_ud) {
          wr.wr.ud.ah = vq.ah;
          wr.wr.ud.remote_qpn = remote.qpn;
          wr.wr.ud.remote_qkey = 0x11111111;
        }
        sge.addr = (uintptr_t)payload;
        sge.length = (uint32_t)c->size;
        sge.lkey = v.mr->lkey;
        VT_CHECK(ibv_post_send(qp, &wr, &bad) == 0, "send");
        nb_tx++;
      }
      for (int i = 0; i < win; i++) {
        for (;;) {
          int r = ibv_poll_cq(v.cq, 1, &wc);
          if (r == 0) {
            if (vt_ns() >= deadline)
              goto ss_client_done;
            continue;
          }
          if (wc.status != IBV_WC_SUCCESS)
            goto ss_client_done;
          if (wc.opcode & IBV_WC_RECV) {
            post_one_recv(qp, &v, wc.wr_id, VT_BUF_SIZE);
            echos++;
            break;
          }
        }
      }
    }
  ss_client_done:
    {
      double sec = (vt_ns() - t0) / 1e9;
      printf("fig5 ss ECHO: %.2f Mops\n", echos / sec / 1e6);
      fflush(stdout);
    }
  }

ss_done:
  free(payload);
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
