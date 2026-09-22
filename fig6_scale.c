/*
 * fig6_scale.c — HERD Sec.3 Fig.6: UC WRITE / UD SEND vs all-to-all size.
 *
 * Paper Sec.3.3 / Fig.6 caption: inlined + unsignaled; x-axis = N
 * (#client procs = #server procs). All-to-all ⇒ N² connected QPs at RNICS.
 *
 * -q is always paper N. Local QP counts:
 *   Out-WRITE: both sides N² (requester cache stress)
 *   In-WRITE:  requester N, responder N² (only N connected to peer;
 *              remaining responder QPs self-paired to RTS so contexts exist)
 *   Out-SEND:  sender 1 UD QP + N AHs; passive N UD QPs
 *
 * Templates: sender-scalability (WRITE), ud-sender (SEND).
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
  int nqp; /* paper N */
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
          "out-send-ud] [-q PAPER_N] [-t POSTLIST] [-l SIZE] [-Q UNSIG] [-D SEC] "
          "[--no-inline] [--rc]\n"
          "  -q : paper N (#procs). QP counts derived per mode (see file header).\n"
          "  -t : postlist (paper Fig.6 uses 1 / nearly-unsignaled).\n",
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
  c->postlist = 1;
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
    fprintf(stderr, "paper N (-q) must be 1..%d\n", VT_MAX_QPS);
    exit(1);
  }
  if (c->unsig < 1)
    c->unsig = 1;
  if (c->postlist < 1)
    c->postlist = 1;
  if (c->postlist > 64)
    c->postlist = 64;
}

static int clamp_nn(int n) {
  int q = n * n;
  if (q > VT_MAX_QPS)
    q = VT_MAX_QPS;
  return q;
}

