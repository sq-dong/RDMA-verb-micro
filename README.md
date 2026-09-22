# rdma_verb_test

Microbenchmarks that reproduce Section 3, Figures 2–6 of HERD / SIGCOMM’14
(*Using RDMA Efficiently for Key-Value Services*).

Measurement semantics follow the author’s later `rdma_bench` suite (ATC’16).
Peers exchange QP metadata over TCP instead of memcached / `libhrd`, which fits
a typical RoCE lab setup.

For reproduction details, result analysis, per-figure design notes, and operational
caveats, see [docs/REPRODUCTION.md](docs/REPRODUCTION.md).

## Mapping to `rdma_bench`

| Paper figure | Binary | Template |
|--------------|--------|----------|
| Fig. 2 latency | `fig2_latency` | `rw-tput-receiver/main.cc`; ECHO polling in `ww-echo/{client,server}.c` |
| Fig. 3 inbound | `fig3_inbound` | `rw-tput-receiver/main.cc`, `run-servers.sh` / `run-machine.sh` |
| Fig. 4 outbound | `fig4_outbound` | `rw-tput-sender/main.cc`; UD SEND in `ud-sender/main.cc` |
| Fig. 5 ECHO | `fig5_echo` | `ww-echo`, `ws-echo`; SEND/SEND patterned on UD messaging |
| Fig. 6 QP scaling | `fig6_scale` | `sender-scalability/main.cc`; UD fan-out as in `ud-sender` |

`rdma_bench/herd/` and the old `HERD/` repo contain only the full KV system, not
these microbenchmarks.

## Build

```bash
cd ~/rdma_verb_test
make
```

Dependencies: `libibverbs`, pthread, Python 3, `matplotlib`, `numpy`.

## Machine layout

| Role | Host | Device / netdev | IP |
|------|------|-----------------|-----|
| server | smartx-server02 | `mlx5_0` / `enp59s0f0np0` | `10.0.0.20` |
| client | smartx-server03 | `mlx5_3` / `enp24s0f0np0` | `10.0.0.21` |
| client2 optional | thoth | `mlx5_1` / `ens7f1np1` | `10.0.0.22` |

Use the RDMA NIC IP for `-a`, never the management address. Pick `-x` from
`show_gids` for the RoCEv2 row that matches that IPv4. All RDMA ports must share
the same subnet and a consistent MTU.

Payload axes and inline policy live in `scripts/paper_config.sh`. Throughput paths
use `vt_inline_grant(size)` so create-time inline stays near `HRD_MAX_INLINE` 60,
not the NIC ceiling.

## Collect and plot

```bash
source ./scripts/setup_machine.sh
./scripts/collect_fig2.sh
./scripts/collect_fig3.sh
./scripts/collect_fig4.sh
./scripts/collect_fig5.sh
./scripts/collect_fig6.sh
# or: ./scripts/collect_all.sh
```

Each collector writes CSV and PNG/PDF under `results/`, then removes intermediate
logs. Re-plot without re-running:

```bash
python3 scripts/plot_paper_figs.py --fig all --results-dir results
```

## Layout

- `common.h` / `common.c`: TCP exchange, RC/UC/UD bring-up, RoCE GID AH, timing, CQ poll
- `fig*.c`: one binary per figure
- `scripts/collect_fig*.sh`: sweep, CSV, plot
- `scripts/plot_paper_figs.py`: paper-style figures
- `docs/REPRODUCTION.md`: full reproduction notes and result discussion
