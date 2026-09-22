#!/usr/bin/env python3
"""
Plot HERD Sec.3 Figures 2–6 from CSV under results/.

Current HW-native axis (CX-5 mlx5_0): payload through 4096; inline cliff ~828/956.
Old paper ticks (kept for reference):
  # Fig.2 / Fig.3: 4 8 16 32 64 128 256 512 1024
  # Fig.4:         0 64 128 192 256
  # Fig.6:         0 4 8 12 16

Usage:
  python3 scripts/plot_paper_figs.py --fig all --results-dir results
  python3 scripts/plot_paper_figs.py --fig 2 --demo
"""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

STYLE = {
    "font.size": 11,
    "axes.labelsize": 12,
    "axes.titlesize": 12,
    "legend.fontsize": 9,
    "xtick.labelsize": 10,
    "ytick.labelsize": 10,
    "axes.grid": True,
    "grid.alpha": 0.35,
    "grid.linestyle": "--",
    "figure.dpi": 140,
    "savefig.dpi": 200,
    "savefig.bbox": "tight",
}

# HW-native payload axis (CX-5); old paper ticks kept as comment only.
# TICKS_FIG2_3 = [4, 8, 16, 32, 64, 128, 256, 512, 1024]
DEFAULT_SIZES_FULL = [4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096]
DEFAULT_SIZES_OUT = [4, 8, 16, 32, 64, 128, 256, 512, 828, 956, 1024, 2048, 4096]
# denser defaults (kept):
# DEFAULT_SIZES_FULL = [4, 8, 16, 32, 64, 128, 256, 512, 828, 1024, 2048, 4096]
# DEFAULT_SIZES_OUT = [
#     4, 8, 16, 32, 48, 64, 96, 128, 192, 256, 320, 384, 512, 640, 768, 828, 956, 1024, 2048, 4096
# ]
TICKS_FIG6 = [0, 4, 8, 12, 16]

COLORS = {
    "write": "#1f77b4",
    "write_inl": "#d62728",
    "read": "#2ca02c",
    "echo": "#9467bd",
    "echo_rtt": "#8c564b",
    "WRITE-UC": "#1f77b4",
    "WRITE-RC": "#ff7f0e",
    "READ-RC": "#2ca02c",
    "WR-UC-INLINE": "#d62728",
    "SEND-UD": "#17becf",
    "In-WRITE-UC": "#1f77b4",
    "Out-WRITE-UC": "#d62728",
    "Out-SEND-UD": "#2ca02c",
}

MARKERS = {
    "write": "o",
    "write_inl": "s",
    "read": "^",
    "echo": "D",
    "echo_rtt": "v",
    "WRITE-UC": "o",
    "WRITE-RC": "s",
    "READ-RC": "^",
    "WR-UC-INLINE": "D",
    "SEND-UD": "v",
    "In-WRITE-UC": "o",
    "Out-WRITE-UC": "s",
    "Out-SEND-UD": "^",
}


def _read_csv(path: Path):
    if not path.exists():
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def _series(rows, key_field, x_field, y_field):
    out = defaultdict(list)
    for r in rows:
        try:
            out[r[key_field]].append((float(r[x_field]), float(r[y_field])))
        except (KeyError, ValueError):
            continue
    for k in out:
        out[k].sort(key=lambda t: t[0])
    return out


def _equal_x(xs_vals, tick_order):
    """Map payload/process values to equally spaced x indices (paper-style)."""
    index = {float(v): i for i, v in enumerate(tick_order)}
    xpos, labels = [], []
    for v in xs_vals:
        if float(v) not in index:
            # Append unknown sizes at the end in sorted order among unknowns
            index[float(v)] = len(index)
        xpos.append(index[float(v)])
    labels = [str(int(v)) if float(v) == int(v) else str(v) for v in tick_order]
    return xpos, labels, len(tick_order)


