/*
 * fig4_outbound.c — HERD Sec.3 Fig.4: outbound verb throughput.
 *
 * Templates:
 *   WRITE/READ: rdma_bench/rw-tput-sender/main.cc
 *   UD SEND:    rdma_bench/ud-sender/main.cc
 *
 * Paper Fig.4a: N processes on MS, each with one QP to one client.
 * We approximate that with -q N QPs in one process (same credit pools /
 * outstanding READs as N threads). On CX-5 a single QP's max_rd_atomic=16
 * leaves READ well below line rate while WRITE saturates — multi-QP lets
 * large-payload READ reach BW so WRITE ≤ READ as in the paper.
 *
 * -R: requester is the TCP server (run on MS).
 *
 * Modes (-m):
 *   write_uc   UC WRITE (+inline by default)
 *   write_rc   RC WRITE
 *   read       RC READ
 *   send_ud    UD SEND (+inline) — needs peer pre-posted RECVs
 */

#include "common.h"

#include <getopt.h>

#define FIG4_MAX_QPS 64

struct cfg {
  int is_server; /* TCP role */
  int requester_is_server;
  char *dev;
  char *ip;
  uint16_t port;
  int gid_index;
  int size;
  int postlist;
  int unsig;
  int duration;
  int use_inline;
  int nqp;
  char mode[32];
};

static void usage(const char *a) {
  fprintf(stderr,
          "Usage: %s -s|-c -d DEV -a IP [-p PORT] [-R] [-l SIZE] [-t POST] "
          "[-Q UNSIG] [-q NQP] [-D SEC] [-m write_uc|write_rc|read|send_ud] "
          "[--no-inline]\n",
          a);
  exit(1);
}

static void parse(int argc, char **argv, struct cfg *c) {
  memset(c, 0, sizeof(*c));
  c->port = 18520;
  c->gid_index = 3;
  c->size = 32;
  c->postlist = 64;
  c->unsig = 64;
  c->duration = 5;
  c->use_inline = 1;
  c->nqp = 1;
  c->is_server = -1;
  strcpy(c->mode, "write_uc");
  static struct option longopts[] = {{"no-inline", no_argument, 0, 1001},
                                     {0, 0, 0, 0}};
  int opt;
  while ((opt = getopt_long(argc, argv, "scRd:a:p:x:l:t:Q:q:D:m:h", longopts,
                            NULL)) != -1) {
    switch (opt) {
    case 's':
      c->is_server = 1;
      break;
    case 'c':
      c->is_server = 0;
      break;
    case 'R':
      c->requester_is_server = 1;
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
    case 't':
      c->postlist = atoi(optarg);
      break;
    case 'Q':
      c->unsig = atoi(optarg);
      break;
    case 'q':
      c->nqp = atoi(optarg);
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
    default:
      usage(argv[0]);
    }
  }
  if (c->is_server < 0 || !c->dev || !c->ip)
    usage(argv[0]);
  if (c->nqp < 1)
    c->nqp = 1;
  if (c->nqp > FIG4_MAX_QPS)
    c->nqp = FIG4_MAX_QPS;
}

static void post_recvs(struct ibv_qp *qp, struct vt_ctx *v, int n) {
  for (int i = 0; i < n; i++) {
    struct ibv_sge sge = {.addr = (uintptr_t)v->buf,
                          .length = (uint32_t)v->buf_size,
                          .lkey = v->mr->lkey};
    struct ibv_recv_wr wr = {.wr_id = (uint64_t)i, .sg_list = &sge, .num_sge = 1};
    struct ibv_recv_wr *bad = NULL;
    VT_CHECK(ibv_post_recv(qp, &wr, &bad) == 0, "post_recv");
  }
}

