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
  close(fd);
  vt_qp_to_rtr(&v, qp, &remote, qpt);
  vt_qp_to_rts(qp, psn);

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

  if (c->is_server) {
    printf("fig5 ww server window=%d size=%d\n", c->window, c->size);
    while (vt_ns() < deadline + 2000000000ull) {
      while (*flag == 0) {
        if (vt_ns() >= deadline + 2000000000ull)
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
        wr[w].send_flags =
            vt_should_signal(nb_tx, c->unsig) ? IBV_SEND_SIGNALED : 0;
        if (vt_should_signal(nb_tx, c->unsig) && nb_tx > 0)
          vt_poll_cq(v.cq, 1);
        if (c->use_inline && c->size <= VT_MAX_INLINE)
          wr[w].send_flags |= IBV_SEND_INLINE;
        payload[0] = 1;
        sgl[w].addr = (uintptr_t)payload;
        sgl[w].length = (uint32_t)c->size;
        sgl[w].lkey = v.mr->lkey;
        wr[w].wr.rdma.remote_addr = remote.addr + (uint64_t)(stride * w);
        wr[w].wr.rdma.rkey = remote.rkey;
        nb_tx++;
      }
      VT_CHECK(ibv_post_send(qp, &wr[0], &bad) == 0, "post");
      echos += (uint64_t)c->window;
    }
  } else {
    printf("fig5 ww client window=%d size=%d\n", c->window, c->size);
    while (vt_ns() < deadline) {
      *flag = 0;
      for (int w = 0; w < c->window; w++) {
        memset(&wr[w], 0, sizeof(wr[w]));
        memset(&sgl[w], 0, sizeof(sgl[w]));
        wr[w].opcode = IBV_WR_RDMA_WRITE;
        wr[w].num_sge = 1;
        wr[w].next = (w == c->window - 1) ? NULL : &wr[w + 1];
        wr[w].sg_list = &sgl[w];
        wr[w].send_flags =
            vt_should_signal(nb_tx, c->unsig) ? IBV_SEND_SIGNALED : 0;
        if (vt_should_signal(nb_tx, c->unsig) && nb_tx > 0)
          vt_poll_cq(v.cq, 1);
        if (c->use_inline && c->size <= VT_MAX_INLINE)
          wr[w].send_flags |= IBV_SEND_INLINE;
        payload[0] = 1;
        sgl[w].addr = (uintptr_t)payload;
        sgl[w].length = (uint32_t)c->size;
        sgl[w].lkey = v.mr->lkey;
        wr[w].wr.rdma.remote_addr = remote.addr + (uint64_t)(stride * w);
        wr[w].wr.rdma.rkey = remote.rkey;
        nb_tx++;
      }
      VT_CHECK(ibv_post_send(qp, &wr[0], &bad) == 0, "post");
      while (*flag == 0) {
      }
      echos += (uint64_t)c->window;
    }
  }

done_ww:
  if (!c->is_server) {
    double sec = (vt_ns() - t0) / 1e9;
    printf("fig5 ww ECHO: %.2f Mops (completed windows*%d)\n",
           echos / sec / 1e6, c->window);
  }
  ibv_destroy_qp(qp);
  vt_close_device(&v);
}

/*
 * WRITE request + UD SEND response (ws-echo / HERD).
 * Client: UC WRITE into server flag; poll RECV for response.
 * Server: poll flag; UD SEND response.
 */