def _plot_equal_spaced(ax, pts, tick_order, **plot_kw):
    """Plot (x_value, y) with equal spacing using tick_order as category axis."""
    if not pts:
        return
    pts = sorted(pts, key=lambda t: t[0])
    # Only keep points that appear on the paper tick list (or all if not in list)
    xs_vals = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    idx_map = {float(v): i for i, v in enumerate(tick_order)}
    xs, ys2 = [], []
    for x, y in zip(xs_vals, ys):
        if float(x) in idx_map:
            xs.append(idx_map[float(x)])
            ys2.append(y)
    if not xs:
        return
    ax.plot(xs, ys2, **plot_kw)


def _finish_equal_x(ax, tick_order, xlabel):
    ax.set_xticks(range(len(tick_order)))
    ax.set_xticklabels([str(int(t)) if float(t) == int(t) else str(t) for t in tick_order])
    ax.set_xlim(-0.3, len(tick_order) - 0.7)
    ax.set_xlabel(xlabel)


def _save(fig, out_base: Path):
    out_base.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(str(out_base) + ".pdf")
    fig.savefig(str(out_base) + ".png")
    plt.close(fig)
    print(f"wrote {out_base}.pdf / .png")


def _x_ticks_from_rows(rows, field: str, fallback: list[int]) -> list[int]:
    if not rows:
        return list(fallback)
    xs = sorted({int(float(r[field])) for r in rows if r.get(field) not in (None, "")})
    return xs if xs else list(fallback)


def plot_fig2(results: Path, demo: bool = False):
    path = results / "fig2.csv"
    if demo and not path.exists():
        rows = []
        for size in DEFAULT_SIZES_FULL:
            base = 1.8 + size / 800.0
            rows.append(
                {
                    "mode": "write",
                    "size": size,
                    "avg_us": base + 0.15,
                    "min_us": 0,
                    "max_us": 0,
                    "rtt_us": "",
                    "half_rtt_us": "",
                }
            )
            if size <= 828:
                rows.append(
                    {
                        "mode": "write_inl",
                        "size": size,
                        "avg_us": base - 0.35,
                        "min_us": 0,
                        "max_us": 0,
                        "rtt_us": "",
                        "half_rtt_us": "",
                    }
                )
                rows.append(
                    {
                        "mode": "echo",
                        "size": size,
                        "avg_us": base + 0.1,
                        "min_us": 0,
                        "max_us": 0,
                        "rtt_us": (base + 0.1) * 2,
                        "half_rtt_us": base + 0.1,
                    }
                )
                rows.append(
                    {
                        "mode": "echo_rtt",
                        "size": size,
                        "avg_us": (base + 0.1) * 2,
                        "min_us": 0,
                        "max_us": 0,
                        "rtt_us": (base + 0.1) * 2,
                        "half_rtt_us": "",
                    }
                )
            rows.append(
                {
                    "mode": "read",
                    "size": size,
                    "avg_us": base + 0.2,
                    "min_us": 0,
                    "max_us": 0,
                    "rtt_us": "",
                    "half_rtt_us": "",
                }
            )
    else:
        rows = _read_csv(path)

    ticks = _x_ticks_from_rows(rows, "size", DEFAULT_SIZES_FULL)
    series = _series(rows, "mode", "size", "avg_us")
    echo_half = []
    echo_rtt = []
    for r in rows:
        if r.get("mode") == "echo" and r.get("half_rtt_us"):
            echo_half.append((float(r["size"]), float(r["half_rtt_us"])))
        if r.get("mode") == "echo_rtt" and r.get("avg_us"):
            echo_rtt.append((float(r["size"]), float(r["avg_us"])))
        elif r.get("mode") == "echo" and r.get("rtt_us"):
            echo_rtt.append((float(r["size"]), float(r["rtt_us"])))

    fig, ax = plt.subplots(figsize=(5.2, 3.6))
    labels = {"write": "WRITE", "write_inl": "WR-INLINE", "read": "READ"}
    for mode in ("write", "write_inl", "read"):
        pts = series.get(mode, [])
        _plot_equal_spaced(
            ax,
            pts,
            ticks,
            marker=MARKERS[mode],
            color=COLORS[mode],
            label=labels[mode],
            linewidth=1.6,
        )
    if echo_rtt:
        _plot_equal_spaced(
            ax,
            echo_rtt,
            ticks,
            marker="D",
            color=COLORS["echo"],
            label="ECHO",
            linewidth=1.6,
        )
    if echo_half:
        _plot_equal_spaced(
            ax,
            echo_half,
            ticks,
            marker="v",
            color=COLORS["echo_rtt"],
            label="ECHO / 2",
            linewidth=1.6,
            linestyle="--",
        )

    ax.set_ylabel("Latency (µs)")
    ax.set_title("Figure 2: Latency of verbs and ECHO operations")
    _finish_equal_x(ax, ticks, "Size of payload (bytes)")
    ax.set_ylim(bottom=0)
    ax.legend(loc="best", frameon=True)
    _save(fig, results / "fig2_latency")


