/*
 * fig4_outbound.c — HERD Sec.3 Fig.4: outbound verb throughput.
 *
 * Templates:
 *   WRITE/READ: rdma_bench/rw-tput-sender/main.cc
 *   UD SEND:    rdma_bench/ud-sender/main.cc
 *
 * Paper Fig.4a: processes on MS issue verbs toward clients (one-to-one).
 * Here -R means "requester is the TCP server side" (run on MS).
 * Without -R, requester is the TCP client (also useful for debugging).
 *
 * Modes (-m):
 *   write_uc   UC WRITE (+inline by default)
 *   write_rc   RC WRITE
 *   read       RC READ
 *   send_ud    UD SEND (+inline) — needs peer pre-posted RECVs
 */

#include "common.h"

#include <getopt.h>

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
  char mode[32];
};

static void usage(const char *a) {
  fprintf(stderr,
          "Usage: %s -s|-c -d DEV -a IP [-p PORT] [-R] [-l SIZE] [-t POST] "
          "[-Q UNSIG] [-D SEC] [-m write_uc|write_rc|read|send_ud] "
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
  c->is_server = -1;
  strcpy(c->mode, "write_uc");
  static struct option longopts[] = {{"no-inline", no_argument, 0, 1001},
                                     {0, 0, 0, 0}};
  int opt;
  while ((opt = getopt_long(argc, argv, "scRd:a:p:x:l:t:Q:D:m:h", longopts,
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

int main(int argc, char **argv) {
  struct cfg c;
  parse(argc, argv, &c);

  int is_ud = (strcmp(c.mode, "send_ud") == 0);
  int do_read = (strcmp(c.mode, "read") == 0);
  int use_uc = (strcmp(c.mode, "write_uc") == 0);
  enum ibv_qp_type qpt =
      is_ud ? IBV_QPT_UD : (do_read || !use_uc) ? IBV_QPT_RC : IBV_QPT_UC;

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

  struct vt_qp vq;
  memset(&vq, 0, sizeof(vq));
  vq.qp = qp;
  vq.type = qpt;
  vq.remote = remote;
  if (is_ud)
    vt_create_ud_ah(&v, &vq);

  int i_am_requester =
      (c.requester_is_server && c.is_server) ||
      (!c.requester_is_server && !c.is_server);

  if (!i_am_requester) {
    printf("fig4 passive side mode=%s\n", c.mode);
    if (is_ud) {
      /* Keep RQ filled for inbound SENDs (ud-sender peer). */
      post_recvs(qp, &v, VT_RQ_DEPTH / 2);
      for (;;) {
        struct ibv_wc wc[16];
        int n = ibv_poll_cq(v.cq, 16, wc);
        for (int i = 0; i < n; i++) {
          if (wc[i].status != IBV_WC_SUCCESS) {
            fprintf(stderr, "wc %s\n", ibv_wc_status_str(wc[i].status));
            exit(1);
          }
          if (wc[i].opcode & IBV_WC_RECV)
            post_recvs(qp, &v, 1);
        }
      }
    }
    pause();
    return 0;
  }

  printf("fig4 requester mode=%s size=%d post=%d unsig=%d\n", c.mode, c.size,
         c.postlist, c.unsig);

  int stride = VT_CACHELINE;
  while (stride < c.size)
    stride += VT_CACHELINE;

  struct ibv_send_wr wr[64], *bad;
  struct ibv_sge sgl[64];
  VT_CHECK(c.postlist <= 64, "postlist");
  memset(v.buf, 1, VT_BUF_SIZE);

  uint64_t nb_tx = 0, ops = 0;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c.duration * 1000000000ull;

  while (vt_ns() < deadline) {
    for (int w = 0; w < c.postlist; w++) {
      memset(&wr[w], 0, sizeof(wr[w]));
      memset(&sgl[w], 0, sizeof(sgl[w]));
      wr[w].num_sge = 1;
      wr[w].next = (w == c.postlist - 1) ? NULL : &wr[w + 1];
      wr[w].sg_list = &sgl[w];
      wr[w].send_flags =
          vt_should_signal(nb_tx, c.unsig) ? IBV_SEND_SIGNALED : 0;
      if (vt_should_signal(nb_tx, c.unsig) && nb_tx > 0)
        vt_poll_cq(v.cq, 1);
      if (c.use_inline && !do_read && c.size <= VT_MAX_INLINE)
        wr[w].send_flags |= IBV_SEND_INLINE;

      sgl[w].addr = (uintptr_t)(v.buf + stride * w);
      sgl[w].length = (uint32_t)c.size;
      sgl[w].lkey = v.mr->lkey;

      if (is_ud) {
        wr[w].opcode = IBV_WR_SEND;
        wr[w].wr.ud.ah = vq.ah;
        wr[w].wr.ud.remote_qpn = remote.qpn;
        wr[w].wr.ud.remote_qkey = 0x11111111;
      } else {
        wr[w].opcode = do_read ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE;
        wr[w].wr.rdma.remote_addr = remote.addr + (uint64_t)(stride * w);
        wr[w].wr.rdma.rkey = remote.rkey;
      }
      nb_tx++;
    }
    VT_CHECK(ibv_post_send(qp, &wr[0], &bad) == 0, "post_send");
    ops += (uint64_t)c.postlist;
  }

  double sec = (vt_ns() - t0) / 1e9;
  printf("fig4 outbound: %.2f Mops  mode=%s size=%d\n", ops / sec / 1e6, c.mode,
         c.size);

  if (vq.ah)
    ibv_destroy_ah(vq.ah);
  ibv_destroy_qp(qp);
  vt_close_device(&v);
  return 0;
}
