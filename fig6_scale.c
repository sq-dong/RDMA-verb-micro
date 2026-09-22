/*
 * fig6_scale.c — HERD Sec.3 Fig.6: true multi-process all-to-all.
 *
 * Paper: N client procs = N server procs; all-to-all ⇒ N² QPs at RNICS.
 * Each process uses -I ID (0..N-1) and -q N.  Per process: N QPs to the N peers
 * (not one process owning N² QPs).
 *
 *   Out-WRITE: N MS procs each post UC WRITE to N clients (N² at MS)
 *   In-WRITE:  N client procs each WRITE to N MS procs (N² at MS)
 *   Out-SEND:  1 MS proc (must -I 0) with 1 UD QP + N AHs; N client procs
 *
 * Bootstrap: process ID listens on (-p + ID); peers connect and exchange
 * one endpoint each (first TCP byte = peer id).
 *
 * Templates: sender-scalability (WRITE), ud-sender (SEND).
 */

#include "common.h"

#include <getopt.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

enum fig6_mode {
  FIG6_OUT_WRITE = 0,
  FIG6_IN_WRITE,
  FIG6_OUT_SEND_UD,
};

struct cfg {
  int is_server; /* TCP role: listen (-s) vs connect (-c) */
  char *dev;
  char *ip;
  uint16_t port; /* base port; process id listens on port+id */
  int gid_index;
  int size;
  int paper_n; /* -q : paper N */
  int proc_id; /* -I : 0 .. N-1 */
  int unsig;
  int postlist;
  int duration;
  int use_inline;
  int use_uc;
  enum fig6_mode mode;
};