static struct ibv_qp *create_qp(struct vt_ctx *v, struct ibv_cq *scq,
                                struct ibv_cq *rcq, enum ibv_qp_type type,
                                int max_inline, int sq_depth, int rq_depth) {
  struct ibv_qp_init_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.send_cq = scq;
  attr.recv_cq = rcq;
  attr.qp_type = type;
  attr.cap.max_send_wr = (uint32_t)sq_depth;
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

static struct ibv_ah *make_ah(struct vt_ctx *v,
                              const struct vt_endpoint *remote) {
  struct ibv_ah_attr ah_attr;
  memset(&ah_attr, 0, sizeof(ah_attr));
  ah_attr.port_num = (uint8_t)v->port;
  ah_attr.sl = 0;
  ah_attr.src_path_bits = 0;
  if (v->is_roce || remote->is_roce) {
    ah_attr.is_global = 1;
    ah_attr.dlid = 0;
    memcpy(ah_attr.grh.dgid.raw, remote->gid, 16);
    ah_attr.grh.sgid_index = (uint8_t)v->gid_index;
    ah_attr.grh.hop_limit = 1;
  } else {
    ah_attr.is_global = 0;
    ah_attr.dlid = (uint16_t)remote->lid;
  }
  struct ibv_ah *ah = ibv_create_ah(v->pd, &ah_attr);
  VT_CHECK(ah, "ibv_create_ah");
  return ah;
}

int main(int argc, char **argv) {
  struct cfg c;
  parse(argc, argv, &c);

  const int is_ud = (c.mode == FIG6_OUT_SEND_UD);
  const int is_in = (c.mode == FIG6_IN_WRITE);
  const int sender = i_am_sender(&c);
  const int paper_n = c.nqp;
  const int nn = clamp_nn(paper_n);

  int n_local, n_exch, n_active, n_ah;
  if (is_ud) {
    n_ah = paper_n;
    n_local = sender ? 1 : paper_n;
    n_exch = paper_n;
    n_active = 1;
  } else if (is_in) {
    n_local = sender ? paper_n : nn;
    n_exch = paper_n;
    n_active = paper_n;
    n_ah = 0;
  } else {
    n_local = nn;
    n_exch = nn;
    n_active = nn;
    n_ah = 0;
  }

  enum ibv_qp_type qpt =
      is_ud ? IBV_QPT_UD : (c.use_uc ? IBV_QPT_UC : IBV_QPT_RC);
  const int inl_cap = vt_inline_grant(c.size, c.use_inline, is_ud);
  const int sq_depth = 1024;
  const int rq_depth = is_ud ? 512 : 16;

  int unsig = c.unsig;
  if (unsig > 1 && unsig < 2 * c.postlist)
    unsig = 2 * c.postlist;
  if (unsig > sq_depth / 2)
    unsig = sq_depth / 2;

  struct vt_ctx v;
  vt_open_device(&v, c.dev, 1, c.gid_index);
  vt_alloc_buf(&v, VT_BUF_SIZE,
               IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                   IBV_ACCESS_REMOTE_READ);

  struct ibv_cq *scqs[VT_MAX_QPS];
  struct ibv_cq *rcqs[VT_MAX_QPS];
  struct ibv_qp *qps[VT_MAX_QPS];
  struct vt_endpoint locals[VT_MAX_QPS], remotes[VT_MAX_QPS];
  struct ibv_ah *ahs[VT_MAX_QPS];
  uint32_t psns[VT_MAX_QPS];
  memset(ahs, 0, sizeof(ahs));
  memset(scqs, 0, sizeof(scqs));
  memset(rcqs, 0, sizeof(rcqs));

  for (int i = 0; i < n_local; i++) {
    scqs[i] = ibv_create_cq(v.ctx, 4096, NULL, NULL, 0);
    VT_CHECK(scqs[i], "scq");
    if (is_ud) {
      rcqs[i] = ibv_create_cq(v.ctx, 4096, NULL, NULL, 0);
      VT_CHECK(rcqs[i], "rcq");
    } else {
      rcqs[i] = scqs[i];
    }
    qps[i] = create_qp(&v, scqs[i], rcqs[i], qpt, inl_cap, sq_depth,
                       is_ud ? rq_depth : 16);
    psns[i] = (uint32_t)((vt_ns() + (uint64_t)i * 9973) & 0xffffff);
    vt_fill_local_ep(&v, qps[i], psns[i], &locals[i]);
    locals[i].addr = (uint64_t)(uintptr_t)(v.buf + (size_t)i * 4096);
    vt_qp_to_init(qps[i], v.port);
  }

  int fd = c.is_server ? vt_tcp_listen(c.ip, c.port)
                       : vt_tcp_connect(c.ip, c.port);

  if (is_ud) {
    for (int i = 0; i < n_exch; i++) {
      if (sender)
        vt_tcp_exchange(fd, &locals[0], &remotes[i]);
      else
        vt_tcp_exchange(fd, &locals[i], &remotes[0]);
    }
  } else {
    for (int i = 0; i < n_exch; i++)
      vt_tcp_exchange(fd, &locals[i], &remotes[i]);
  }
  close(fd);

  for (int i = 0; i < n_exch && i < n_local; i++) {
    const struct vt_endpoint *r = &remotes[i];
    if (is_ud)
      r = sender ? &remotes[0] : &remotes[0];
    vt_qp_to_rtr(&v, qps[i], r, qpt);
    vt_qp_to_rts(qps[i], psns[i]);
  }

  /* In-WRITE responder: self-pair leftover QPs so N² contexts reach RTS. */
  if (is_in && !sender && n_local > n_exch) {
    for (int i = n_exch; i < n_local; i++) {
      int peer = n_exch + ((i - n_exch) ^ 1);
      if (peer >= n_local)
        peer = n_exch;
      if (peer == i)
        peer = (i + 1 < n_local) ? i + 1 : n_exch;
      vt_qp_to_rtr(&v, qps[i], &locals[peer], qpt);
      vt_qp_to_rts(qps[i], psns[i]);
    }
  }

  if (is_ud && sender) {
    for (int i = 0; i < n_ah; i++)
      ahs[i] = make_ah(&v, &remotes[i]);
  }

  if (!sender) {
    printf("fig6 passive paper_N=%d nqp=%d mode=%d\n", paper_n, n_local,
           (int)c.mode);
    fflush(stdout);
    if (is_ud) {
      for (int qi = 0; qi < n_local; qi++) {
        for (int r = 0; r < rq_depth / 2; r++)
          ud_post_recv(qps[qi], &v, (uint64_t)((qi << 16) | r));
      }
      for (;;) {
        for (int qi = 0; qi < n_local; qi++) {
          struct ibv_wc wc[16];
          int n = ibv_poll_cq(rcqs[qi], 16, wc);
          for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
              fprintf(stderr, "wc %s\n", ibv_wc_status_str(wc[i].status));
              exit(1);
            }
            if (wc[i].opcode & IBV_WC_RECV)
              ud_post_recv(qps[qi], &v, wc[i].wr_id);
          }
        }
      }
    }
    pause();
    return 0;
  }

  memset(v.buf, 1, VT_BUF_SIZE);

  const int pl = c.postlist;
  struct ibv_send_wr wr[64], *bad;
  struct ibv_sge sge[64];
  uint64_t nb_tx = 0;
  uint64_t nb_tx_qp[VT_MAX_QPS];
  memset(nb_tx_qp, 0, sizeof(nb_tx_qp));
  uint64_t ops = 0;
  uint64_t seed = 0x12345678abcdefull;

  printf("fig6 sender paper_N=%d nqp=%d ndest=%d size=%d inl=%d postlist=%d "
         "unsig=%d\n",
         paper_n, n_active, is_ud ? n_ah : n_active, c.size, inl_cap, pl,
         unsig);
  fflush(stdout);

  const uint64_t check_every = 65536;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c.duration * 1000000000ull;

  while (1) {
    if ((ops & (check_every - 1)) == 0 && ops > 0) {
      if (vt_ns() >= deadline)
        break;
    }

    if (is_ud) {
      for (int w = 0; w < pl; w++) {
        int cn = (int)(nb_tx % (uint64_t)n_ah);
        memset(&wr[w], 0, sizeof(wr[w]));
        memset(&sge[w], 0, sizeof(sge[w]));
        wr[w].opcode = IBV_WR_SEND;
        wr[w].num_sge = 1;
        wr[w].sg_list = &sge[w];
        wr[w].next = (w == pl - 1) ? NULL : &wr[w + 1];
        wr[w].wr.ud.ah = ahs[cn];
        wr[w].wr.ud.remote_qpn = remotes[cn].qpn;
        wr[w].wr.ud.remote_qkey = 0x11111111;

        int do_sig = (nb_tx % (uint64_t)unsig == 0);
        wr[w].send_flags = do_sig ? IBV_SEND_SIGNALED : 0;
        if (nb_tx >= (uint64_t)unsig &&
            (nb_tx % (uint64_t)unsig == (uint64_t)unsig - 1))
          poll_one(scqs[0]);
        if (c.use_inline && c.size <= inl_cap)
          wr[w].send_flags |= IBV_SEND_INLINE;

        sge[w].addr = (uintptr_t)v.buf;
        sge[w].length = (uint32_t)c.size;
        sge[w].lkey = v.mr->lkey;
        nb_tx++;
      }
      VT_CHECK(ibv_post_send(qps[0], &wr[0], &bad) == 0, "post");
      ops += (uint64_t)pl;
    } else {
      seed ^= seed << 13;
      seed ^= seed >> 7;
      seed ^= seed << 17;
      int qi = (int)(seed % (uint64_t)n_active);

      for (int w = 0; w < pl; w++) {
        memset(&wr[w], 0, sizeof(wr[w]));
        memset(&sge[w], 0, sizeof(sge[w]));
        wr[w].opcode = IBV_WR_RDMA_WRITE;
        wr[w].num_sge = 1;
        wr[w].sg_list = &sge[w];
        wr[w].next = (w == pl - 1) ? NULL : &wr[w + 1];
        wr[w].wr.rdma.remote_addr = remotes[qi].addr;
        wr[w].wr.rdma.rkey = remotes[qi].rkey;

        int do_sig = (nb_tx_qp[qi] % (uint64_t)unsig == 0);
        wr[w].send_flags = do_sig ? IBV_SEND_SIGNALED : 0;
        if (nb_tx_qp[qi] >= (uint64_t)unsig &&
            (nb_tx_qp[qi] % (uint64_t)unsig == (uint64_t)unsig - 1))
          poll_one(scqs[qi]);
        if (c.use_inline && c.size <= inl_cap)
          wr[w].send_flags |= IBV_SEND_INLINE;

        sge[w].addr = (uintptr_t)v.buf;
        sge[w].length = (uint32_t)c.size;
        sge[w].lkey = v.mr->lkey;
        nb_tx_qp[qi]++;
      }
      VT_CHECK(ibv_post_send(qps[qi], &wr[0], &bad) == 0, "post");
      ops += (uint64_t)pl;
    }
  }

  double sec = (vt_ns() - t0) / 1e9;
  if (sec < 1e-6)
    sec = 1e-6;
  if (c.mode == FIG6_IN_WRITE)
    printf("fig6 In-WRITE: %.2f Mops  paper_N=%d nqp=%d size=%d\n",
           ops / sec / 1e6, paper_n, n_active, c.size);
  else if (c.mode == FIG6_OUT_SEND_UD)
    printf("fig6 Out-SEND: %.2f Mops  paper_N=%d ndest=%d size=%d\n",
           ops / sec / 1e6, paper_n, n_ah, c.size);
  else
    printf("fig6 Out-WRITE: %.2f Mops  paper_N=%d nqp=%d size=%d\n",
           ops / sec / 1e6, paper_n, n_active, c.size);

  for (int i = 0; i < n_ah; i++) {
    if (ahs[i])
      ibv_destroy_ah(ahs[i]);
  }
  for (int i = 0; i < n_local; i++) {
    ibv_destroy_qp(qps[i]);
    if (is_ud && rcqs[i] && rcqs[i] != scqs[i])
      ibv_destroy_cq(rcqs[i]);
    ibv_destroy_cq(scqs[i]);
  }
  vt_close_device(&v);
  return 0;
}
