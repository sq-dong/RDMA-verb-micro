/*
 * fig2_latency.c — HERD Sec.3 Fig.2: verb / ECHO latency.
 *
 * Template sources:
 *   - Signaled READ/WRITE latency style:
 *       rdma_bench/rw-tput-receiver/main.cc  (+ README: UnsigBatch=1, postlist=1)
 *   - ECHO via memory polling (unsignaled WRITE):
 *       rdma_bench/ww-echo/client.c  (wait while conn_buf[0]==0)
 *       rdma_bench/ww-echo/server.c  (same poll then WRITE back)
 *
 * Modes (-m):
 *   write       signaled RDMA WRITE (RC), time post->CQE
 *   write_inl   same + IBV_SEND_INLINE
 *   read        signaled RDMA READ (RC)
 *   echo        RC WRITE request + RC WRITE response, poll memory; print RTT and RTT/2
 *               (paper Fig.2a: "WR-I, RC (ECHO)"; ww-echo-style flag poll)
 *
 * Unused alternative (perftest-style, not used — kept for reference).
 * Always signaled; cannot measure unsignaled WRITE or ECHO/2:
 *
 *   char cmd[256];
 *   snprintf(cmd, sizeof(cmd),
 *            "ib_write_lat -d %s -F -x %d -s %d -n %d %s",
 *            c->dev, c->gid_index, c->size, c->iters,
 *            c->is_server ? "" : c->ip);
 *   system(cmd);
 *
 *   snprintf(cmd, sizeof(cmd),
 *            "ib_write_lat -d %s -F -x %d -s %d -n %d -I %d %s",
 *            c->dev, c->gid_index, c->size, c->iters, c->size,
 *            c->is_server ? "" : c->ip);
 *   system(cmd);
 *
 *   snprintf(cmd, sizeof(cmd),
 *            "ib_read_lat -d %s -F -x %d -s %d -n %d %s",
 *            c->dev, c->gid_index, c->size, c->iters,
 *            c->is_server ? "" : c->ip);
 *   system(cmd);
 */

#include "common.h"

#include <getopt.h>

struct cfg {
  int is_server;
  char *dev;
  char *ip;
  uint16_t port;
  int gid_index;
  int iters;
  int warmup;
  int size;
  char mode[32];
};

static void usage(const char *a) {
  fprintf(stderr,
          "Usage: %s -s|-c -d DEV -a IP -p PORT [-x GID] [-n ITERS] [-w WARMUP] "
          "[-l SIZE] [-m write|write_inl|read|echo]\n",
          a);
  exit(1);
}

static void parse(int argc, char **argv, struct cfg *c) {
  memset(c, 0, sizeof(*c));
  c->port = 18500;
  c->gid_index = 3;
  c->iters = 10000;
  c->warmup = 1000;
  c->size = 64;
  strcpy(c->mode, "write");
  int opt;
  while ((opt = getopt(argc, argv, "scd:a:p:x:n:w:l:m:h")) != -1) {
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
    case 'n':
      c->iters = atoi(optarg);
      break;
    case 'w':
      c->warmup = atoi(optarg);
      break;
    case 'l':
      c->size = atoi(optarg);
      break;
    case 'm':
      strncpy(c->mode, optarg, sizeof(c->mode) - 1);
      break;
    default:
      usage(argv[0]);
    }
  }
  if (!c->dev || !c->ip)
    usage(argv[0]);
}

