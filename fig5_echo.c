/*
 * fig5_echo.c — HERD Sec.3 Fig.5: ECHO throughput (32B).
 *
 * Templates:
 *   ww  → rdma_bench/ww-echo/{client,server}.c
 *   ws  → rdma_bench/ws-echo/{client,worker}.c
 *   ss  → rdma_bench/ss-echo/main.cc  (separate send/recv CQs!)
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
  int use_uc;
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
  if (c->unsig < 1)
    c->unsig = 1;
  if (c->window < 1)
    c->window = 1;
  if (c->window > 128)
    c->window = 128;
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

static struct ibv_qp *qp_on_cqs(struct vt_ctx *v, struct ibv_cq *scq,
                                struct ibv_cq *rcq, enum ibv_qp_type type,
                                int max_inline, int rq_depth) {
  struct ibv_qp_init_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.send_cq = scq;
  attr.recv_cq = rcq;
  attr.qp_type = type;
  attr.cap.max_send_wr = VT_SQ_DEPTH;
  attr.cap.max_recv_wr = (uint32_t)rq_depth;
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

static void post_recv(struct ibv_qp *qp, struct vt_ctx *v, uint64_t addr,
                      uint32_t len, uint64_t id) {
  struct ibv_sge sge = {.addr = addr, .length = len, .lkey = v->mr->lkey};
  struct ibv_recv_wr wr = {.wr_id = id, .sg_list = &sge, .num_sge = 1};
  struct ibv_recv_wr *bad = NULL;
  VT_CHECK(ibv_post_recv(qp, &wr, &bad) == 0, "post_recv");
}

static void poll_n(struct ibv_cq *cq, int n) {
  struct ibv_wc wc;
  int got = 0;
  while (got < n) {
    int r = ibv_poll_cq(cq, 1, &wc);
    if (r < 0)
      VT_DIE("poll");
    if (r == 0)
      continue;
    if (wc.status != IBV_WC_SUCCESS) {
      fprintf(stderr, "CQE %s\n", ibv_wc_status_str(wc.status));
      exit(1);
    }
    got++;
  }
}

/* -------- WRITE / WRITE (ww-echo) -------- */
static void run_ww(struct cfg *c) {
  enum ibv_qp_type qpt = c->use_uc ? IBV_QPT_UC : IBV_QPT_RC;
  int inl = vt_inline_grant(c->size, c->use_inline, 0);
  int win = c->window;

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
  /* Receive window + local send staging (must be in MR for --no-inline). */
  VT_CHECK(stride * win + c->size + 64 <= (int)VT_BUF_SIZE, "window");
  uint8_t *send_buf = v.buf + (size_t)stride * (size_t)win;
  memset(send_buf, 1, (size_t)c->size);

  struct ibv_send_wr wr[128], *bad;
  struct ibv_sge sgl[128];
  uint64_t echos = 0, iters = 0;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c->duration * 1000000000ull;
  uint64_t lim = c->is_server ? deadline + 3000000000ull : deadline;

  /*
   * ww-echo: post window WRITEs, signal last, poll 1 CQE, wait on flag.
   * Payload lives in MR (send_buf). ww-echo always inlines; --no-inline
   * must still DMA from registered memory or we get LOC_PROT_ERR.
   */
  if (c->is_server) {
    printf("fig5 ww server win=%d size=%d\n", win, c->size);
    fflush(stdout);
    while (vt_ns() < lim) {
      while (*flag == 0) {
        if (vt_ns() >= lim)
          goto done_ww;
      }
      *flag = 0;
      for (int w = 0; w < win; w++) {
        memset(&wr[w], 0, sizeof(wr[w]));
        wr[w].opcode = IBV_WR_RDMA_WRITE;
        wr[w].num_sge = 1;
        wr[w].next = (w == win - 1) ? NULL : &wr[w + 1];
        wr[w].sg_list = &sgl[w];
        if (c->unsig <= 1)
          wr[w].send_flags = IBV_SEND_SIGNALED;
        else
          wr[w].send_flags = (w == win - 1) ? IBV_SEND_SIGNALED : 0;
        if (c->use_inline && c->size <= inl)
          wr[w].send_flags |= IBV_SEND_INLINE;
        sgl[w].addr = (uintptr_t)send_buf;
        sgl[w].length = (uint32_t)c->size;
        sgl[w].lkey = v.mr->lkey;
        wr[w].wr.rdma.remote_addr = remote.addr + (uint64_t)(stride * w);
        wr[w].wr.rdma.rkey = remote.rkey;
      }
      VT_CHECK(ibv_post_send(qp, &wr[0], &bad) == 0, "post");
      poll_n(v.cq, c->unsig <= 1 ? win : 1);
      echos += (uint64_t)win;
    }
  } else {
    printf("fig5 ww client win=%d size=%d\n", win, c->size);
    fflush(stdout);
    while (1) {
      if ((iters & 0xff) == 0 && vt_ns() >= deadline)
        break;
      iters++;
      *flag = 0;
      send_buf[0] = 1;
      for (int w = 0; w < win; w++) {
        memset(&wr[w], 0, sizeof(wr[w]));
        wr[w].opcode = IBV_WR_RDMA_WRITE;
        wr[w].num_sge = 1;
        wr[w].next = (w == win - 1) ? NULL : &wr[w + 1];
        wr[w].sg_list = &sgl[w];
        if (c->unsig <= 1)
          wr[w].send_flags = IBV_SEND_SIGNALED;
        else
          wr[w].send_flags = (w == win - 1) ? IBV_SEND_SIGNALED : 0;
        if (c->use_inline && c->size <= inl)
          wr[w].send_flags |= IBV_SEND_INLINE;
        sgl[w].addr = (uintptr_t)send_buf;
        sgl[w].length = (uint32_t)c->size;
        sgl[w].lkey = v.mr->lkey;
        wr[w].wr.rdma.remote_addr = remote.addr + (uint64_t)(stride * w);
        wr[w].wr.rdma.rkey = remote.rkey;
      }
      VT_CHECK(ibv_post_send(qp, &wr[0], &bad) == 0, "post");
      poll_n(v.cq, c->unsig <= 1 ? win : 1);
      while (*flag == 0) {
        if ((iters & 0xff) == 0 && vt_ns() >= deadline)
          goto done_ww;
      }
      echos += (uint64_t)win;
    }
  }

done_ww:
  if (!c->is_server) {
    double sec = (vt_ns() - t0) / 1e9;
    if (sec < 1e-6)
      sec = 1e-6;
    printf("fig5 ww ECHO: %.2f Mops (win=%d)\n", echos / sec / 1e6, win);
    fflush(stdout);
  }
  ibv_destroy_qp(qp);
  vt_close_device(&v);
}

