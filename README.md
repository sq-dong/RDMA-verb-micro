# rdma_verb_test

Microbenchmarks that reproduce **Section 3 (Figures 2–6)** of HERD / SIGCOMM'14
(*Using RDMA Efficiently for Key-Value Services*).

The measurement logic follows the same author's later `rdma_bench` suite (ATC'16).
Connection setup does **not** use memcached / `libhrd`; peers exchange QP metadata
over a plain **TCP** socket, which fits a typical RoCE lab setup.

## 1. Mapping to `rdma_bench` templates

| Paper figure | Binary here | Template paths |
|--------------|-------------|----------------|
| Fig. 2 latency | `fig2_latency` | `rdma_bench/rw-tput-receiver/main.cc` (README: set `kAppUnsigBatch=1`, `postlist=1` for latency); ECHO memory polling in `rdma_bench/ww-echo/{client,server}.c` |
| Fig. 3 inbound throughput | `fig3_inbound` | `rdma_bench/rw-tput-receiver/main.cc` + `run-servers.sh` / `run-machine.sh` |
| Fig. 4 outbound throughput | `fig4_outbound` | WRITE/READ: `rdma_bench/rw-tput-sender/main.cc`; UD SEND: `rdma_bench/ud-sender/main.cc` |
| Fig. 5 ECHO | `fig5_echo` | WR/WR: `rdma_bench/ww-echo/{client,server}.c`; WR/SEND: `rdma_bench/ws-echo/{client,worker}.c` |
| Fig. 6 QP scaling | `fig6_scale` | `rdma_bench/sender-scalability/main.cc` (outbound multi-QP) |

Top-level notes: `rdma_bench/README.md` (benchmark table + selective-signaling section).

`rdma_bench/herd/` and the old `HERD/` repo contain **no** Fig. 2–6 microbenchmarks
(only the full KV system). Do not use them as the source for these figures.

## 2. Build

```bash
cd ~/rdma_verb_test
make
```

Dependencies: `libibverbs`, pthread, Python 3 + `matplotlib` + `numpy` (for plotting).

## 3. Machine layout (adjust IPs to your cluster)

| Role | Host | RDMA device / netdev | Suggested IP |
|------|------|----------------------|--------------|
| server | smartx-server02 | `mlx5_0` / `enp59s0f0np0` | `10.0.0.20` |
| client | smartx-server03 | `mlx5_3` / `enp24s0f0np0` | `10.0.0.21` |
| client2 (optional for Figs. 3/6) | thoth | `mlx5_1` / `ens7f1np1` | `10.0.0.22` |

All RDMA ports must share the same subnet and MTU. Always pass the **RDMA NIC IP**
to `-a`, never the management address (`192.168.100.x`).

Pick `-x` from `show_gids` for the RoCEv2 row that matches that IPv4 address.

Payload sweeps follow the paper axis ticks:

| Figure | X axis | Measured sizes |
|--------|--------|----------------|
| Fig. 2 | 4 … 1024 | WRITE / WR-INLINE / ECHO: `4..256`; READ: `4..1024` |
| Fig. 3 | 4 … 1024 | all curves: `4 8 16 32 64 128 256 512 1024` |
| Fig. 4 | 0 … 256 | `4 8 16 32 64 128 192 256` |
| Fig. 5 | (bars) | fixed **32** bytes |
| Fig. 6 | 0 … 16 processes | `n = 4 8 12 16` |

Each `collect_fig*.sh` writes CSV, plots PNG/PDF, then deletes `*.log` (keeps CSV and figures).

## 4. Automated collect and plot

```bash
# Load cluster / RDMA settings (edit scripts/setup_machine.sh if IPs/GIDs change)
source ./scripts/setup_machine.sh

# One figure at a time (writes CSV + PNG/PDF; deletes logs, keeps CSV)
./scripts/collect_fig2.sh
./scripts/collect_fig3.sh   # uses CLT_HOST + CLT_HOST2 if set
./scripts/collect_fig4.sh
./scripts/collect_fig5.sh
./scripts/collect_fig6.sh

# Or everything
./scripts/collect_all.sh
```