static void usage(const char *a) {
  fprintf(stderr,
          "Usage: %s -s|-c -d DEV -a IP -I ID -q N [-p PORT] "
          "[-M out-write|in-write|out-send-ud] [-t POSTLIST] [-l SIZE] "
          "[-Q UNSIG] [-D SEC] [--no-inline] [--rc]\n"
          "  -q N : paper N (#procs each side)\n"
          "  -I   : this process id in 0..N-1\n"
          "  -p   : base TCP port (this process listens on port+ID when -s)\n",
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
  c->paper_n = 1;
  c->proc_id = 0;
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
  while ((opt = getopt_long(argc, argv, "scd:a:p:x:l:q:I:t:Q:D:M:h", longopts,
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
      c->paper_n = atoi(optarg);
      break;
    case 'I':
      c->proc_id = atoi(optarg);
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
  if (c->paper_n < 1 || c->paper_n > VT_MAX_QPS) {
    fprintf(stderr, "paper N (-q) must be 1..%d\n", VT_MAX_QPS);
    exit(1);
  }
  if (c->proc_id < 0 || c->proc_id >= c->paper_n) {
    fprintf(stderr, "-I id must be in 0..N-1 (N=%d)\n", c->paper_n);
    exit(1);
  }
  if (c->mode == FIG6_OUT_SEND_UD && c->is_server && c->proc_id != 0) {
    fprintf(stderr, "Out-SEND: only -I 0 posts on the MS side\n");
    exit(1);
  }
  if (c->unsig < 1)
    c->unsig = 1;
  if (c->postlist < 1)
    c->postlist = 1;
  if (c->postlist > 64)
    c->postlist = 64;
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

/* Listen only (do not accept) — vt_tcp_listen accepts once and closes listen fd. */
static int tcp_listen_only(const char *ip, uint16_t port, int backlog) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  VT_CHECK(fd >= 0, "socket");
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  VT_CHECK(inet_pton(AF_INET, ip, &addr.sin_addr) == 1, "inet_pton");
  VT_CHECK(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0, "bind");
  VT_CHECK(listen(fd, backlog) == 0, "listen");
  return fd;
}

/* Listener: accept N peers, each sends 1-byte peer id then exchanges EP. */
static void mesh_listen_exchange(struct cfg *c, struct vt_endpoint *locals,
                                 struct vt_endpoint *remotes, int n_peer,
                                 int n_local_qp) {
  int lfd = tcp_listen_only(c->ip, (uint16_t)(c->port + c->proc_id),
                            n_peer + 4);
  int got = 0;
  while (got < n_peer) {
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int fd = accept(lfd, (struct sockaddr *)&addr, &alen);
    VT_CHECK(fd >= 0, "accept");
    uint8_t peer = 0xff;
    VT_CHECK(read(fd, &peer, 1) == 1, "peer id");
    VT_CHECK(peer < (uint8_t)n_peer, "peer id range");
    int li = (n_local_qp == 1) ? 0 : (int)peer;
    vt_tcp_exchange(fd, &locals[li], &remotes[peer]);
    close(fd);
    got++;
  }
  close(lfd);
}

/* Connector: for each peer server id, connect to port+peer and exchange. */
static void mesh_connect_exchange(struct cfg *c, struct vt_endpoint *locals,
                                  struct vt_endpoint *remotes, int n_peer,
                                  int n_local_qp) {
  for (int peer = 0; peer < n_peer; peer++) {
    int fd = -1;
    for (int try = 0; try < 600; try++) {
      fd = socket(AF_INET, SOCK_STREAM, 0);
      VT_CHECK(fd >= 0, "socket");
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons((uint16_t)(c->port + peer));
      VT_CHECK(inet_pton(AF_INET, c->ip, &addr.sin_addr) == 1, "inet_pton");
      if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        break;
      close(fd);
      fd = -1;
      usleep(50000);
    }
    VT_CHECK(fd >= 0, "connect peers");
    uint8_t me = (uint8_t)c->proc_id;
    VT_CHECK(write(fd, &me, 1) == 1, "send id");
    int li = (n_local_qp == 1) ? 0 : peer;
    vt_tcp_exchange(fd, &locals[li], &remotes[peer]);
    close(fd);
  }
}

static int i_am_sender(const struct cfg *c) {
  switch (c->mode) {
  case FIG6_OUT_WRITE:
  case FIG6_OUT_SEND_UD:
    return c->is_server; /* MS posts */
  case FIG6_IN_WRITE:
    return !c->is_server; /* clients post toward MS */
  }
  return 0;
}

int main(int argc, char **argv) {
  struct cfg c;
  parse(argc, argv, &c);
  setvbuf(stdout, NULL, _IONBF, 0);

  const int is_ud = (c.mode == FIG6_OUT_SEND_UD);
  const int sender = i_am_sender(&c);
  const int N = c.paper_n;

  /*
   * Per-process QP counts (paper all-to-all):
   *   WRITE modes: each proc has N QPs (one per peer) → N procs × N = N²
   *   Out-SEND MS: 1 QP; each client proc: 1 QP; MS builds N AHs
   */
  int n_local;
  int n_peer = N;
  if (is_ud && sender)
    n_local = 1;
  else if (is_ud && !sender)
    n_local = 1;
  else
    n_local = N;

  enum ibv_qp_type qpt =
      is_ud ? IBV_QPT_UD : (c.use_uc ? IBV_QPT_UC : IBV_QPT_RC);
  const int inl_cap = vt_inline_grant(c.size, c.use_inline, is_ud);
  const int sq_depth = 128; /* match sender-scalability small SQ */
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
    scqs[i] = ibv_create_cq(v.ctx, 512, NULL, NULL, 0);
    VT_CHECK(scqs[i], "scq");
    if (is_ud) {
      rcqs[i] = ibv_create_cq(v.ctx, 512, NULL, NULL, 0);
      VT_CHECK(rcqs[i], "rcq");
    } else {
      rcqs[i] = scqs[i];
    }
    qps[i] = create_qp(&v, scqs[i], rcqs[i], qpt, inl_cap, sq_depth,
                       is_ud ? rq_depth : 16);
    psns[i] = (uint32_t)((vt_ns() + (uint64_t)i * 9973 +
                          (uint64_t)c.proc_id * 7919) &
                         0xffffff);
    vt_fill_local_ep(&v, qps[i], psns[i], &locals[i]);
    locals[i].addr = (uint64_t)(uintptr_t)(v.buf + (size_t)i * 4096);
    vt_qp_to_init(qps[i], v.port);
  }

  if (c.is_server) {
    mesh_listen_exchange(&c, locals, remotes, n_peer, n_local);
  } else if (is_ud) {
    /* Out-SEND: every client connects only to MS process 0. */
    int fd = -1;
    for (int try = 0; try < 600; try++) {
      fd = socket(AF_INET, SOCK_STREAM, 0);
      VT_CHECK(fd >= 0, "socket");
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(c.port); /* MS -I 0 */
      VT_CHECK(inet_pton(AF_INET, c.ip, &addr.sin_addr) == 1, "inet_pton");
      if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        break;
      close(fd);
      fd = -1;
      usleep(50000);
    }
    VT_CHECK(fd >= 0, "connect MS");
    uint8_t me = (uint8_t)c.proc_id;
    VT_CHECK(write(fd, &me, 1) == 1, "send id");
    vt_tcp_exchange(fd, &locals[0], &remotes[0]);
    close(fd);
  } else {
    mesh_connect_exchange(&c, locals, remotes, n_peer, n_local);
  }

  if (is_ud) {
    if (sender) {
      vt_qp_to_rtr(&v, qps[0], &remotes[0], qpt);
      vt_qp_to_rts(qps[0], psns[0]);
      for (int i = 0; i < n_peer; i++)
        ahs[i] = make_ah(&v, &remotes[i]);
    } else {
      vt_qp_to_rtr(&v, qps[0], &remotes[0], qpt);
      vt_qp_to_rts(qps[0], psns[0]);
    }
  } else {
    for (int i = 0; i < n_local; i++) {
      vt_qp_to_rtr(&v, qps[i], &remotes[i], qpt);
      vt_qp_to_rts(qps[i], psns[i]);
    }
  }

  if (!sender) {
    printf("fig6 passive N=%d id=%d nqp=%d mode=%d\n", N, c.proc_id, n_local,
           (int)c.mode);
    fflush(stdout);
    if (is_ud) {
      for (int r = 0; r < rq_depth / 2; r++)
        ud_post_recv(qps[0], &v, (uint64_t)r);
      for (;;) {
        struct ibv_wc wc[16];
        int n = ibv_poll_cq(rcqs[0], 16, wc);
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

  const int pl = c.postlist;
  struct ibv_send_wr wr[64], *bad;
  struct ibv_sge sge[64];
  uint64_t nb_tx_qp[VT_MAX_QPS];
  memset(nb_tx_qp, 0, sizeof(nb_tx_qp));
  uint64_t ops = 0;
  uint64_t seed = 0x12345678abcdefull ^ ((uint64_t)c.proc_id << 17);

  printf("fig6 sender N=%d id=%d nqp=%d size=%d inl=%d postlist=%d unsig=%d\n",
         N, c.proc_id, is_ud ? 1 : n_local, c.size, inl_cap, pl, unsig);
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
        int dest = (int)(seed % (uint64_t)n_peer);
        seed = seed * 6364136223846793005ull + 1;
        memset(&wr[w], 0, sizeof(wr[w]));
        memset(&sge[w], 0, sizeof(sge[w]));
        wr[w].opcode = IBV_WR_SEND;
        wr[w].num_sge = 1;
        wr[w].sg_list = &sge[w];
        wr[w].next = (w == pl - 1) ? NULL : &wr[w + 1];
        wr[w].send_flags =
            vt_should_signal(nb_tx_qp[0], unsig) ? IBV_SEND_SIGNALED : 0;
        if (vt_should_signal(nb_tx_qp[0], unsig) && nb_tx_qp[0] > 0)
          poll_one(scqs[0]);
        if (c.use_inline && c.size <= inl_cap)
          wr[w].send_flags |= IBV_SEND_INLINE;
        sge[w].addr = (uintptr_t)v.buf;
        sge[w].length = (uint32_t)c.size;
        sge[w].lkey = v.mr->lkey;
        wr[w].wr.ud.ah = ahs[dest];
        wr[w].wr.ud.remote_qpn = remotes[dest].qpn;
        wr[w].wr.ud.remote_qkey = 0x11111111;
        nb_tx_qp[0]++;
      }
      VT_CHECK(ibv_post_send(qps[0], &wr[0], &bad) == 0, "post");
      ops += (uint64_t)pl;
    } else {
      /* Round-robin across this process's N peer QPs (like sender-scalability). */
      int qi = (int)(seed % (uint64_t)n_local);
      seed = seed * 6364136223846793005ull + 1;
      for (int w = 0; w < pl; w++) {
        memset(&wr[w], 0, sizeof(wr[w]));
        memset(&sge[w], 0, sizeof(sge[w]));
        wr[w].opcode = IBV_WR_RDMA_WRITE;
        wr[w].num_sge = 1;
        wr[w].sg_list = &sge[w];
        wr[w].next = (w == pl - 1) ? NULL : &wr[w + 1];
        wr[w].send_flags =
            vt_should_signal(nb_tx_qp[qi], unsig) ? IBV_SEND_SIGNALED : 0;
        if (vt_should_signal(nb_tx_qp[qi], unsig) && nb_tx_qp[qi] > 0)
          poll_one(scqs[qi]);
        if (c.use_inline && c.size <= inl_cap)
          wr[w].send_flags |= IBV_SEND_INLINE;
        sge[w].addr = (uintptr_t)(v.buf + (size_t)qi * 64);
        sge[w].length = (uint32_t)c.size;
        sge[w].lkey = v.mr->lkey;
        wr[w].wr.rdma.remote_addr = remotes[qi].addr;
        wr[w].wr.rdma.rkey = remotes[qi].rkey;
        nb_tx_qp[qi]++;
      }
      VT_CHECK(ibv_post_send(qps[qi], &wr[0], &bad) == 0, "post");
      ops += (uint64_t)pl;
    }
  }

  double sec = (vt_ns() - t0) / 1e9;
  if (sec < 1e-6)
    sec = 1e-6;
  double mops = ops / sec / 1e6;
  if (c.mode == FIG6_OUT_WRITE)
    printf("fig6 Out-WRITE: %.2f Mops  N=%d id=%d nqp=%d size=%d\n", mops, N,
           c.proc_id, n_local, c.size);
  else if (c.mode == FIG6_IN_WRITE)
    printf("fig6 In-WRITE: %.2f Mops  N=%d id=%d nqp=%d size=%d\n", mops, N,
           c.proc_id, n_local, c.size);
  else
    printf("fig6 Out-SEND: %.2f Mops  N=%d id=%d ndest=%d size=%d\n", mops, N,
           c.proc_id, n_peer, c.size);
  fflush(stdout);

  for (int i = 0; i < n_peer; i++) {
    if (ahs[i])
      ibv_destroy_ah(ahs[i]);
  }
  for (int i = 0; i < n_local; i++) {
    ibv_destroy_qp(qps[i]);
    if (rcqs[i] && rcqs[i] != scqs[i])
      ibv_destroy_cq(rcqs[i]);
    ibv_destroy_cq(scqs[i]);
  }
  vt_close_device(&v);
  return 0;
}