/* -------- WRITE / SEND (ws-echo) -------- */
static void run_ws(struct cfg *c) {
  enum ibv_qp_type conn_t = c->use_uc ? IBV_QPT_UC : IBV_QPT_RC;
  int postlist = c->window;
  int inl_c = vt_inline_grant(c->size, c->use_inline, 0);
  int inl_d = vt_inline_grant(c->size, c->use_inline, 1);
  /* ws-echo: start polling RECVs after 512 outstanding. */
  const int pipeline = 512;
  const int rq_depth = 2048;

  struct vt_ctx v;
  vt_open_device(&v, c->dev, 1, c->gid_index);
  vt_alloc_buf(&v, VT_BUF_SIZE,
               IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                   IBV_ACCESS_REMOTE_READ);

  struct ibv_cq *conn_cq = ibv_create_cq(v.ctx, VT_CQ_DEPTH, NULL, NULL, 0);
  struct ibv_cq *dgram_scq = ibv_create_cq(v.ctx, VT_CQ_DEPTH, NULL, NULL, 0);
  struct ibv_cq *dgram_rcq = ibv_create_cq(v.ctx, VT_CQ_DEPTH, NULL, NULL, 0);
  VT_CHECK(conn_cq && dgram_scq && dgram_rcq, "cq");

  struct ibv_qp *cqp = qp_on_cqs(&v, conn_cq, conn_cq, conn_t, inl_c, VT_RQ_DEPTH);
  struct ibv_qp *dqp =
      qp_on_cqs(&v, dgram_scq, dgram_rcq, IBV_QPT_UD, inl_d, rq_depth);

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

  struct vt_qp dvq = {.qp = dqp, .remote = dremote};
  vt_create_ud_ah(&v, &dvq);

  memset(v.buf, 0, VT_BUF_SIZE);
  volatile long long *req_slot = (volatile long long *)v.buf;

  /* Staging in MR (ww/ws-echo use malloc+INLINE; --no-inline needs MR).
   *   [0, 64):        req_slot
   *   [64, 4096):     UD RECV staging (client)
   *   [4096, 8192):   server SEND response
   *   [8192, ...):    client WRITE request postlist slots
   */
  int msg = c->size < 8 ? 8 : c->size;
  uint8_t *resp_buf = v.buf + 4096;
  memset(resp_buf, 1, (size_t)msg);
  uint8_t *req_area = v.buf + 8192;
  VT_CHECK(8192 + (size_t)postlist * (size_t)msg <= VT_BUF_SIZE, "ws");

  struct ibv_send_wr wr[128], *bad;
  struct ibv_sge sgl[128];
  struct ibv_wc wc;
  uint64_t nb_tx = 0, echos = 0;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c->duration * 1000000000ull;

  if (c->is_server) {
    printf("fig5 ws server postlist=%d\n", postlist);
    fflush(stdout);
    tcp_ready(fd, 1);
    close(fd);
    long long last_req = 0;
    uint64_t nb_dgram = 0, sig_posted = 0, sig_reaped = 0;
    uint64_t lim = deadline + 3000000000ull;

    while (vt_ns() < lim) {
      /* Block until all prior signaled SENDs complete — keeps SQ from
       * overflowing when catch-up posts a full postlist repeatedly. */
      while (sig_posted > sig_reaped) {
        poll_n(dgram_scq, 1);
        sig_reaped++;
      }

      int nnew = 0;
      while (nnew < postlist) {
        long long cur = *req_slot;
        if (cur == last_req)
          break;
        /* ws-echo catch-up: one SEND per missed sequence step. */
        last_req++;

        memset(&wr[nnew], 0, sizeof(wr[nnew]));
        wr[nnew].opcode = IBV_WR_SEND;
        wr[nnew].num_sge = 1;
        wr[nnew].sg_list = &sgl[nnew];
        wr[nnew].next = NULL;
        int do_sig = (nb_dgram % (uint64_t)c->unsig == 0);
        wr[nnew].send_flags = do_sig ? IBV_SEND_SIGNALED : 0;
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
        if (do_sig)
          sig_posted++;
        nb_dgram++;
        nnew++;
      }
      if (nnew > 0) {
        /* Always signal the last WR of this doorbell so one CQE reclaims SQ. */
        if (!(wr[nnew - 1].send_flags & IBV_SEND_SIGNALED)) {
          wr[nnew - 1].send_flags |= IBV_SEND_SIGNALED;
          sig_posted++;
        }
        VT_CHECK(ibv_post_send(dqp, &wr[0], &bad) == 0, "ud send");
      }
    }
  } else {
    printf("fig5 ws client postlist=%d pipeline=%d\n", postlist, pipeline);
    fflush(stdout);
    tcp_ready(fd, 0);
    close(fd);

    long long req_seq = 0;
    uint64_t rolling = 0;

    while (1) {
      if ((rolling & 0xff) == 0 && vt_ns() >= deadline)
        break;

      /*
       * Reap prior signaled WRITE completions BEFORE building the next
       * postlist. With -Q 1 every WR is signaled → poll a full postlist.
       */
      if (nb_tx > 0) {
        if (c->unsig <= 1)
          poll_n(conn_cq, postlist);
        else if ((nb_tx - (uint64_t)postlist) % (uint64_t)c->unsig == 0)
          poll_n(conn_cq, 1);
      }

      for (int i = 0; i < postlist; i++) {
        if ((int)nb_tx >= pipeline) {
          while (ibv_poll_cq(dgram_rcq, 1, &wc) == 0) {
            if ((rolling & 0xff) == 0 && vt_ns() >= deadline)
              goto ws_done;
          }
          if (wc.status != IBV_WC_SUCCESS)
            goto ws_done;
          echos++;
        }
        /* RECV staging: after req_slot, before send staging. */
        post_recv(dqp, &v, (uintptr_t)(v.buf + 64), 2048, (uint64_t)i);

        memset(&wr[i], 0, sizeof(wr[i]));
        wr[i].opcode = IBV_WR_RDMA_WRITE;
        wr[i].num_sge = 1;
        wr[i].next = (i == postlist - 1) ? NULL : &wr[i + 1];
        wr[i].sg_list = &sgl[i];
        wr[i].send_flags =
            (nb_tx % (uint64_t)c->unsig == 0) ? IBV_SEND_SIGNALED : 0;
        if (c->use_inline && c->size <= inl_c)
          wr[i].send_flags |= IBV_SEND_INLINE;

        req_seq++;
        long long *slot = (long long *)(req_area + (size_t)i * (size_t)msg);
        slot[0] = req_seq;
        sgl[i].addr = (uintptr_t)slot;
        sgl[i].length = (uint32_t)msg;
        sgl[i].lkey = v.mr->lkey;
        wr[i].wr.rdma.remote_addr = cremote.addr;
        wr[i].wr.rdma.rkey = cremote.rkey;
        nb_tx++;
        rolling++;
      }
      VT_CHECK(ibv_post_send(cqp, &wr[0], &bad) == 0, "write");
    }
  ws_done:
    {
      uint64_t drain = vt_ns() + 500000000ull;
      while (echos < nb_tx && vt_ns() < drain) {
        if (ibv_poll_cq(dgram_rcq, 1, &wc) == 1 &&
            wc.status == IBV_WC_SUCCESS)
          echos++;
      }
      double sec = (vt_ns() - t0) / 1e9;
      if (sec < 1e-6)
        sec = 1e-6;
      printf("fig5 ws ECHO: %.2f Mops (postlist=%d)\n", echos / sec / 1e6,
             postlist);
      fflush(stdout);
    }
  }

  if (dvq.ah)
    ibv_destroy_ah(dvq.ah);
  ibv_destroy_qp(cqp);
  ibv_destroy_qp(dqp);
  ibv_destroy_cq(conn_cq);
  ibv_destroy_cq(dgram_scq);
  ibv_destroy_cq(dgram_rcq);
  vt_close_device(&v);
}