**Multi-client (paper Sec.3):** Fig.3 inbound and Fig.6 QP scaling need many
clients most; Fig.4 outbound also pairs one server proc per client. Fig.2 / Fig.5
are fine with one. With `CLT_HOST2=thoth` in `setup_machine.sh`, `collect_fig3.sh`
runs both clients in parallel and **sums** Mops as a stand-in for multi-client
inbound.

Outputs:

| CSV | Plot files |
|-----|------------|
| `results/fig2.csv` | `results/fig2_latency.{png,pdf}` |
| `results/fig3.csv` | `results/fig3_inbound.{png,pdf}` |
| `results/fig4.csv` | `results/fig4_outbound.{png,pdf}` |
| `results/fig5.csv` | `results/fig5_echo.{png,pdf}` |
| `results/fig6.csv` | `results/fig6_scale.{png,pdf}` |

Re-plot from existing CSV without re-running RDMA:

```bash
python3 scripts/plot_paper_figs.py --fig all --results-dir results
```

Preview plot style without measurements (synthetic paper-shaped curves):

```bash
python3 scripts/plot_paper_figs.py --fig all --demo --results-dir results
```

Plot titles / series names follow the SIGCOMM'14 captions
(WRITE, WR-INLINE, READ, ECHO, ECHO/2; inbound/outbound Mops; ECHO bars;
In-WRITE-UC / Out-WRITE-UC / Out-SEND-UD). Absolute numbers will differ on
ConnectX-5 RoCE vs the paper's ConnectX-3 InfiniBand; compare curve *shape*.

## 5. Manual quick start (start server first, then client)

```bash
./fig2_latency -s -d mlx5_0 -a 10.0.0.20 -p 18500 -x 4 -n 10000
./fig2_latency -c -d mlx5_3 -a 10.0.0.20 -p 18500 -x <gid> -n 10000 -l 64 -m write

./fig3_inbound -s -d mlx5_0 -a 10.0.0.20 -p 18510 -x 4
./fig3_inbound -c -d mlx5_3 -a 10.0.0.20 -p 18510 -x <gid> -l 32 -t 64 -Q 64 --uc --no-inline -D 5

./fig4_outbound -s -d mlx5_0 -a 10.0.0.20 -p 18520 -x 4 -R -l 32 -t 64 -Q 64 -m write_uc -D 5
./fig4_outbound -c -d mlx5_3 -a 10.0.0.20 -p 18520 -x <gid> -R -l 32 -t 64 -Q 64 -m write_uc -D 5

./fig5_echo -s -d mlx5_0 -a 10.0.0.20 -p 18530 -x 4 -m ws -l 32 -w 32 -D 5
./fig5_echo -c -d mlx5_3 -a 10.0.0.20 -p 18530 -x <gid> -m ws -l 32 -w 32 -D 5

./fig6_scale -s -d mlx5_0 -a 10.0.0.20 -p 18540 -x 4 -q 16 -l 32 -D 5
./fig6_scale -c -d mlx5_3 -a 10.0.0.20 -p 18540 -x <gid> -q 16 -l 32 -D 5
```

## 6. Paper knobs  program flags

| Paper term | Flag here |
|------------|-----------|
| UC | `fig3`: `--uc` / `--rc`; `fig4`: `-m write_uc`; echo WRITE path defaults to UC |
| UD SEND | `fig4 -m send_ud`; `fig5 -m ws\|ss` |
| inline | Fig.2 `write_inl` and Fig.4 small WRITE/SEND. Fig.3 WRITE/READ pass `--no-inline` |
| unsignaled / selective signaling | `-Q` (signal once every Q WRs) |
| outstanding window | `-t` (postlist) or `fig5 -w` |
| ECHO/2 | `fig2 -m echo` prints `rtt` and `rtt/2` |

## 7. Layout

- `common.h` / `common.c`: TCP exchange, RC/UC/UD bring-up, RoCE GID AH, timing, CQ poll
- `fig*.c`: one binary per figure
- `scripts/collect_fig*.sh`: sweep → CSV → plot
- `scripts/plot_paper_figs.py`: paper-style figures
- Unused alternatives are kept as full block comments, not deleted
