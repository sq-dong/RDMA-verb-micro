/*
 * fig6_scale.c — HERD Sec.3 Fig.6: all-to-all QP scaling (32 B, inlined).
 *
 * Paper x-axis N = number of client/server processes. All-to-all gives N*N
 * active QPs per machine. collect_fig6.sh passes -q N*N (capped at VT_MAX_QPS).
 *
 * Modes (-M):
 *   out-write   server -> client UC WRITE (drops with QP count)
 *   in-write    client -> server UC WRITE (stays high)
 *   out-send-ud server -> client UD SEND (stays high)
 *
 * Template: rdma_bench/sender-scalability/main.cc (+ ws-echo UD for send path)
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
  int duration;
  int use_inline;
  int use_uc;
  enum fig6_mode mode;
};

static void usage(const char *a) {
  fprintf(stderr,
          "Usage: %s -s|-c -d DEV -a IP [-p PORT] [-M out-write|in-write|"
          "out-send-ud] [-q NQPS] [-l SIZE] [-Q UNSIG] [-D SEC] "
          "[--no-inline] [--rc]\n",
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
  c->duration = 5;
  c->use_inline = 1;
  c->use_uc = 1;
  c->is_server = -1;
  c->mode = FIG6_OUT_WRITE;
  static struct option longopts[] = {{"no-inline", no_argument, 0, 1001},
                                     {"rc", no_argument, 0, 1002},
                                     {0, 0, 0, 0}};
  int opt;
  while ((opt = getopt_long(argc, argv, "scd:a:p:x:l:q:Q:D:M:h", longopts,
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
}

static void ud_post_recv(struct ibv_qp *qp, struct vt_ctx *v, uint64_t id) {
  struct ibv_sge sge = {.addr = (uintptr_t)v->buf,
                        .length = (uint32_t)v->buf_size,
                        .lkey = v->mr->lkey};
  struct ibv_recv_wr wr = {.wr_id = id, .sg_list = &sge, .num_sge = 1};
  struct ibv_recv_wr *bad = NULL;
  VT_CHECK(ibv_post_recv(qp, &wr, &bad) == 0, "post_recv");
}

static void ud_passive_loop(struct ibv_qp **qps, struct vt_ctx *v, int nqp) {
  for (int i = 0; i < nqp; i++)
    ud_post_recv(qps[i], v, (uint64_t)i);
  for (;;) {
    struct ibv_wc wc[32];
    int n = ibv_poll_cq(v->cq, 32, wc);
    for (int i = 0; i < n; i++) {
      if (wc[i].status != IBV_WC_SUCCESS) {
        fprintf(stderr, "wc %s\n", ibv_wc_status_str(wc[i].status));
        exit(1);
      }
      if (wc[i].opcode & IBV_WC_RECV)
        ud_post_recv(qps[(int)wc[i].wr_id], v, wc[i].wr_id);
    }
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
  enum ibv_qp_type qpt =
      is_ud ? IBV_QPT_UD : (c.use_uc ? IBV_QPT_UC : IBV_QPT_RC);
  const int inl_cap = is_ud ? VT_MAX_INLINE_UD : VT_MAX_INLINE;

  struct vt_ctx v;
  vt_open_device(&v, c.dev, 1, c.gid_index);
  vt_alloc_buf(&v, VT_BUF_SIZE,
               IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                   IBV_ACCESS_REMOTE_READ);

  struct ibv_qp *qps[VT_MAX_QPS];
  struct vt_endpoint locals[VT_MAX_QPS], remotes[VT_MAX_QPS];
  struct ibv_ah *ahs[VT_MAX_QPS];
  uint32_t psns[VT_MAX_QPS];
  memset(ahs, 0, sizeof(ahs));

  for (int i = 0; i < c.nqp; i++) {
    qps[i] = vt_create_qp(&v, qpt, inl_cap);
    psns[i] = (uint32_t)((vt_ns() + (uint64_t)i * 9973) & 0xffffff);
    vt_fill_local_ep(&v, qps[i], psns[i], &locals[i]);
    locals[i].addr = (uint64_t)(uintptr_t)(v.buf + (size_t)i * 4096);
    vt_qp_to_init(qps[i], v.port);
  }

  int fd = c.is_server ? vt_tcp_listen(c.ip, c.port)
                       : vt_tcp_connect(c.ip, c.port);
  for (int i = 0; i < c.nqp; i++)
    vt_tcp_exchange(fd, &locals[i], &remotes[i]);
  close(fd);

  for (int i = 0; i < c.nqp; i++) {
    vt_qp_to_rtr(&v, qps[i], &remotes[i], qpt);
    vt_qp_to_rts(qps[i], psns[i]);
    if (is_ud) {
      struct vt_qp vq = {.qp = qps[i], .remote = remotes[i]};
      vt_create_ud_ah(&v, &vq);
      ahs[i] = vq.ah;
    }
  }

  if (!i_am_sender(&c)) {
    const char *role = is_ud ? "UD RECV" : "UC passive";
    printf("fig6 passive %s nqp=%d mode=%d\n", role, c.nqp, (int)c.mode);
    if (is_ud)
      ud_passive_loop(qps, &v, c.nqp);
    pause();
    return 0;
  }

  memset(v.buf, 1, VT_BUF_SIZE);
  struct ibv_send_wr wr, *bad;
  struct ibv_sge sge;
  uint64_t nb_tx[VT_MAX_QPS];
  memset(nb_tx, 0, sizeof(nb_tx));
  uint64_t ops = 0;
  uint64_t seed = 0x12345678abcdefull;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c.duration * 1000000000ull;

  while (vt_ns() < deadline) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    int qi = (int)(seed % (uint64_t)c.nqp);

    memset(&wr, 0, sizeof(wr));
    memset(&sge, 0, sizeof(sge));
    wr.num_sge = 1;
    wr.sg_list = &sge;
    wr.send_flags =
        vt_should_signal(nb_tx[qi], c.unsig) ? IBV_SEND_SIGNALED : 0;
    if (vt_should_signal(nb_tx[qi], c.unsig) && nb_tx[qi] > 0)
      vt_poll_cq(v.cq, 1);
    if (c.use_inline && c.size <= inl_cap)
      wr.send_flags |= IBV_SEND_INLINE;
    sge.addr = (uintptr_t)v.buf;
    sge.length = (uint32_t)c.size;
    sge.lkey = v.mr->lkey;

    if (is_ud) {
      wr.opcode = IBV_WR_SEND;
      wr.wr.ud.ah = ahs[qi];
      wr.wr.ud.remote_qpn = remotes[qi].qpn;
      wr.wr.ud.remote_qkey = 0x11111111;
    } else {
      wr.opcode = IBV_WR_RDMA_WRITE;
      wr.wr.rdma.remote_addr = remotes[qi].addr;
      wr.wr.rdma.rkey = remotes[qi].rkey;
    }

    VT_CHECK(ibv_post_send(qps[qi], &wr, &bad) == 0, "post");
    nb_tx[qi]++;
    ops++;
  }

  double sec = (vt_ns() - t0) / 1e9;
  if (c.mode == FIG6_IN_WRITE)
    printf("fig6 In-WRITE: %.2f Mops  nqp=%d size=%d\n", ops / sec / 1e6, c.nqp,
           c.size);
  else if (c.mode == FIG6_OUT_SEND_UD)
    printf("fig6 Out-SEND: %.2f Mops  nqp=%d size=%d\n", ops / sec / 1e6, c.nqp,
           c.size);
  else
    printf("fig6 Out-WRITE: %.2f Mops  nqp=%d size=%d\n", ops / sec / 1e6, c.nqp,
           c.size);

  for (int i = 0; i < c.nqp; i++) {
    if (ahs[i])
      ibv_destroy_ah(ahs[i]);
    ibv_destroy_qp(qps[i]);
  }
  vt_close_device(&v);
  return 0;
}
