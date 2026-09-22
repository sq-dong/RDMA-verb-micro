/*
 * fig6_scale.c — HERD Sec.3 Fig.6: UC WRITE / UD SEND vs all-to-all size.
 *
 * Paper: N client + N server processes, all-to-all ⇒ N² QPs at RNICS.
 * collect_fig6.sh passes -q N*N and labels the CSV with paper N.
 * Out-SEND-UD keeps 1 QP (datagram one-to-many).
 *
 * -t POSTLIST: post a batch to one randomly chosen QP per doorbell so low-N
 * is not doorbell-capped at ~5 Mops on CX-5 RoCE (paper used CX-3 IB).
 *
 * Template: rdma_bench/sender-scalability/main.cc
 */

#include "common.h"

#include <getopt.h>

enum fig6_mode {
  FIG6_OUT_WRITE = 0,
  FIG6_IN_WRITE,
  FIG6_OUT_SEND_UD,
};

struct cfg {
  int is_server;
  char *dev;
  char *ip;
  uint16_t port;
  int gid_index;
  int size;
  int nqp;
  int unsig;
  int postlist;
  int duration;
  int use_inline;
  int use_uc;
  enum fig6_mode mode;
};

static void usage(const char *a) {
  fprintf(stderr,
          "Usage: %s -s|-c -d DEV -a IP [-p PORT] [-M out-write|in-write|"
          "out-send-ud] [-q NQPS] [-t POSTLIST] [-l SIZE] [-Q UNSIG] [-D SEC] "
          "[--no-inline] [--rc]\n"
          "  -q : #QPs on this NIC (paper all-to-all N ⇒ N*N QPs).\n"
          "  -t : postlist to one randomly chosen QP (doorbell amortize).\n",
          a);
  exit(1);
}

static enum fig6_mode parse_mode(const char *s) {
  if (!strcmp(s, "out-write"))
    return FIG6_OUT_WRITE;
  if (!strcmp(s, "in-write"))
    return FIG6_IN_WRITE;
  if (!strcmp(s, "out-send-ud"))
    return FIG6_OUT_SEND_UD;
  fprintf(stderr, "unknown mode %s\n", s);
  usage("fig6_scale");
  return FIG6_OUT_WRITE;
}