static struct ibv_qp *create_qp_cq(struct vt_ctx *v, struct ibv_cq *cq,
                                   enum ibv_qp_type qpt, int inl_cap) {
  struct ibv_qp_init_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.send_cq = cq;
  attr.recv_cq = cq;
  attr.qp_type = qpt;
  attr.cap.max_send_wr = VT_SQ_DEPTH;
  attr.cap.max_recv_wr = VT_RQ_DEPTH;
  attr.cap.max_send_sge = 1;
  attr.cap.max_recv_sge = 1;
  attr.cap.max_inline_data = (uint32_t)inl_cap;
  struct ibv_qp *qp = ibv_create_qp(v->pd, &attr);
  VT_CHECK(qp, "ibv_create_qp");
  return qp;
}

int main(int argc, char **argv) {
  struct cfg c;
  parse(argc, argv, &c);

  int is_ud = (strcmp(c.mode, "send_ud") == 0);
  int do_read = (strcmp(c.mode, "read") == 0);
  int use_uc = (strcmp(c.mode, "write_uc") == 0);
  enum ibv_qp_type qpt =
      is_ud ? IBV_QPT_UD : (do_read || !use_uc) ? IBV_QPT_RC : IBV_QPT_UC;
  /* UD SEND stays 1 QP (one AH); multi-QP is for RC/UC WRITE/READ. */
  int nqp = is_ud ? 1 : c.nqp;

  struct vt_ctx v;
  vt_open_device(&v, c.dev, 1, c.gid_index);
  int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
               IBV_ACCESS_REMOTE_READ;
  vt_alloc_buf(&v, VT_BUF_SIZE, access);
  int want_inl = c.use_inline && !do_read;
  int inl_cap = vt_inline_grant(c.size, want_inl, is_ud);

  struct ibv_cq *cqs[FIG4_MAX_QPS];
  struct ibv_qp *qps[FIG4_MAX_QPS];
  struct vt_endpoint locals[FIG4_MAX_QPS], remotes[FIG4_MAX_QPS];
  struct ibv_ah *ahs[FIG4_MAX_QPS];
  uint32_t psns[FIG4_MAX_QPS];
  memset(ahs, 0, sizeof(ahs));

  for (int i = 0; i < nqp; i++) {
    cqs[i] = ibv_create_cq(v.ctx, VT_CQ_DEPTH, NULL, NULL, 0);
    VT_CHECK(cqs[i], "ibv_create_cq");
    qps[i] = create_qp_cq(&v, cqs[i], qpt, inl_cap);
    psns[i] = (uint32_t)((vt_ns() + (uint64_t)i * 7919) & 0xffffff);
    vt_fill_local_ep(&v, qps[i], psns[i], &locals[i]);
    /* Give each QP a distinct MR window so multi-QP READs don't contend. */
    locals[i].addr = (uint64_t)(uintptr_t)v.buf +
                     (uint64_t)i * (VT_BUF_SIZE / (size_t)nqp);
    vt_qp_to_init(qps[i], v.port);
  }

  int fd = c.is_server ? vt_tcp_listen(c.ip, c.port)
                       : vt_tcp_connect(c.ip, c.port);
  for (int i = 0; i < nqp; i++)
    vt_tcp_exchange(fd, &locals[i], &remotes[i]);
  close(fd);

  for (int i = 0; i < nqp; i++) {
    vt_qp_to_rtr(&v, qps[i], &remotes[i], qpt);
    vt_qp_to_rts(qps[i], psns[i]);
    if (is_ud) {
      struct vt_qp vq;
      memset(&vq, 0, sizeof(vq));
      vq.qp = qps[i];
      vq.type = qpt;
      vq.remote = remotes[i];
      vt_create_ud_ah(&v, &vq);
      ahs[i] = vq.ah;
    }
  }

  int i_am_requester =
      (c.requester_is_server && c.is_server) ||
      (!c.requester_is_server && !c.is_server);

  if (!i_am_requester) {
    printf("fig4 passive side mode=%s nqp=%d\n", c.mode, nqp);
    if (is_ud) {
      post_recvs(qps[0], &v, VT_RQ_DEPTH / 2);
      for (;;) {
        struct ibv_wc wc[16];
        int n = ibv_poll_cq(cqs[0], 16, wc);
        for (int i = 0; i < n; i++) {
          if (wc[i].status != IBV_WC_SUCCESS) {
            fprintf(stderr, "wc %s\n", ibv_wc_status_str(wc[i].status));
            exit(1);
          }
          if (wc[i].opcode & IBV_WC_RECV)
            post_recvs(qps[0], &v, 1);
        }
      }
    }
    pause();
    return 0;
  }

  printf("fig4 requester mode=%s size=%d post=%d unsig=%d nqp=%d\n", c.mode,
         c.size, c.postlist, c.unsig, nqp);

  int stride = VT_CACHELINE;
  while (stride < c.size)
    stride += VT_CACHELINE;
  size_t per_qp = VT_BUF_SIZE / (size_t)nqp;
  VT_CHECK((size_t)stride * (size_t)c.postlist + (size_t)c.size <= per_qp,
           "postlist window");

  struct ibv_send_wr wr[64], *bad;
  struct ibv_sge sgl[64];
  VT_CHECK(c.postlist <= 64, "postlist");
  memset(v.buf, 1, VT_BUF_SIZE);

  uint64_t nb_tx[FIG4_MAX_QPS];
  memset(nb_tx, 0, sizeof(nb_tx));
  uint64_t ops = 0;
  int qp_i = 0;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c.duration * 1000000000ull;

  while (vt_ns() < deadline) {
    struct ibv_qp *qp = qps[qp_i];
    struct ibv_cq *cq = cqs[qp_i];
    uint64_t base = (uint64_t)(uintptr_t)v.buf + (uint64_t)qp_i * per_qp;

    for (int w = 0; w < c.postlist; w++) {
      memset(&wr[w], 0, sizeof(wr[w]));
      memset(&sgl[w], 0, sizeof(sgl[w]));
      wr[w].num_sge = 1;
      wr[w].next = (w == c.postlist - 1) ? NULL : &wr[w + 1];
      wr[w].sg_list = &sgl[w];
      wr[w].send_flags =
          vt_should_signal(nb_tx[qp_i], c.unsig) ? IBV_SEND_SIGNALED : 0;
      if (vt_should_signal(nb_tx[qp_i], c.unsig) && nb_tx[qp_i] > 0)
        vt_poll_cq(cq, 1);
      if (c.use_inline && !do_read && c.size <= inl_cap)
        wr[w].send_flags |= IBV_SEND_INLINE;

      sgl[w].addr = base + (uint64_t)(stride * w);
      sgl[w].length = (uint32_t)c.size;
      sgl[w].lkey = v.mr->lkey;

      if (is_ud) {
        wr[w].opcode = IBV_WR_SEND;
        wr[w].wr.ud.ah = ahs[0];
        wr[w].wr.ud.remote_qpn = remotes[0].qpn;
        wr[w].wr.ud.remote_qkey = 0x11111111;
      } else {
        wr[w].opcode = do_read ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE;
        wr[w].wr.rdma.remote_addr =
            remotes[qp_i].addr + (uint64_t)(stride * w);
        wr[w].wr.rdma.rkey = remotes[qp_i].rkey;
      }
      nb_tx[qp_i]++;
    }
    VT_CHECK(ibv_post_send(qp, &wr[0], &bad) == 0, "post_send");
    ops += (uint64_t)c.postlist;
    qp_i++;
    if (qp_i == nqp)
      qp_i = 0;
  }

  double sec = (vt_ns() - t0) / 1e9;
  printf("fig4 outbound: %.2f Mops  mode=%s size=%d nqp=%d\n", ops / sec / 1e6,
         c.mode, c.size, nqp);

  for (int i = 0; i < nqp; i++) {
    if (ahs[i])
      ibv_destroy_ah(ahs[i]);
    ibv_destroy_qp(qps[i]);
    ibv_destroy_cq(cqs[i]);
  }
  /* Device CQ in vt_ctx unused for data path; still tear down via close. */
  vt_close_device(&v);
  return 0;
}
