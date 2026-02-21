#!/usr/bin/env python3
"""Analyze multihost rank-sweep outputs and build an HTML dashboard."""

from __future__ import annotations

import argparse
import csv
import html
import math
import re
from collections import defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Tuple

try:
    import matplotlib.pyplot as plt
    HAS_MATPLOTLIB = True
except ImportError:  # pragma: no cover
    plt = None  # type: ignore[assignment]
    HAS_MATPLOTLIB = False

RANK_RE = re.compile(r"(?:^|/)ranks-(\d+)(?:/|$)")


def _to_float(value: str, default: float = 0.0) -> float:
    if value is None or value == "":
        return default
    try:
        return float(value)
    except ValueError:
        return default


def _to_int(value: str, default: int = 0) -> int:
    if value is None or value == "":
        return default
    try:
        return int(float(value))
    except ValueError:
        return default


def _mean(values: List[float]) -> float:
    if not values:
        return 0.0
    return sum(values) / len(values)


def _stddev(values: List[float]) -> float:
    if len(values) <= 1:
        return 0.0
    mu = _mean(values)
    return math.sqrt(sum((v - mu) ** 2 for v in values) / (len(values) - 1))


def _ci95(values: List[float]) -> float:
    if len(values) <= 1:
        return 0.0
    return 1.96 * _stddev(values) / math.sqrt(len(values))


def _find_summary_csvs(root: Path) -> List[Path]:
    return sorted(root.rglob("summary.csv"))


def _infer_ranks(csv_path: str) -> int:
    m = RANK_RE.search(csv_path)
    if not m:
        return 0
    return int(m.group(1))


def load_rows(root: Path, scenario_kind_filter: str) -> List[Dict[str, object]]:
    out: List[Dict[str, object]] = []
    for summary_csv in _find_summary_csvs(root):
        with summary_csv.open("r", encoding="utf-8", newline="") as f:
            reader = csv.DictReader(f)
            header = set(reader.fieldnames or [])
            for row in reader:
                scenario_kind = row.get("scenario_kind", "")
                if not scenario_kind:
                    # legacy schema (tcpcc era)
                    scenario_kind = "legacy"
                if scenario_kind_filter != "any" and scenario_kind != scenario_kind_filter:
                    continue
                csv_path = row.get("csv_path", "")
                ranks = _infer_ranks(csv_path)
                out.append(
                    {
                        "scenario_kind": scenario_kind,
                        "scenario": row.get("scenario", "unknown"),
                        "mode": row.get("mode", "unknown"),
                        "trial": _to_int(row.get("trial", "0"), 0),
                        "ranks": ranks,
                        "t_iter_us": _to_float(row.get("t_iter_us", "0"), 0.0),
                        "t_wait_us": _to_float(row.get("t_wait_us", "0"), 0.0),
                        "t_interior_us": _to_float(row.get("t_interior_us", "0"), 0.0),
                        "t_boundary_us": _to_float(row.get("t_boundary_us", "0"), 0.0),
                        "wait_frac": _to_float(row.get("wait_frac", "0"), 0.0),
                        "overlap_ratio": _to_float(row.get("overlap_ratio", "0"), 0.0),
                        "csv_path": csv_path,
                        "summary_csv": str(summary_csv),
                    }
                )
    return [r for r in out if int(r["ranks"]) > 0]


def aggregate(rows: List[Dict[str, object]]) -> List[Dict[str, object]]:
    grouped: Dict[Tuple[str, str, int], List[Dict[str, object]]] = defaultdict(list)
    for r in rows:
        key = (str(r["scenario"]), str(r["mode"]), int(r["ranks"]))
        grouped[key].append(r)

    out: List[Dict[str, object]] = []
    for (scenario, mode, ranks), bucket in grouped.items():
        def vals(k: str) -> List[float]:
            return [float(x[k]) for x in bucket]

        out.append(
            {
                "scenario": scenario,
                "mode": mode,
                "ranks": ranks,
                "samples": len(bucket),
                "t_iter_us_mean": _mean(vals("t_iter_us")),
                "t_iter_us_ci95": _ci95(vals("t_iter_us")),
                "t_wait_us_mean": _mean(vals("t_wait_us")),
                "t_wait_us_ci95": _ci95(vals("t_wait_us")),
                "wait_frac_mean": _mean(vals("wait_frac")),
                "wait_frac_ci95": _ci95(vals("wait_frac")),
                "overlap_ratio_mean": _mean(vals("overlap_ratio")),
                "overlap_ratio_ci95": _ci95(vals("overlap_ratio")),
            }
        )
    out.sort(key=lambda x: (str(x["scenario"]), str(x["mode"]), int(x["ranks"])))
    return out