/* ---- signaled one-sided latency (Fig.2 solid lines) ---- */
static void run_onesided(struct cfg *c) {
  int do_read = (strcmp(c->mode, "read") == 0);
  int do_inl = (strcmp(c->mode, "write_inl") == 0);
  enum ibv_qp_type qpt = IBV_QPT_RC; /* READ needs RC; WRITE matched to paper RC line */

  /*
   * Paper also plots WRITE over UC, but signaled WRITE latency path length is
   * the same on RC (completion waits for ACK). Keep RC here.
   * Alternative (commented, not deleted):
   *
   *   if (!do_read) {
   *     qpt = IBV_QPT_UC;
   *   } else {
   *     qpt = IBV_QPT_RC;
   *   }
   */

  struct vt_ctx v;
  vt_open_device(&v, c->dev, 1, c->gid_index);
  int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
               IBV_ACCESS_REMOTE_READ;
  vt_alloc_buf(&v, VT_BUF_SIZE, access);
  /* Request only the inline this size needs — not the full HW max. */
  int inl = vt_inline_grant(c->size, do_inl, 0);
  struct ibv_qp *qp = vt_create_qp(&v, qpt, inl, NULL, NULL);
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

  if (c->is_server) {
    printf("fig2 server ready mode=%s size=%d\n", c->mode, c->size);
    /* Passive; client drives completions. */
    pause();
    return;
  }

  struct ibv_send_wr wr;
  struct ibv_sge sge;
  struct ibv_wc wc;
  memset(v.buf, 0xab, (size_t)c->size);

  double sum_us = 0, min_us = 1e9, max_us = 0;
  int total = c->warmup + c->iters;
  for (int i = 0; i < total; i++) {
    memset(&wr, 0, sizeof(wr));
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)v.buf;
    sge.length = (uint32_t)c->size;
    sge.lkey = v.mr->lkey;
    wr.wr_id = 1;
    wr.opcode = do_read ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    if (do_inl && !do_read && c->size <= inl)
      wr.send_flags |= IBV_SEND_INLINE;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.wr.rdma.remote_addr = remote.addr;
    wr.wr.rdma.rkey = remote.rkey;

    uint64_t t0 = vt_ns();
    struct ibv_send_wr *bad = NULL;
    VT_CHECK(ibv_post_send(qp, &wr, &bad) == 0, "post_send");
    VT_CHECK(vt_poll_cq_one(v.cq, &wc, 5000) == 0, "poll");
    uint64_t t1 = vt_ns();
    if (i >= c->warmup) {
      double us = (t1 - t0) / 1000.0;
      sum_us += us;
      if (us < min_us)
        min_us = us;
      if (us > max_us)
        max_us = us;
    }
  }
  printf("fig2 mode=%s size=%d iters=%d  avg=%.3f us  min=%.3f  max=%.3f\n",
         c->mode, c->size, c->iters, sum_us / c->iters, min_us, max_us);

  ibv_destroy_qp(qp);
  vt_close_device(&v);
}

/* Busy-poll until *flag == expect, or timeout_us elapses. Returns 0 on hit. */
static int wait_flag_eq(volatile uint8_t *flag, uint8_t expect, int timeout_us) {
  uint64_t t0 = vt_ns();
  uint64_t lim = (uint64_t)timeout_us * 1000ull;
  while (*flag != expect) {
    if ((vt_ns() - t0) > lim)
      return -1;
  }
  return 0;
}

/*
 * ECHO: both sides RC WRITE + poll buf[0] (paper Fig.2a "WR-I, RC (ECHO)").
 * Control flow matches ww-echo; transport is RC for paper fidelity
 * (ww-echo allows --use_uc; Fig.2 diagram specifies RC).
 *
 * Sequence + timeout/retry kept as a backstop so collect never hangs if the
 * peer dies mid-run (RC normally delivers; retries should be rare).
 */