static void run_ws(struct cfg *c) {
  enum ibv_qp_type conn_t = c->use_uc ? IBV_QPT_UC : IBV_QPT_RC;

  struct vt_ctx v;
  vt_open_device(&v, c->dev, 1, c->gid_index);
  vt_alloc_buf(&v, VT_BUF_SIZE,
               IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                   IBV_ACCESS_REMOTE_READ);

  /* Connected QP for WRITE requests */
  struct ibv_qp *cqp = vt_create_qp(&v, conn_t, VT_MAX_INLINE);
  /* Datagram QP for SEND responses */
  struct ibv_qp *dqp = vt_create_qp(&v, IBV_QPT_UD, VT_MAX_INLINE);

  uint32_t cpsn = (uint32_t)(vt_ns() & 0xffffff);
  uint32_t dpsn = (uint32_t)((vt_ns() >> 8) & 0xffffff);
  struct vt_endpoint clocal, cremote, dlocal, dremote;
  vt_fill_local_ep(&v, cqp, cpsn, &clocal);
  vt_fill_local_ep(&v, dqp, dpsn, &dlocal);

  vt_qp_to_init(cqp, v.port);
  vt_qp_to_init(dqp, v.port);

  int fd = c->is_server ? vt_tcp_listen(c->ip, c->port)
                        : vt_tcp_connect(c->ip, c->port);
  /* Exchange connected then dgram endpoints (order fixed). */
  vt_tcp_exchange(fd, &clocal, &cremote);
  vt_tcp_exchange(fd, &dlocal, &dremote);
  close(fd);

  vt_qp_to_rtr(&v, cqp, &cremote, conn_t);
  vt_qp_to_rts(cqp, cpsn);
  vt_qp_to_rtr(&v, dqp, &dremote, IBV_QPT_UD);
  vt_qp_to_rts(dqp, dpsn);

  struct vt_qp dvq;
  memset(&dvq, 0, sizeof(dvq));
  dvq.qp = dqp;
  dvq.remote = dremote;
  vt_create_ud_ah(&v, &dvq);

  volatile uint8_t *flag = v.buf;
  *flag = 0;
  uint8_t *payload = v.buf + VT_CACHELINE;
  memset(payload, 1, (size_t)c->size);

  struct ibv_send_wr wr, *bad;
  struct ibv_sge sge;
  uint64_t nb_tx = 0, echos = 0;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c->duration * 1000000000ull;

  if (c->is_server) {
    printf("fig5 ws server (WRITE poll + UD SEND)\n");
    while (vt_ns() < deadline + 2000000000ull) {
      while (*flag == 0) {
        if (vt_ns() >= deadline + 2000000000ull)
          goto done_ws;
      }
      *flag = 0;

      memset(&wr, 0, sizeof(wr));
      memset(&sge, 0, sizeof(sge));
      wr.opcode = IBV_WR_SEND;
      wr.num_sge = 1;
      wr.sg_list = &sge;
      wr.send_flags =
          vt_should_signal(nb_tx, c->unsig) ? IBV_SEND_SIGNALED : 0;
      if (vt_should_signal(nb_tx, c->unsig) && nb_tx > 0)
        vt_poll_cq(v.cq, 1);
      if (c->use_inline && c->size <= VT_MAX_INLINE)
        wr.send_flags |= IBV_SEND_INLINE;
      wr.wr.ud.ah = dvq.ah;
      wr.wr.ud.remote_qpn = dremote.qpn;
      wr.wr.ud.remote_qkey = 0x11111111;
      sge.addr = (uintptr_t)payload;
      sge.length = (uint32_t)c->size;
      sge.lkey = v.mr->lkey;
      VT_CHECK(ibv_post_send(dqp, &wr, &bad) == 0, "ud send");
      nb_tx++;
      echos++;
    }
  } else {
    printf("fig5 ws client (UC WRITE + UD RECV)\n");
    for (int i = 0; i < VT_RQ_DEPTH / 2; i++)
      post_one_recv(dqp, &v, (uint64_t)i);

    while (vt_ns() < deadline) {
      *flag = 0;
      payload[0] = 1;
      memset(&wr, 0, sizeof(wr));
      memset(&sge, 0, sizeof(sge));
      wr.opcode = IBV_WR_RDMA_WRITE;
      wr.num_sge = 1;
      wr.sg_list = &sge;
      wr.send_flags =
          vt_should_signal(nb_tx, c->unsig) ? IBV_SEND_SIGNALED : 0;
      if (vt_should_signal(nb_tx, c->unsig) && nb_tx > 0)
        vt_poll_cq(v.cq, 1);
      if (c->use_inline && c->size <= VT_MAX_INLINE)
        wr.send_flags |= IBV_SEND_INLINE;
      sge.addr = (uintptr_t)payload;
      sge.length = (uint32_t)c->size;
      sge.lkey = v.mr->lkey;
      wr.wr.rdma.remote_addr = cremote.addr;
      wr.wr.rdma.rkey = cremote.rkey;
      VT_CHECK(ibv_post_send(cqp, &wr, &bad) == 0, "write");
      nb_tx++;

      /* Wait for one UD RECV completion, then repost. */
      struct ibv_wc wc;
      for (;;) {
        int r = ibv_poll_cq(v.cq, 1, &wc);
        if (r == 1 && (wc.opcode & IBV_WC_RECV)) {
          if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "recv %s\n", ibv_wc_status_str(wc.status));
            exit(1);
          }
          post_one_recv(dqp, &v, wc.wr_id);
          break;
        }
        if (r == 1 && wc.status != IBV_WC_SUCCESS) {
          fprintf(stderr, "wc %s\n", ibv_wc_status_str(wc.status));
          exit(1);
        }
      }
      echos++;
    }
    double sec = (vt_ns() - t0) / 1e9;
    printf("fig5 ws ECHO: %.2f Mops\n", echos / sec / 1e6);
  }

done_ws:
  if (dvq.ah)
    ibv_destroy_ah(dvq.ah);
  ibv_destroy_qp(cqp);
  ibv_destroy_qp(dqp);
  vt_close_device(&v);
}