/*
 * SEND / SEND — follows ss-echo/main.cc:
 *   separate send CQ + recv CQ (sharing one CQ caused 0 Mops).
 *   Client: postlist RECV+SEND, poll 1 SEND, poll postlist RECVs.
 *   Server: poll RECVs, repost, reply postlist SENDs.
 * --rc uses RC SEND with the same CQ separation; else UD.
 */
static void run_ss(struct cfg *c) {
  int use_ud = c->use_uc;
  enum ibv_qp_type qpt = use_ud ? IBV_QPT_UD : IBV_QPT_RC;
  int postlist = c->window;
  int inl = vt_inline_grant(c->size, c->use_inline, use_ud);
  const int rq_depth = 2048;

  struct vt_ctx v;
  vt_open_device(&v, c->dev, 1, c->gid_index);
  vt_alloc_buf(&v, VT_BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);

  struct ibv_cq *scq = ibv_create_cq(v.ctx, VT_CQ_DEPTH, NULL, NULL, 0);
  struct ibv_cq *rcq = ibv_create_cq(v.ctx, VT_CQ_DEPTH, NULL, NULL, 0);
  VT_CHECK(scq && rcq, "cq");
  /* Detach default cq unused. */
  struct ibv_qp *qp = qp_on_cqs(&v, scq, rcq, qpt, inl, rq_depth);

  uint32_t psn = (uint32_t)(vt_ns() & 0xffffff);
  struct vt_endpoint local, remote;
  vt_fill_local_ep(&v, qp, psn, &local);
  vt_qp_to_init(qp, v.port);
  int fd = c->is_server ? vt_tcp_listen(c->ip, c->port)
                        : vt_tcp_connect(c->ip, c->port);
  vt_tcp_exchange(fd, &local, &remote);
  vt_qp_to_rtr(&v, qp, &remote, qpt);
  vt_qp_to_rts(qp, psn);

  struct vt_qp vq = {.qp = qp, .remote = remote};
  if (use_ud)
    vt_create_ud_ah(&v, &vq);

  uint8_t *payload = v.buf + (VT_BUF_SIZE / 2);
  memset(payload, 1, (size_t)c->size);

  uint32_t recv_len =
      use_ud ? (uint32_t)c->size + 40 : (uint32_t)c->size; /* GRH for UD */

  /* Server pre-fills RQ (ss-echo). Client posts RECVs in the request loop. */
  if (c->is_server) {
    for (int i = 0; i < rq_depth; i++)
      post_recv(qp, &v, (uintptr_t)v.buf, recv_len, (uint64_t)i);
  }

  tcp_ready(fd, c->is_server);
  close(fd);

  struct ibv_send_wr wr[128], *bad;
  struct ibv_sge sgl[128];
  struct ibv_wc wc[128];
  uint64_t nb_tx = 0, echos = 0, rolling = 0;
  uint64_t sig_posted = 0, sig_reaped = 0;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c->duration * 1000000000ull;

  if (c->is_server) {
    printf("fig5 ss server qpt=%s postlist=%d\n", use_ud ? "UD" : "RC",
           postlist);
    fflush(stdout);
    uint64_t lim = deadline + 3000000000ull;

    while (vt_ns() < lim) {
      int n = ibv_poll_cq(rcq, postlist, wc);
      if (n <= 0)
        continue;
      for (int i = 0; i < n; i++) {
        if (wc[i].status != IBV_WC_SUCCESS) {
          fprintf(stderr, "recv %s\n", ibv_wc_status_str(wc[i].status));
          goto ss_done;
        }
        post_recv(qp, &v, (uintptr_t)v.buf, recv_len, wc[i].wr_id);
      }
      /* Reap prior SEND CQEs before posting more (safe for -Q 1). */
      while (sig_posted > sig_reaped) {
        struct ibv_wc swc;
        if (ibv_poll_cq(scq, 1, &swc) == 0)
          break;
        if (swc.status != IBV_WC_SUCCESS) {
          fprintf(stderr, "send %s\n", ibv_wc_status_str(swc.status));
          goto ss_done;
        }
        sig_reaped++;
      }
      for (int i = 0; i < n; i++) {
        memset(&wr[i], 0, sizeof(wr[i]));
        wr[i].opcode = IBV_WR_SEND;
        wr[i].num_sge = 1;
        wr[i].next = (i == n - 1) ? NULL : &wr[i + 1];
        wr[i].sg_list = &sgl[i];
        int do_sig = (c->unsig <= 1) ? (i == n - 1)
                                     : (nb_tx % (uint64_t)c->unsig == 0);
        wr[i].send_flags = do_sig ? IBV_SEND_SIGNALED : 0;
        if (c->use_inline && c->size <= inl)
          wr[i].send_flags |= IBV_SEND_INLINE;
        if (use_ud) {
          wr[i].wr.ud.ah = vq.ah;
          wr[i].wr.ud.remote_qpn = remote.qpn;
          wr[i].wr.ud.remote_qkey = 0x11111111;
        }
        sgl[i].addr = (uintptr_t)payload;
        sgl[i].length = (uint32_t)c->size;
        sgl[i].lkey = v.mr->lkey;
        if (do_sig)
          sig_posted++;
        nb_tx++;
      }
      VT_CHECK(ibv_post_send(qp, &wr[0], &bad) == 0, "send");
    }
  } else {
    printf("fig5 ss client qpt=%s postlist=%d\n", use_ud ? "UD" : "RC",
           postlist);
    fflush(stdout);

    while (1) {
      if ((rolling & 0xff) == 0 && vt_ns() >= deadline)
        break;

      for (int i = 0; i < postlist; i++) {
        post_recv(qp, &v, (uintptr_t)v.buf, recv_len, (uint64_t)i);

        memset(&wr[i], 0, sizeof(wr[i]));
        wr[i].opcode = IBV_WR_SEND;
        wr[i].num_sge = 1;
        wr[i].next = (i == postlist - 1) ? NULL : &wr[i + 1];
        wr[i].sg_list = &sgl[i];
        /* Signal last SEND so CQE covers the whole postlist. */
        wr[i].send_flags = (i == postlist - 1) ? IBV_SEND_SIGNALED : 0;
        if (c->use_inline && c->size <= inl)
          wr[i].send_flags |= IBV_SEND_INLINE;
        if (use_ud) {
          wr[i].wr.ud.ah = vq.ah;
          wr[i].wr.ud.remote_qpn = remote.qpn;
          wr[i].wr.ud.remote_qkey = 0x11111111;
        }
        sgl[i].addr = (uintptr_t)payload;
        sgl[i].length = (uint32_t)c->size;
        sgl[i].lkey = v.mr->lkey;
        rolling++;
      }
      VT_CHECK(ibv_post_send(qp, &wr[0], &bad) == 0, "send");
      poll_n(scq, 1);        /* SEND completion for last WR */
      poll_n(rcq, postlist); /* all RECVs */
      echos += (uint64_t)postlist;
      nb_tx += (uint64_t)postlist;
    }
    double sec = (vt_ns() - t0) / 1e9;
    if (sec < 1e-6)
      sec = 1e-6;
    printf("fig5 ss ECHO: %.2f Mops\n", echos / sec / 1e6);
    fflush(stdout);
  }

ss_done:
  if (vq.ah)
    ibv_destroy_ah(vq.ah);
  ibv_destroy_qp(qp);
  ibv_destroy_cq(scq);
  ibv_destroy_cq(rcq);
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