def _plot_metric(agg: List[Dict[str, object]], metric_mean: str, metric_ci: str, ylabel: str, title: str, out_path: Path) -> bool:
    if not HAS_MATPLOTLIB:
        return False

    scenarios = sorted({str(r["scenario"]) for r in agg})
    modes = sorted({str(r["mode"]) for r in agg})
    if not scenarios or not modes:
        return False

    ncols = min(3, len(scenarios))
    nrows = (len(scenarios) + ncols - 1) // ncols
    fig, axes = plt.subplots(nrows=nrows, ncols=ncols, figsize=(6 * ncols, 4 * nrows), squeeze=False)

    for idx, scenario in enumerate(scenarios):
        ax = axes[idx // ncols][idx % ncols]
        for mode in modes:
            rows = [r for r in agg if str(r["scenario"]) == scenario and str(r["mode"]) == mode]
            rows.sort(key=lambda x: int(x["ranks"]))
            if not rows:
                continue
            xs = [int(r["ranks"]) for r in rows]
            ys = [float(r[metric_mean]) for r in rows]
            err = [float(r[metric_ci]) for r in rows]
            ax.errorbar(xs, ys, yerr=err, marker="o", capsize=3, linewidth=1.5, label=mode)

        ax.set_title(scenario)
        ax.set_xlabel("ranks")
        ax.set_ylabel(ylabel)
        ax.grid(True, alpha=0.25)
        ax.legend(fontsize=8)

    for idx in range(len(scenarios), nrows * ncols):
        axes[idx // ncols][idx % ncols].axis("off")

    fig.suptitle(title)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(out_path, dpi=160)
    plt.close(fig)
    return True


def _plot_metric_svg(
    agg: List[Dict[str, object]],
    metric_mean: str,
    metric_ci: str,
    ylabel: str,
    title: str,
    out_path: Path,
) -> bool:
    scenarios = sorted({str(r["scenario"]) for r in agg})
    modes = sorted({str(r["mode"]) for r in agg})
    if not scenarios or not modes:
        return False

    panel_w = 420
    panel_h = 260
    margin_l = 56
    margin_r = 16
    margin_t = 28
    margin_b = 36
    gap = 20
    cols = min(3, len(scenarios))
    rows = (len(scenarios) + cols - 1) // cols
    width = cols * panel_w + (cols + 1) * gap
    height = rows * panel_h + (rows + 1) * gap + 36

    ranks_all = sorted({int(r["ranks"]) for r in agg})
    if not ranks_all:
        return False
    x_min, x_max = min(ranks_all), max(ranks_all)
    if x_min == x_max:
        x_min -= 1
        x_max += 1

    vals = []
    for r in agg:
        m = float(r[metric_mean])
        c = float(r[metric_ci])
        vals.extend([m - c, m + c, m])
    y_min = min(vals) if vals else 0.0
    y_max = max(vals) if vals else 1.0
    if y_max <= y_min:
        y_max = y_min + 1.0
    pad = 0.08 * (y_max - y_min)
    y_min -= pad
    y_max += pad

    colors = [
        "#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd",
        "#8c564b", "#e377c2", "#7f7f7f", "#bcbd22", "#17becf",
    ]
    mode_color = {m: colors[i % len(colors)] for i, m in enumerate(modes)}

    def sx(rank: int, left: int) -> float:
        return left + margin_l + (rank - x_min) * (panel_w - margin_l - margin_r) / (x_max - x_min)

    def sy(value: float, top: int) -> float:
        return top + margin_t + (y_max - value) * (panel_h - margin_t - margin_b) / (y_max - y_min)

    svg = [
        f"<svg xmlns='http://www.w3.org/2000/svg' width='{width}' height='{height}' viewBox='0 0 {width} {height}'>",
        "<rect width='100%' height='100%' fill='white'/>",
        f"<text x='{width/2:.1f}' y='22' text-anchor='middle' font-family='sans-serif' font-size='16' font-weight='600'>{html.escape(title)}</text>",
    ]

    for idx, scenario in enumerate(scenarios):
        r = idx // cols
        c = idx % cols
        left = gap + c * (panel_w + gap)
        top = 36 + gap + r * (panel_h + gap)
        plot_left = left + margin_l
        plot_right = left + panel_w - margin_r
        plot_top = top + margin_t
        plot_bottom = top + panel_h - margin_b

        svg.append(f"<rect x='{left}' y='{top}' width='{panel_w}' height='{panel_h}' fill='#fcfcfc' stroke='#dcdcdc'/>")
        svg.append(f"<text x='{left + panel_w/2:.1f}' y='{top + 18}' text-anchor='middle' font-family='sans-serif' font-size='13'>{html.escape(scenario)}</text>")
        svg.append(f"<line x1='{plot_left}' y1='{plot_bottom}' x2='{plot_right}' y2='{plot_bottom}' stroke='#666'/>")
        svg.append(f"<line x1='{plot_left}' y1='{plot_top}' x2='{plot_left}' y2='{plot_bottom}' stroke='#666'/>")

        for rk in ranks_all:
            x = sx(rk, left)
            svg.append(f"<line x1='{x:.1f}' y1='{plot_top}' x2='{x:.1f}' y2='{plot_bottom}' stroke='#eee'/>")
            svg.append(f"<text x='{x:.1f}' y='{plot_bottom + 16:.1f}' text-anchor='middle' font-family='sans-serif' font-size='10'>{rk}</text>")

        for y_tick in range(5):
            frac = y_tick / 4.0
            val = y_min + frac * (y_max - y_min)
            y = sy(val, top)
            svg.append(f"<line x1='{plot_left}' y1='{y:.1f}' x2='{plot_right}' y2='{y:.1f}' stroke='#eee'/>")
            svg.append(f"<text x='{plot_left - 6:.1f}' y='{y + 4:.1f}' text-anchor='end' font-family='sans-serif' font-size='10'>{val:.3g}</text>")

        for mode in modes:
            rows_mode = [x for x in agg if str(x["scenario"]) == scenario and str(x["mode"]) == mode]
            rows_mode.sort(key=lambda x: int(x["ranks"]))
            if not rows_mode:
                continue
            points = []
            for rr in rows_mode:
                rk = int(rr["ranks"])
                mean = float(rr[metric_mean])
                ci = float(rr[metric_ci])
                x = sx(rk, left)
                y = sy(mean, top)
                y0 = sy(mean - ci, top)
                y1 = sy(mean + ci, top)
                svg.append(f"<line x1='{x:.1f}' y1='{y0:.1f}' x2='{x:.1f}' y2='{y1:.1f}' stroke='{mode_color[mode]}' stroke-width='1.2'/>")
                svg.append(f"<circle cx='{x:.1f}' cy='{y:.1f}' r='2.8' fill='{mode_color[mode]}'/>")
                points.append(f"{x:.1f},{y:.1f}")
            if points:
                svg.append(f"<polyline fill='none' stroke='{mode_color[mode]}' stroke-width='1.5' points='{' '.join(points)}'/>")

        lx = left + 8
        ly = top + panel_h - 8
        for i, mode in enumerate(modes[:5]):
            y = ly - i * 12
            svg.append(f"<line x1='{lx}' y1='{y}' x2='{lx + 14}' y2='{y}' stroke='{mode_color[mode]}' stroke-width='2'/>")
            svg.append(f"<text x='{lx + 18}' y='{y + 3}' font-family='sans-serif' font-size='10'>{html.escape(mode)}</text>")

        svg.append(f"<text x='{left + panel_w/2:.1f}' y='{top + panel_h - 4}' text-anchor='middle' font-family='sans-serif' font-size='10'>ranks</text>")
        svg.append(
            f"<text x='{left + 12}' y='{top + panel_h/2:.1f}' text-anchor='middle' font-family='sans-serif' font-size='10' "
            f"transform='rotate(-90 {left + 12} {top + panel_h/2:.1f})'>{html.escape(ylabel)}</text>"
        )

    svg.append("</svg>")
    out_path.write_text("\n".join(svg), encoding="utf-8")
    return True


def write_csv(path: Path, rows: List[Dict[str, object]], headers: Iterable[str]) -> None:
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(headers))
        w.writeheader()
        for r in rows:
            w.writerow(r)


def write_html(out_dir: Path, raw_rows: List[Dict[str, object]], agg_rows: List[Dict[str, object]], scenario_kind: str) -> None:
    plot_names = [
        "t_iter_vs_ranks.png",
        "t_wait_vs_ranks.png",
        "wait_frac_vs_ranks.png",
        "overlap_ratio_vs_ranks.png",
        "t_iter_vs_ranks.svg",
        "t_wait_vs_ranks.svg",
        "wait_frac_vs_ranks.svg",
        "overlap_ratio_vs_ranks.svg",
    ]
    existing = [p for p in plot_names if (out_dir / p).exists()]

    scenarios = sorted({str(r["scenario"]) for r in raw_rows})
    modes = sorted({str(r["mode"]) for r in raw_rows})
    ranks = sorted({int(r["ranks"]) for r in raw_rows})

    cards = [
        ("Scenario Kind", scenario_kind),
        ("Raw rows", str(len(raw_rows))),
        ("Aggregated rows", str(len(agg_rows))),
        ("Scenarios", ", ".join(scenarios) or "n/a"),
        ("Modes", ", ".join(modes) or "n/a"),
        ("Ranks", ", ".join(str(r) for r in ranks) or "n/a"),
    ]
    card_html = "".join(
        f"<div class='card'><div class='k'>{html.escape(k)}</div><div class='v'>{html.escape(v)}</div></div>"
        for k, v in cards
    )

    if existing:
        plot_html = "".join(
            f"<section class='plot'><h3>{html.escape(name)}</h3><img src='{html.escape(name)}' alt='{html.escape(name)}'></section>"
            for name in existing
        )
    else:
        plot_html = "<p>No plots generated. Install matplotlib and rerun.</p>"

    table_rows = agg_rows[:50]
    headers = [
        "scenario", "mode", "ranks", "samples",
        "t_iter_us_mean", "t_iter_us_ci95", "t_wait_us_mean", "t_wait_us_ci95",
        "wait_frac_mean", "wait_frac_ci95", "overlap_ratio_mean", "overlap_ratio_ci95",
    ]
    if table_rows:
        head_html = "".join(f"<th>{h}</th>" for h in headers)
        body_html = ""
        for r in table_rows:
            body_html += "<tr>" + "".join(f"<td>{html.escape(str(r.get(h, '')))}</td>" for h in headers) + "</tr>"
        table_html = f"<table><thead><tr>{head_html}</tr></thead><tbody>{body_html}</tbody></table>"
    else:
        table_html = "<p>No aggregate rows found.</p>"

    page = f"""<!doctype html>
<html lang='en'>
<head>
  <meta charset='utf-8' />
  <meta name='viewport' content='width=device-width, initial-scale=1' />
  <title>PhaseGap Multihost Dashboard</title>
  <style>
    body {{ font-family: -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif; margin:24px; color:#1a1a1a; }}
    .cards {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(220px,1fr)); gap:12px; margin:16px 0 24px; }}
    .card {{ border:1px solid #ddd; border-radius:10px; padding:12px; background:#fafafa; }}
    .card .k {{ font-size:12px; color:#555; }}
    .card .v {{ font-size:16px; font-weight:600; margin-top:4px; }}
    .plot {{ margin: 20px 0; }}
    .plot img {{ max-width:100%; border:1px solid #ddd; border-radius:8px; }}
    table {{ border-collapse: collapse; width:100%; font-size: 14px; }}
    th, td {{ border:1px solid #ddd; padding:6px 8px; text-align:left; }}
    th {{ background:#f2f2f2; }}
  </style>
</head>
<body>
  <h1>PhaseGap Multihost Dashboard</h1>
  <p>Generated by scripts/analyze_multihost.py</p>
  <div class='cards'>{card_html}</div>
  <h2>Plots</h2>
  {plot_html}
  <h2>Aggregate Summary (first 50 rows)</h2>
  {table_html}
</body>
</html>
"""

    (out_dir / "dashboard.html").write_text(page, encoding="utf-8")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generate multihost rank-sweep dashboard")
    p.add_argument("--input-root", default="runs/multihost-rank-sweep", help="Root containing ranks-*/.../summary.csv")
    p.add_argument("--out-dir", default="runs/multihost-rank-sweep/analysis", help="Output directory")
    p.add_argument("--scenario-kind", default="netem", help="Filter scenario_kind: netem|tcpcc|legacy|any")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    input_root = Path(args.input_root).resolve()
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(input_root, args.scenario_kind)
    if not rows:
        print(f"[analyze_multihost] fail: no rows for scenario_kind={args.scenario_kind} under {input_root}")
        return 1

    agg = aggregate(rows)
    write_csv(
        out_dir / "aggregate.csv",
        agg,
        headers=[
            "scenario", "mode", "ranks", "samples",
            "t_iter_us_mean", "t_iter_us_ci95",
            "t_wait_us_mean", "t_wait_us_ci95",
            "wait_frac_mean", "wait_frac_ci95",
            "overlap_ratio_mean", "overlap_ratio_ci95",
        ],
    )

    made_iter = _plot_metric(
        agg,
        metric_mean="t_iter_us_mean",
        metric_ci="t_iter_us_ci95",
        ylabel="t_iter_us",
        title="t_iter vs ranks (mean ±95% CI)",
        out_path=out_dir / "t_iter_vs_ranks.png",
    )
    if not made_iter:
        _plot_metric_svg(
            agg, "t_iter_us_mean", "t_iter_us_ci95", "t_iter_us", "t_iter vs ranks (mean ±95% CI)",
            out_dir / "t_iter_vs_ranks.svg"
        )

    made_wait = _plot_metric(
        agg,
        metric_mean="t_wait_us_mean",
        metric_ci="t_wait_us_ci95",
        ylabel="t_wait_us",
        title="t_wait vs ranks (mean ±95% CI)",
        out_path=out_dir / "t_wait_vs_ranks.png",
    )
    if not made_wait:
        _plot_metric_svg(
            agg, "t_wait_us_mean", "t_wait_us_ci95", "t_wait_us", "t_wait vs ranks (mean ±95% CI)",
            out_dir / "t_wait_vs_ranks.svg"
        )

    made_wait_frac = _plot_metric(
        agg,
        metric_mean="wait_frac_mean",
        metric_ci="wait_frac_ci95",
        ylabel="wait_frac",
        title="wait_frac vs ranks (mean ±95% CI)",
        out_path=out_dir / "wait_frac_vs_ranks.png",
    )
    if not made_wait_frac:
        _plot_metric_svg(
            agg, "wait_frac_mean", "wait_frac_ci95", "wait_frac", "wait_frac vs ranks (mean ±95% CI)",
            out_path=out_dir / "wait_frac_vs_ranks.svg"
        )

    made_overlap = _plot_metric(
        agg,
        metric_mean="overlap_ratio_mean",
        metric_ci="overlap_ratio_ci95",
        ylabel="overlap_ratio",
        title="overlap_ratio vs ranks (mean ±95% CI)",
        out_path=out_dir / "overlap_ratio_vs_ranks.png",
    )
    if not made_overlap:
        _plot_metric_svg(
            agg, "overlap_ratio_mean", "overlap_ratio_ci95", "overlap_ratio", "overlap_ratio vs ranks (mean ±95% CI)",
            out_path=out_dir / "overlap_ratio_vs_ranks.svg"
        )

    write_html(out_dir, rows, agg, args.scenario_kind)

    print(f"[analyze_multihost] rows={len(rows)}")
    print(f"[analyze_multihost] wrote {out_dir / 'aggregate.csv'}")
    print(f"[analyze_multihost] wrote {out_dir / 'dashboard.html'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
