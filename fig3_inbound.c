/*
 * fig3_inbound.c — HERD Sec.3 Fig.3: inbound verb throughput.
 *
 * Template: rdma_bench/rw-tput-receiver/main.cc
 *   - Server publishes QP and sleeps (passive).
 *   - Client posts WRITE/READ window with selective signaling + optional inline.
 *   - One client thread <-> one server thread (paper Fig.3a).
 *
 * Direction: CLIENT is the requester (verbs land on the server RNIC).
 * Use multiple client processes / machines and sum Mops for multi-client curves.
 *
 * Flags mirror the template:
 *   -c UC|RC   transport for WRITE (READ always RC)
 *   -t N       postlist / window batch
 *   -Q N       UNSIG_BATCH (signal every N)
 *   -l SIZE    payload
 *   -D SEC     run duration
 *   --no-inline
 *
 * Commented original registry style (libhrd / rw-tput-receiver):
 *
 *   char srv_name[kHrdQPNameSize];
 *   sprintf(srv_name, "server-%zu", srv_gid);
 *   char clt_name[kHrdQPNameSize];
 *   sprintf(clt_name, "client-%zu", clt_gid);
 *
 *   hrd_publish_conn_qp(cb, 0, srv_name);
 *   printf("main: Server %s published. Waiting for client %s\n", srv_name,
 *          clt_name);
 *
 *   hrd_qp_attr_t *clt_qp = NULL;
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
 *   printf("main: Server %s READY\n", srv_name);
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
  int postlist;
  int unsig;
  int duration;
  int use_uc;
  int do_read;
  int use_inline;
};

static void usage(const char *a) {
  fprintf(stderr,
          "Usage: %s -s|-c -d DEV -a IP [-p PORT] [-x GID] [-l SIZE] [-t POST] "
          "[-Q UNSIG] [-D SEC] [-c UC|RC] [--read] [--no-inline]\n"
          "Note: short -c is client; use --uc / --rc for transport.\n",
          a);
  exit(1);
}

static void parse(int argc, char **argv, struct cfg *c) {
  memset(c, 0, sizeof(*c));
  c->port = 18510;
  c->gid_index = 3;
  c->size = 32;
  c->postlist = 64;
  c->unsig = 64;
  c->duration = 5;
  c->use_uc = 1;
  c->use_inline = 1;
  c->is_server = -1;

  static struct option longopts[] = {{"read", no_argument, 0, 1000},
                                     {"no-inline", no_argument, 0, 1001},
                                     {"uc", no_argument, 0, 1002},
                                     {"rc", no_argument, 0, 1003},
                                     {0, 0, 0, 0}};
  int opt;
  while ((opt = getopt_long(argc, argv, "scd:a:p:x:l:t:Q:D:h", longopts,
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
    case 't':
      c->postlist = atoi(optarg);
      break;
    case 'Q':
      c->unsig = atoi(optarg);
      break;
    case 'D':
      c->duration = atoi(optarg);
      break;
    case 1000:
      c->do_read = 1;
      c->use_uc = 0;
      c->use_inline = 0;
      break;
    case 1001:
      c->use_inline = 0;
      break;
    case 1002:
      c->use_uc = 1;
      break;
    case 1003:
      c->use_uc = 0;
      break;
    default:
      usage(argv[0]);
    }
  }
  if (c->is_server < 0 || !c->dev || !c->ip)
    usage(argv[0]);
  if (c->postlist > c->unsig) {
    fprintf(stderr, "postlist must be <= unsig_batch (rdma_bench rule)\n");
    exit(1);
  }
}

int main(int argc, char **argv) {
  struct cfg c;
  parse(argc, argv, &c);

  enum ibv_qp_type qpt = (c.do_read || !c.use_uc) ? IBV_QPT_RC : IBV_QPT_UC;
  struct vt_ctx v;
  vt_open_device(&v, c.dev, 1, c.gid_index);
  int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
               IBV_ACCESS_REMOTE_READ;
  vt_alloc_buf(&v, VT_BUF_SIZE, access);
  struct ibv_qp *qp = vt_create_qp(&v, qpt, VT_MAX_INLINE);
  uint32_t psn = (uint32_t)(vt_ns() & 0xffffff);
  struct vt_endpoint local, remote;
  vt_fill_local_ep(&v, qp, psn, &local);
  vt_qp_to_init(qp, v.port);

  int fd = c.is_server ? vt_tcp_listen(c.ip, c.port)
                       : vt_tcp_connect(c.ip, c.port);
  vt_tcp_exchange(fd, &local, &remote);
  close(fd);
  vt_qp_to_rtr(&v, qp, &remote, qpt);
  vt_qp_to_rts(qp, psn);

  if (c.is_server) {
    printf("fig3 inbound server ready (passive). qpt=%s size=%d\n",
           qpt == IBV_QPT_UC ? "UC" : "RC", c.size);
    /*
     * Same as rw-tput-receiver server loop: just keep process alive.
     * Optional debug print of buf[0] from the template (commented, not deleted):
     *
     *   while (1) {
     *     printf("main: Server: %d\n", v.buf[0]);
     *     sleep(1);
     *   }
     */
    pause();
    return 0;
  }

  /* ---- client requester (template run_client) ---- */
  int stride = VT_CACHELINE;
  while (stride < c.size)
    stride += VT_CACHELINE;
  VT_CHECK((size_t)stride * (size_t)c.postlist <= VT_BUF_SIZE, "buf");

  struct ibv_send_wr wr[64], *bad;
  struct ibv_sge sgl[64];
  struct ibv_wc wc;
  VT_CHECK(c.postlist <= 64, "postlist");

  memset(v.buf, 1, VT_BUF_SIZE);
  enum ibv_wr_opcode opcode = c.do_read ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE;

  uint64_t nb_tx = 0, ops = 0;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c.duration * 1000000000ull;

  while (vt_ns() < deadline) {
    for (int w = 0; w < c.postlist; w++) {
      memset(&wr[w], 0, sizeof(wr[w]));
      memset(&sgl[w], 0, sizeof(sgl[w]));
      wr[w].opcode = opcode;
      wr[w].num_sge = 1;
      wr[w].next = (w == c.postlist - 1) ? NULL : &wr[w + 1];
      wr[w].sg_list = &sgl[w];
      wr[w].send_flags = vt_should_signal(nb_tx, c.unsig) ? IBV_SEND_SIGNALED : 0;
      if (vt_should_signal(nb_tx, c.unsig) && nb_tx > 0)
        vt_poll_cq(v.cq, 1);
      if (!c.do_read && c.use_inline && c.size <= VT_MAX_INLINE)
        wr[w].send_flags |= IBV_SEND_INLINE;
      sgl[w].addr = (uintptr_t)(v.buf + stride * w);
      sgl[w].length = (uint32_t)c.size;
      sgl[w].lkey = v.mr->lkey;
      wr[w].wr.rdma.remote_addr = remote.addr + (uint64_t)(stride * w);
      wr[w].wr.rdma.rkey = remote.rkey;
      nb_tx++;
    }
    VT_CHECK(ibv_post_send(qp, &wr[0], &bad) == 0, "post_send");
    ops += (uint64_t)c.postlist;
  }

  /* Drain last signaled completion if any outstanding. */
  (void)wc;
  double sec = (vt_ns() - t0) / 1e9;
  printf("fig3 inbound client: %.2f Mops  (ops=%" PRIu64 " size=%d qpt=%s "
         "inline=%d)\n",
         ops / sec / 1e6, ops, c.size, qpt == IBV_QPT_UC ? "UC" : "RC",
         c.use_inline && !c.do_read);

  ibv_destroy_qp(qp);
  vt_close_device(&v);
  return 0;
}