static void run_echo(struct cfg *c) {
  enum ibv_qp_type qpt = IBV_QPT_RC;
  const int flag_timeout_us = 2000;
  const int max_tries = 64;
  int use_inl = 1;
  int inl = vt_inline_grant(c->size, use_inl, 0);

  struct vt_ctx v;
  vt_open_device(&v, c->dev, 1, c->gid_index);
  int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
  vt_alloc_buf(&v, VT_BUF_SIZE, access);
  struct ibv_qp *qp = vt_create_qp(&v, qpt, inl, NULL, NULL);
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

  /* Slot 0 is the doorbell byte (same idea as ww-echo conn_buf[0]). */
  volatile uint8_t *flag = v.buf;
  *flag = 0;

  struct ibv_send_wr wr;
  struct ibv_sge sge;
  struct ibv_wc wc;

  if (c->is_server) {
    printf("fig2 echo server size=%d inline=%d\n", c->size, use_inl);
    fflush(stdout);
    uint8_t *payload = v.buf + VT_CACHELINE;
    memset(payload, 1, (size_t)c->size);
    static uint64_t nb;
    for (;;) {
      uint8_t seq;
      /* Wait for a non-zero seq (client never uses 0 as seq). */
      while ((seq = *flag) == 0) {
      }
      *flag = 0;

      memset(&wr, 0, sizeof(wr));
      memset(&sge, 0, sizeof(sge));
      sge.addr = (uintptr_t)payload;
      sge.length = (uint32_t)c->size;
      sge.lkey = v.mr->lkey;
      wr.opcode = IBV_WR_RDMA_WRITE;
      wr.send_flags = 0;
      if (use_inl && c->size <= inl)
        wr.send_flags |= IBV_SEND_INLINE;
      if (vt_should_signal(nb++, 64))
        wr.send_flags |= IBV_SEND_SIGNALED;
      wr.sg_list = &sge;
      wr.num_sge = 1;
      /* Echo seq into client's flag byte (remote WRITE starts at remote.addr). */
      payload[0] = seq;
      wr.wr.rdma.remote_addr = remote.addr;
      wr.wr.rdma.rkey = remote.rkey;
      struct ibv_send_wr *bad = NULL;
      VT_CHECK(ibv_post_send(qp, &wr, &bad) == 0, "post");
      if (wr.send_flags & IBV_SEND_SIGNALED)
        vt_poll_cq(v.cq, 1);
    }
  }

  /* client */
  uint8_t *req = v.buf + VT_CACHELINE;
  memset(req, 1, (size_t)c->size);
  double sum = 0, minv = 1e9, maxv = 0;
  int total = c->warmup + c->iters;
  int retries = 0;
  static uint64_t nb;
  uint8_t seq_gen = 0;

  for (int i = 0; i < total; i++) {
    int ok = 0;
    uint64_t t0 = 0, t1 = 0;

    for (int try = 0; try < max_tries; try++) {
      uint8_t seq = ++seq_gen;
      if (seq == 0)
        seq = ++seq_gen; /* never use 0 */

      *flag = 0;
      req[0] = seq;

      memset(&wr, 0, sizeof(wr));
      memset(&sge, 0, sizeof(sge));
      sge.addr = (uintptr_t)req;
      sge.length = (uint32_t)c->size;
      sge.lkey = v.mr->lkey;
      wr.opcode = IBV_WR_RDMA_WRITE;
      wr.send_flags = 0;
      if (use_inl && c->size <= inl)
        wr.send_flags |= IBV_SEND_INLINE;
      int sig = vt_should_signal(nb++, 64);
      if (sig)
        wr.send_flags |= IBV_SEND_SIGNALED;
      wr.sg_list = &sge;
      wr.num_sge = 1;
      wr.wr.rdma.remote_addr = remote.addr;
      wr.wr.rdma.rkey = remote.rkey;

      t0 = vt_ns();
      struct ibv_send_wr *bad = NULL;
      VT_CHECK(ibv_post_send(qp, &wr, &bad) == 0, "post");
      if (sig)
        VT_CHECK(vt_poll_cq_one(v.cq, &wc, 5000) == 0, "echo sig poll");

      if (wait_flag_eq(flag, seq, flag_timeout_us) == 0) {
        t1 = vt_ns();
        *flag = 0;
        ok = 1;
        if (try > 0)
          retries += try;
        break;
      }
      /* No reply in time — clear stale flag and retransmit. */
      *flag = 0;
    }

    if (!ok)
      VT_DIE("echo: no response after retries (peer stuck?)");

    if (i >= c->warmup) {
      double us = (t1 - t0) / 1000.0;
      sum += us;
      if (us < minv)
        minv = us;
      if (us > maxv)
        maxv = us;
    }
  }
  double avg = sum / c->iters;
  printf("fig2 mode=echo size=%d  rtt_avg=%.3f us  rtt/2=%.3f us  "
         "min_rtt=%.3f max_rtt=%.3f  retries=%d\n",
         c->size, avg, avg / 2.0, minv, maxv, retries);

  ibv_destroy_qp(qp);
  vt_close_device(&v);
}

int main(int argc, char **argv) {
  struct cfg c;
  parse(argc, argv, &c);
  if (strcmp(c.mode, "echo") == 0)
    run_echo(&c);
  else
    run_onesided(&c);
  return 0;
}