def plot_fig3(results: Path, demo: bool = False):
    path = results / "fig3.csv"
    if demo and not path.exists():
        rows = []
        for size in DEFAULT_SIZES_FULL:
            rows.append({"curve": "WRITE-UC", "size": size, "mops": max(5, 35 - size / 40)})
            rows.append({"curve": "WRITE-RC", "size": size, "mops": max(4, 32 - size / 35)})
            rows.append({"curve": "READ-RC", "size": size, "mops": max(3, 26 - size / 50)})
    else:
        rows = _read_csv(path)

    ticks = _x_ticks_from_rows(rows, "size", DEFAULT_SIZES_FULL)
    series = _series(rows, "curve", "size", "mops")
    fig, ax = plt.subplots(figsize=(5.2, 3.6))
    for name in ("WRITE-UC", "WRITE-RC", "READ-RC"):
        pts = series.get(name, [])
        _plot_equal_spaced(
            ax,
            pts,
            ticks,
            marker=MARKERS.get(name, "o"),
            color=COLORS.get(name, None),
            label=name,
            linewidth=1.6,
        )
    ax.set_ylabel("Throughput (Mops)")
    ax.set_title("Figure 3: Inbound verbs throughput")
    _finish_equal_x(ax, ticks, "Size of payload (bytes)")
    ax.set_ylim(bottom=0)
    ax.legend(loc="best")
    _save(fig, results / "fig3_inbound")


def plot_fig4(results: Path, demo: bool = False):
    path = results / "fig4.csv"
    measure = list(DEFAULT_SIZES_OUT)
    if demo and not path.exists():
        rows = []
        for size in measure:
            if size <= 828:
                rows.append({"curve": "WR-UC-INLINE", "size": size, "mops": max(8, 38 - size / 8)})
                rows.append({"curve": "SEND-UD", "size": size, "mops": max(7, 36 - size / 7)})
            rows.append({"curve": "WRITE-UC", "size": size, "mops": max(6, 28 - size / 12)})
            rows.append({"curve": "READ-RC", "size": size, "mops": max(5, 22 - size / 30)})
    else:
        rows = _read_csv(path)

    series = _series(rows, "curve", "size", "mops")
    ticks_fig4_meas = _x_ticks_from_rows(rows, "size", measure)
    fig, ax = plt.subplots(figsize=(5.8, 3.6))
    for name in ("WR-UC-INLINE", "SEND-UD", "WRITE-UC", "READ-RC"):
        pts = series.get(name, [])
        _plot_equal_spaced(
            ax,
            pts,
            ticks_fig4_meas,
            marker=MARKERS.get(name, "o"),
            color=COLORS.get(name, None),
            label=name,
            linewidth=1.6,
        )
    ax.set_ylabel("Throughput (Mops)")
    ax.set_title("Figure 4: Outbound verbs throughput")
    _finish_equal_x(ax, ticks_fig4_meas, "Size of payload (bytes)")
    ax.set_ylim(bottom=0)
    ax.legend(loc="best")
    _save(fig, results / "fig4_outbound")


