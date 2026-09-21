/*
 * fig6_scale.c — HERD Sec.3 Fig.6: UC WRITE vs many QPs (outbound focus).
 *
 * Template: rdma_bench/sender-scalability/main.cc
 *   Server thread creates N QPs, each connected to a different client QP.
 *   Then randomly picks a client QP and issues inline unsignaled WRITEs.
 *
 * Paper also measures inbound all-to-all and Out-SEND-UD. Those paths are
 * sketched in comments below (not deleted) — this binary implements the
 * Out-WRITE-UC scaling curve that drops sharply with QP count.
 *
 * -q N : number of QPs (= fanout). Paper uses N client procs => N^2 QPs at
 *        the server when all-to-all; here one process pair with N QPs is the
 *        same NIC-cache pressure knob.
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
  int nqp;
  int unsig;
  int duration;
  int use_inline;
  int use_uc;
};

static void usage(const char *a) {
  fprintf(stderr,
          "Usage: %s -s|-c -d DEV -a IP [-p PORT] [-q NQPS] [-l SIZE] "
          "[-Q UNSIG] [-D SEC] [--no-inline] [--rc]\n",
          a);
  exit(1);
}

static void parse(int argc, char **argv, struct cfg *c) {
  memset(c, 0, sizeof(*c));
  c->port = 18540;
  c->gid_index = 3;
  c->size = 32;
  c->nqp = 16;
  c->unsig = 4; /* sender-scalability default style: small unsig batch */
  c->duration = 5;
  c->use_inline = 1;
  c->use_uc = 1;
  c->is_server = -1;
  static struct option longopts[] = {{"no-inline", no_argument, 0, 1001},
                                     {"rc", no_argument, 0, 1002},
                                     {0, 0, 0, 0}};
  int opt;
  while ((opt = getopt_long(argc, argv, "scd:a:p:x:l:q:Q:D:h", longopts,
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

int main(int argc, char **argv) {
  struct cfg c;
  parse(argc, argv, &c);
  enum ibv_qp_type qpt = c.use_uc ? IBV_QPT_UC : IBV_QPT_RC;

  struct vt_ctx v;
  vt_open_device(&v, c.dev, 1, c.gid_index);
  vt_alloc_buf(&v, VT_BUF_SIZE,
               IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                   IBV_ACCESS_REMOTE_READ);

  struct ibv_qp *qps[VT_MAX_QPS];
  struct vt_endpoint locals[VT_MAX_QPS], remotes[VT_MAX_QPS];
  uint32_t psns[VT_MAX_QPS];

  for (int i = 0; i < c.nqp; i++) {
    qps[i] = vt_create_qp(&v, qpt, VT_MAX_INLINE);
    psns[i] = (uint32_t)((vt_ns() + (uint64_t)i * 9973) & 0xffffff);
    vt_fill_local_ep(&v, qps[i], psns[i], &locals[i]);
    /* Per-QP remote buffer offset so WRITEs do not stomp each other. */
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
  }

  /*
   * Inbound all-to-all (paper Fig.6 In-WRITE-UC) — not active; full sketch kept:
   *
   *   // Server side: stay passive (like fig3), do not post sends.
   *   // if (c.is_server) {
   *   //   printf("fig6 inbound passive server with %d QPs\n", c.nqp);
   *   //   pause();
   *   //   return 0;
   *   // }
   *   // Client side is the requester below.
   *   uint64_t seed = 0xdeadbeefull;
   *   uint64_t nb_tx_in[VT_MAX_QPS];
   *   memset(nb_tx_in, 0, sizeof(nb_tx_in));
   *   uint64_t ops_in = 0;
   *   uint64_t t0_in = vt_ns();
   *   uint64_t deadline_in = t0_in + (uint64_t)c.duration * 1000000000ull;
   *   struct ibv_send_wr wr_in, *bad_in;
   *   struct ibv_sge sge_in;
   *   while (vt_ns() < deadline_in) {
   *     seed ^= seed << 13;
   *     seed ^= seed >> 7;
   *     seed ^= seed << 17;
   *     int qi = (int)(seed % (uint64_t)c.nqp);
   *     memset(&wr_in, 0, sizeof(wr_in));
   *     memset(&sge_in, 0, sizeof(sge_in));
   *     wr_in.opcode = IBV_WR_RDMA_WRITE;
   *     wr_in.num_sge = 1;
   *     wr_in.sg_list = &sge_in;
   *     wr_in.send_flags =
   *         vt_should_signal(nb_tx_in[qi], c.unsig) ? IBV_SEND_SIGNALED : 0;
   *     if (vt_should_signal(nb_tx_in[qi], c.unsig) && nb_tx_in[qi] > 0)
   *       vt_poll_cq(v.cq, 1);
   *     if (c.use_inline && c.size <= VT_MAX_INLINE)
   *       wr_in.send_flags |= IBV_SEND_INLINE;
   *     sge_in.addr = (uintptr_t)v.buf;
   *     sge_in.length = (uint32_t)c.size;
   *     sge_in.lkey = v.mr->lkey;
   *     wr_in.wr.rdma.remote_addr = remotes[qi].addr;
   *     wr_in.wr.rdma.rkey = remotes[qi].rkey;
   *     VT_CHECK(ibv_post_send(qps[qi], &wr_in, &bad_in) == 0, "post");
   *     nb_tx_in[qi]++;
   *     ops_in++;
   *   }
   *   double sec_in = (vt_ns() - t0_in) / 1e9;
   *   printf("fig6 In-WRITE: %.2f Mops  nqp=%d size=%d\n",
   *          ops_in / sec_in / 1e6, c.nqp, c.size);
   *
   * Out-SEND-UD scaling — use fig4 -m send_ud with one UD QP (scales by design):
   *
   *   // ./fig4_outbound -s -R -d mlx5_0 -a 10.0.0.20 -p 18520 -m send_ud \
   *   //     -l 32 -t 64 -Q 64 -D 5
   *   // ./fig4_outbound -c -R -d mlx5_3 -a 10.0.0.20 -p 18520 -m send_ud \
   *   //     -l 32 -t 64 -Q 64 -D 5
   */

  int i_am_sender = c.is_server; /* MS issues outbound WRITEs */
  if (!i_am_sender) {
    printf("fig6 passive client with %d QPs\n", c.nqp);
    pause();
    return 0;
  }

  printf("fig6 outbound WRITE fanout=%d size=%d uc=%d\n", c.nqp, c.size,
         c.use_uc);
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
    /* xorshift pick QP — same spirit as sender-scalability random client. */
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    int qi = (int)(seed % (uint64_t)c.nqp);

    memset(&wr, 0, sizeof(wr));
    memset(&sge, 0, sizeof(sge));
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.num_sge = 1;
    wr.sg_list = &sge;
    wr.send_flags =
        vt_should_signal(nb_tx[qi], c.unsig) ? IBV_SEND_SIGNALED : 0;
    if (vt_should_signal(nb_tx[qi], c.unsig) && nb_tx[qi] > 0)
      vt_poll_cq(v.cq, 1);
    if (c.use_inline && c.size <= VT_MAX_INLINE)
      wr.send_flags |= IBV_SEND_INLINE;
    sge.addr = (uintptr_t)v.buf;
    sge.length = (uint32_t)c.size;
    sge.lkey = v.mr->lkey;
    wr.wr.rdma.remote_addr = remotes[qi].addr;
    wr.wr.rdma.rkey = remotes[qi].rkey;
    VT_CHECK(ibv_post_send(qps[qi], &wr, &bad) == 0, "post");
    nb_tx[qi]++;
    ops++;
  }

  double sec = (vt_ns() - t0) / 1e9;
  printf("fig6 Out-WRITE: %.2f Mops  nqp=%d size=%d\n", ops / sec / 1e6, c.nqp,
         c.size);

  for (int i = 0; i < c.nqp; i++)
    ibv_destroy_qp(qps[i]);
  vt_close_device(&v);
  return 0;
}