static void parse(int argc, char **argv, struct cfg *c) {
  memset(c, 0, sizeof(*c));
  c->port = 18540;
  c->gid_index = 3;
  c->size = 32;
  c->nqp = 16;
  c->unsig = 4;
  c->postlist = 64;
  c->duration = 5;
  c->use_inline = 1;
  c->use_uc = 1;
  c->is_server = -1;
  c->mode = FIG6_OUT_WRITE;
  static struct option longopts[] = {{"no-inline", no_argument, 0, 1001},
                                     {"rc", no_argument, 0, 1002},
                                     {0, 0, 0, 0}};
  int opt;
  while ((opt = getopt_long(argc, argv, "scd:a:p:x:l:q:t:Q:D:M:h", longopts,
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
    case 'q':
      c->nqp = atoi(optarg);
      break;
    case 't':
      c->postlist = atoi(optarg);
      break;
    case 'Q':
      c->unsig = atoi(optarg);
      break;
    case 'D':
      c->duration = atoi(optarg);
      break;
    case 'M':
      c->mode = parse_mode(optarg);
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
  if (c->nqp < 1 || c->nqp > VT_MAX_QPS) {
    fprintf(stderr, "nqp must be 1..%d\n", VT_MAX_QPS);
    exit(1);
  }
  if (c->unsig < 1)
    c->unsig = 1;
  if (c->postlist < 1)
    c->postlist = 1;
  if (c->postlist > 64)
    c->postlist = 64;
}

static struct ibv_qp *create_qp_on_cq(struct vt_ctx *v, struct ibv_cq *cq,
                                      enum ibv_qp_type type, int max_inline) {
  /* Deep SQ so N=1 can also keep enough outstanding to reach peak rate
   * (paper Fig.6 low-N should match Fig.4 outbound peak). */
  const int sq_depth = 1024;
  const int rq_depth = 512;
  struct ibv_qp_init_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.send_cq = cq;
  attr.recv_cq = cq;
  attr.qp_type = type;
  attr.cap.max_send_wr = sq_depth;
  attr.cap.max_recv_wr = rq_depth;
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

static void ud_post_recv(struct ibv_qp *qp, struct vt_ctx *v, uint64_t id) {
  struct ibv_sge sge = {.addr = (uintptr_t)v->buf,
                        .length = (uint32_t)v->buf_size,
                        .lkey = v->mr->lkey};
  struct ibv_recv_wr wr = {.wr_id = id, .sg_list = &sge, .num_sge = 1};
  struct ibv_recv_wr *bad = NULL;
  VT_CHECK(ibv_post_recv(qp, &wr, &bad) == 0, "post_recv");
}

static void poll_one(struct ibv_cq *cq) {
  struct ibv_wc wc;
  while (ibv_poll_cq(cq, 1, &wc) == 0)
    ;
  if (wc.status != IBV_WC_SUCCESS) {
    fprintf(stderr, "wc %s\n", ibv_wc_status_str(wc.status));
    exit(1);
  }
}

static int i_am_sender(const struct cfg *c) {
  switch (c->mode) {
  case FIG6_OUT_WRITE:
  case FIG6_OUT_SEND_UD:
    return c->is_server;
  case FIG6_IN_WRITE:
    return !c->is_server;
  }
  return 0;
}

int main(int argc, char **argv) {
  struct cfg c;
  parse(argc, argv, &c);

  const int is_ud = (c.mode == FIG6_OUT_SEND_UD);
  const int n_local = is_ud ? 1 : c.nqp;
  const int n_exch = n_local;

  enum ibv_qp_type qpt =
      is_ud ? IBV_QPT_UD : (c.use_uc ? IBV_QPT_UC : IBV_QPT_RC);
  const int inl_cap = vt_inline_grant(c.size, c.use_inline, is_ud);

  struct vt_ctx v;
  vt_open_device(&v, c.dev, 1, c.gid_index);
  vt_alloc_buf(&v, VT_BUF_SIZE,
               IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                   IBV_ACCESS_REMOTE_READ);

  struct ibv_cq *cqs[VT_MAX_QPS];
  struct ibv_qp *qps[VT_MAX_QPS];
  struct vt_endpoint locals[VT_MAX_QPS], remotes[VT_MAX_QPS];
  struct ibv_ah *ahs[VT_MAX_QPS];
  uint32_t psns[VT_MAX_QPS];
  memset(ahs, 0, sizeof(ahs));
  memset(cqs, 0, sizeof(cqs));

  for (int i = 0; i < n_local; i++) {
    /* CQ must hold many signaled completions with deep SQ + unsig batch. */
    cqs[i] = ibv_create_cq(v.ctx, 4096, NULL, NULL, 0);
    VT_CHECK(cqs[i], "ibv_create_cq");
    qps[i] = create_qp_on_cq(&v, cqs[i], qpt, inl_cap);
    psns[i] = (uint32_t)((vt_ns() + (uint64_t)i * 9973) & 0xffffff);
    vt_fill_local_ep(&v, qps[i], psns[i], &locals[i]);
    locals[i].addr = (uint64_t)(uintptr_t)(v.buf + (size_t)i * 4096);
    vt_qp_to_init(qps[i], v.port);
  }

  int fd = c.is_server ? vt_tcp_listen(c.ip, c.port)
                       : vt_tcp_connect(c.ip, c.port);
  for (int i = 0; i < n_exch; i++)
    vt_tcp_exchange(fd, &locals[i], &remotes[i]);
  close(fd);

  for (int i = 0; i < n_local; i++) {
    vt_qp_to_rtr(&v, qps[i], &remotes[i], qpt);
    vt_qp_to_rts(qps[i], psns[i]);
    if (is_ud) {
      struct vt_qp vq = {.qp = qps[i], .remote = remotes[i]};
      vt_create_ud_ah(&v, &vq);
      ahs[i] = vq.ah;
    }
  }

  if (!i_am_sender(&c)) {
    printf("fig6 passive nqp=%d mode=%d\n", n_local, (int)c.mode);
    fflush(stdout);
    if (is_ud) {
      for (int r = 0; r < VT_RQ_DEPTH / 2; r++)
        ud_post_recv(qps[0], &v, (uint64_t)r);
      for (;;) {
        struct ibv_wc wc[16];
        int n = ibv_poll_cq(cqs[0], 16, wc);
        for (int i = 0; i < n; i++) {
          if (wc[i].status != IBV_WC_SUCCESS) {
            fprintf(stderr, "wc %s\n", ibv_wc_status_str(wc[i].status));
            exit(1);
          }
          if (wc[i].opcode & IBV_WC_RECV)
            ud_post_recv(qps[0], &v, wc[i].wr_id);
        }
      }
    }
    pause();
    return 0;
  }

  memset(v.buf, 1, VT_BUF_SIZE);
  /*
   * Paper Fig.6 / sender-scalability posts one WR at a time. On CX-5 RoCE that
   * doorbell-limits us to ~5 Mops for EVERY curve, hiding QP-cache thrashing.
   * We post a postlist to one randomly chosen QP per doorbell: low-N stays
   * near fig4 rates; high-N still switches QPs and stresses the NIC cache.
   */
  const int pl = c.postlist;
  struct ibv_send_wr wr[64], *bad;
  struct ibv_sge sge[64];
  uint64_t nb_tx[VT_MAX_QPS];
  uint64_t sig_posted[VT_MAX_QPS], sig_reaped[VT_MAX_QPS];
  memset(nb_tx, 0, sizeof(nb_tx));
  memset(sig_posted, 0, sizeof(sig_posted));
  memset(sig_reaped, 0, sizeof(sig_reaped));
  uint64_t ops = 0;
  uint64_t seed = 0x12345678abcdefull;

  printf("fig6 sender nqp=%d size=%d inl=%d postlist=%d\n", n_local, c.size,
         inl_cap, pl);
  fflush(stdout);

  const uint64_t check_every = 65536;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c.duration * 1000000000ull;

  while (1) {
    if ((ops & (check_every - 1)) == 0 && ops > 0) {
      if (vt_ns() >= deadline)
        break;
    }

    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    int qi = is_ud ? 0 : (int)(seed % (uint64_t)n_local);

    /*
     * Keep several signaled WRs in flight (do not drain to zero every
     * batch).  Otherwise N=1 is capped far below multi-QP runs that
     * naturally keep work queued across QPs.
     */
    const uint64_t max_sig_inflight = 8;
    while (sig_posted[qi] - sig_reaped[qi] >= max_sig_inflight) {
      poll_one(cqs[qi]);
      sig_reaped[qi]++;
    }

    for (int w = 0; w < pl; w++) {
      memset(&wr[w], 0, sizeof(wr[w]));
      memset(&sge[w], 0, sizeof(sge[w]));
      wr[w].num_sge = 1;
      wr[w].sg_list = &sge[w];
      wr[w].next = (w == pl - 1) ? NULL : &wr[w + 1];
      int do_sig = (nb_tx[qi] % (uint64_t)c.unsig == 0);
      wr[w].send_flags = do_sig ? IBV_SEND_SIGNALED : 0;
      if (c.use_inline && c.size <= inl_cap)
        wr[w].send_flags |= IBV_SEND_INLINE;
      sge[w].addr = (uintptr_t)v.buf;
      sge[w].length = (uint32_t)c.size;
      sge[w].lkey = v.mr->lkey;
      if (is_ud) {
        wr[w].opcode = IBV_WR_SEND;
        wr[w].wr.ud.ah = ahs[0];
        wr[w].wr.ud.remote_qpn = remotes[0].qpn;
        wr[w].wr.ud.remote_qkey = 0x11111111;
      } else {
        wr[w].opcode = IBV_WR_RDMA_WRITE;
        wr[w].wr.rdma.remote_addr = remotes[qi].addr;
        wr[w].wr.rdma.rkey = remotes[qi].rkey;
      }
      if (do_sig)
        sig_posted[qi]++;
      nb_tx[qi]++;
    }
    VT_CHECK(ibv_post_send(qps[qi], &wr[0], &bad) == 0, "post");
    ops += (uint64_t)pl;
  }

  double sec = (vt_ns() - t0) / 1e9;
  if (sec < 1e-6)
    sec = 1e-6;
  if (c.mode == FIG6_IN_WRITE)
    printf("fig6 In-WRITE: %.2f Mops  nqp=%d size=%d\n", ops / sec / 1e6,
           n_local, c.size);
  else if (c.mode == FIG6_OUT_SEND_UD)
    printf("fig6 Out-SEND: %.2f Mops  nqp=%d size=%d\n", ops / sec / 1e6,
           n_local, c.size);
  else
    printf("fig6 Out-WRITE: %.2f Mops  nqp=%d size=%d\n", ops / sec / 1e6,
           n_local, c.size);

  for (int i = 0; i < n_local; i++) {
    if (ahs[i])
      ibv_destroy_ah(ahs[i]);
    ibv_destroy_qp(qps[i]);
    ibv_destroy_cq(cqs[i]);
  }
  vt_close_device(&v);
  return 0;
}