def plot_fig5(results: Path, demo: bool = False):
    path = results / "fig5.csv"
    opts = ["basic", "+unreliable", "+unsignalled", "+inlined"]
    types = ["SEND/SEND", "WR/WR", "WR/SEND"]
    if demo and not path.exists():
        base = {
            ("SEND/SEND", "basic"): 4,
            ("SEND/SEND", "+unreliable"): 10,
            ("SEND/SEND", "+unsignalled"): 16,
            ("SEND/SEND", "+inlined"): 21,
            ("WR/WR", "basic"): 5,
            ("WR/WR", "+unreliable"): 12,
            ("WR/WR", "+unsignalled"): 20,
            ("WR/WR", "+inlined"): 26,
            ("WR/SEND", "basic"): 5,
            ("WR/SEND", "+unreliable"): 12,
            ("WR/SEND", "+unsignalled"): 20,
            ("WR/SEND", "+inlined"): 26,
        }
        rows = [
            {"echo_type": t, "opt": o, "mops": base[(t, o)]} for t in types for o in opts
        ]
    else:
        rows = _read_csv(path)

    data = {(r["echo_type"], r["opt"]): float(r["mops"]) for r in rows if r.get("mops")}

    fig, ax = plt.subplots(figsize=(6.2, 3.8))
    x = np.arange(len(types))
    width = 0.18
    palette = ["#4d4d4d", "#1f77b4", "#ff7f0e", "#2ca02c"]
    for i, opt in enumerate(opts):
        ys = [data.get((t, opt), 0.0) for t in types]
        ax.bar(
            x + (i - 1.5) * width,
            ys,
            width,
            label=opt,
            color=palette[i],
            edgecolor="black",
            linewidth=0.4,
        )

    ax.set_xticks(x)
    ax.set_xticklabels(types)
    ax.set_ylabel("Throughput (Mops)")
    ax.set_title("Figure 5: Throughput of ECHOs with 32 byte messages")
    ax.legend(loc="upper left", ncol=2)
    ax.set_ylim(bottom=0)
    _save(fig, results / "fig5_echo")


def plot_fig6(results: Path, demo: bool = False):
    path = results / "fig6.csv"
    measure = [1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24]
    if demo and not path.exists():
        rows = []
        for n in measure:
            rows.append({"curve": "In-WRITE-UC", "n": n, "mops": 30 - n * 0.2})
            rows.append(
                {
                    "curve": "Out-WRITE-UC",
                    "n": n,
                    "mops": max(5, 28 * (0.55 ** (n / 8))),
                }
            )
            rows.append({"curve": "Out-SEND-UD", "n": n, "mops": 27 - n * 0.3})
    else:
        rows = _read_csv(path)

    series = _series(rows, "curve", "n", "mops")
    present = sorted({int(float(r["n"])) for r in rows}) if rows else measure
    ticks_n = present if present else measure
    fig, ax = plt.subplots(figsize=(5.2, 3.6))
    for name in ("In-WRITE-UC", "Out-WRITE-UC", "Out-SEND-UD"):
        pts = series.get(name, [])
        _plot_equal_spaced(
            ax,
            pts,
            ticks_n,
            marker=MARKERS.get(name, "o"),
            color=COLORS.get(name, None),
            label=name,
            linewidth=1.6,
        )
    ax.set_ylabel("Throughput (Mops)")
    ax.set_title("Figure 6: UD vs UC for all-to-all (32 byte payloads)")
    _finish_equal_x(
        ax, ticks_n, "Number of client processes (= number of server processes)"
    )
    ax.set_ylim(bottom=0)
    ax.legend(loc="best")
    _save(fig, results / "fig6_scale")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fig", default="all", help="2|3|4|5|6|all")
    ap.add_argument("--results-dir", default="results")
    ap.add_argument(
        "--demo",
        action="store_true",
        help="If CSV missing, synthesize paper-shaped demo curves",
    )
    args = ap.parse_args()
    results = Path(args.results_dir)
    results.mkdir(parents=True, exist_ok=True)

    plt.rcParams.update(STYLE)

    figs = ["2", "3", "4", "5", "6"] if args.fig == "all" else [args.fig]
    dispatch = {
        "2": plot_fig2,
        "3": plot_fig3,
        "4": plot_fig4,
        "5": plot_fig5,
        "6": plot_fig6,
    }
    for f in figs:
        if f not in dispatch:
            raise SystemExit(f"unknown fig {f}")
        dispatch[f](results, demo=args.demo)


if __name__ == "__main__":
    main()