/* SEND/SEND over UD both ways (simplified ss). */
static void run_ss(struct cfg *c) {
  struct vt_ctx v;
  vt_open_device(&v, c->dev, 1, c->gid_index);
  vt_alloc_buf(&v, VT_BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);
  struct ibv_qp *qp = vt_create_qp(&v, IBV_QPT_UD, VT_MAX_INLINE);
  uint32_t psn = (uint32_t)(vt_ns() & 0xffffff);
  struct vt_endpoint local, remote;
  vt_fill_local_ep(&v, qp, psn, &local);
  vt_qp_to_init(qp, v.port);
  int fd = c->is_server ? vt_tcp_listen(c->ip, c->port)
                        : vt_tcp_connect(c->ip, c->port);
  vt_tcp_exchange(fd, &local, &remote);
  close(fd);
  vt_qp_to_rtr(&v, qp, &remote, IBV_QPT_UD);
  vt_qp_to_rts(qp, psn);
  struct vt_qp vq = {.qp = qp, .remote = remote};
  vt_create_ud_ah(&v, &vq);

  for (int i = 0; i < VT_RQ_DEPTH / 2; i++)
    post_one_recv(qp, &v, (uint64_t)i);

  uint8_t *payload = v.buf + VT_CACHELINE;
  memset(payload, 1, (size_t)c->size);
  struct ibv_send_wr wr, *bad;
  struct ibv_sge sge;
  uint64_t nb_tx = 0, echos = 0;
  uint64_t t0 = vt_ns();
  uint64_t deadline = t0 + (uint64_t)c->duration * 1000000000ull;

  if (c->is_server) {
    printf("fig5 ss server\n");
    while (vt_ns() < deadline + 2000000000ull) {
      struct ibv_wc wc;
      int r = ibv_poll_cq(v.cq, 1, &wc);
      if (r != 1)
        continue;
      if (wc.status != IBV_WC_SUCCESS)
        continue;
      if (wc.opcode & IBV_WC_RECV) {
        post_one_recv(qp, &v, wc.wr_id);
        memset(&wr, 0, sizeof(wr));
        memset(&sge, 0, sizeof(sge));
        wr.opcode = IBV_WR_SEND;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.send_flags =
            vt_should_signal(nb_tx, c->unsig) ? IBV_SEND_SIGNALED : 0;
        if (vt_should_signal(nb_tx, c->unsig) && nb_tx > 0)
          vt_poll_cq(v.cq, 1);
        if (c->use_inline && c->size <= VT_MAX_INLINE)
          wr.send_flags |= IBV_SEND_INLINE;
        wr.wr.ud.ah = vq.ah;
        wr.wr.ud.remote_qpn = remote.qpn;
        wr.wr.ud.remote_qkey = 0x11111111;
        sge.addr = (uintptr_t)payload;
        sge.length = (uint32_t)c->size;
        sge.lkey = v.mr->lkey;
        VT_CHECK(ibv_post_send(qp, &wr, &bad) == 0, "send");
        nb_tx++;
        echos++;
      }
    }
  } else {
    printf("fig5 ss client\n");
    while (vt_ns() < deadline) {
      memset(&wr, 0, sizeof(wr));
      memset(&sge, 0, sizeof(sge));
      wr.opcode = IBV_WR_SEND;
      wr.sg_list = &sge;
      wr.num_sge = 1;
      wr.send_flags =
          vt_should_signal(nb_tx, c->unsig) ? IBV_SEND_SIGNALED : 0;
      if (vt_should_signal(nb_tx, c->unsig) && nb_tx > 0)
        vt_poll_cq(v.cq, 1);
      if (c->use_inline && c->size <= VT_MAX_INLINE)
        wr.send_flags |= IBV_SEND_INLINE;
      wr.wr.ud.ah = vq.ah;
      wr.wr.ud.remote_qpn = remote.qpn;
      wr.wr.ud.remote_qkey = 0x11111111;
      sge.addr = (uintptr_t)payload;
      sge.length = (uint32_t)c->size;
      sge.lkey = v.mr->lkey;
      VT_CHECK(ibv_post_send(qp, &wr, &bad) == 0, "send");
      nb_tx++;

      for (;;) {
        struct ibv_wc wc;
        int r = ibv_poll_cq(v.cq, 1, &wc);
        if (r == 1 && (wc.opcode & IBV_WC_RECV)) {
          post_one_recv(qp, &v, wc.wr_id);
          break;
        }
      }
      echos++;
    }
    double sec = (vt_ns() - t0) / 1e9;
    printf("fig5 ss ECHO: %.2f Mops\n", echos / sec / 1e6);
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
