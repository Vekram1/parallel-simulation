#!/usr/bin/env python3
"""Build per-netem-scenario multihost rank dashboards with 5 plots each."""

from __future__ import annotations

import argparse
import csv
import html
import math
import re
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Tuple

RANK_RE = re.compile(r"(?:^|/)ranks-(\d+)(?:/|$)")

METRICS = [
    ("t_iter_us", "t_iter_us", "t_iter_vs_ranks.svg"),
    ("t_wait_us", "t_wait_us", "t_wait_vs_ranks.svg"),
    ("wait_frac", "wait_frac", "wait_frac_vs_ranks.svg"),
    ("overlap_ratio", "overlap_ratio", "overlap_ratio_vs_ranks.svg"),
]

COMPONENTS = ["t_post_us", "t_interior_us", "t_wait_us", "t_boundary_us"]
COMP_COLORS = {
    "t_post_us": "#4c78a8",
    "t_interior_us": "#f58518",
    "t_wait_us": "#e45756",
    "t_boundary_us": "#72b7b2",
    "residual_us": "#b279a2",
}
MODE_COLORS = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd", "#8c564b"]


def _to_float(s: str, default: float = 0.0) -> float:
    if s is None or s == "":
        return default
    try:
        return float(s)
    except ValueError:
        return default


def _mean(vals: List[float]) -> float:
    return sum(vals) / len(vals) if vals else 0.0


def _std(vals: List[float]) -> float:
    if len(vals) <= 1:
        return 0.0
    mu = _mean(vals)
    return math.sqrt(sum((v - mu) ** 2 for v in vals) / (len(vals) - 1))


def _ci95(vals: List[float]) -> float:
    if len(vals) <= 1:
        return 0.0
    return 1.96 * _std(vals) / math.sqrt(len(vals))


def _infer_ranks(csv_path: str) -> int:
    m = RANK_RE.search(csv_path)
    return int(m.group(1)) if m else 0


def _read_results_row(path: Path) -> Dict[str, str]:
    if not path.exists():
        return {}
    with path.open("r", encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    return rows[-1] if rows else {}


def load_rows(input_root: Path, scenario_kind: str) -> List[Dict[str, object]]:
    rows: List[Dict[str, object]] = []
    for summary_path in sorted(input_root.rglob("summary.csv")):
        with summary_path.open("r", encoding="utf-8", newline="") as f:
            for row in csv.DictReader(f):
                sk = row.get("scenario_kind", "legacy")
                if scenario_kind != "any" and sk != scenario_kind:
                    continue
                csv_path = Path(row.get("csv_path", ""))
                ranks = _infer_ranks(str(csv_path))
                if ranks <= 0:
                    continue
                result_row = _read_results_row(csv_path)
                t_iter = _to_float(row.get("t_iter_us", "0"))
                t_wait = _to_float(row.get("t_wait_us", "0"))
                t_interior = _to_float(row.get("t_interior_us", "0"))
                t_boundary = _to_float(row.get("t_boundary_us", "0"))
                t_post = _to_float(result_row.get("t_post_us", "0"))
                rows.append(
                    {
                        "scenario_kind": sk,
                        "scenario": row.get("scenario", "unknown"),
                        "mode": row.get("mode", "unknown"),
                        "trial": int(float(row.get("trial", "0") or 0)),
                        "ranks": ranks,
                        "t_iter_us": t_iter,
                        "t_wait_us": t_wait,
                        "t_interior_us": t_interior,
                        "t_boundary_us": t_boundary,
                        "t_post_us": t_post,
                        "wait_frac": _to_float(row.get("wait_frac", "0")),
                        "overlap_ratio": _to_float(row.get("overlap_ratio", "0")),
                    }
                )
    return rows


def aggregate(rows: List[Dict[str, object]]) -> List[Dict[str, object]]:
    grouped: Dict[Tuple[str, str, int], List[Dict[str, object]]] = defaultdict(list)
    for r in rows:
        key = (str(r["scenario"]), str(r["mode"]), int(r["ranks"]))
        grouped[key].append(r)

    out: List[Dict[str, object]] = []
    for (scenario, mode, ranks), bucket in grouped.items():
        def vals(k: str) -> List[float]:
            return [float(x[k]) for x in bucket]

        entry: Dict[str, object] = {
            "scenario": scenario,
            "mode": mode,
            "ranks": ranks,
            "samples": len(bucket),
        }
        for k in ["t_iter_us", "t_wait_us", "wait_frac", "overlap_ratio", "t_post_us", "t_interior_us", "t_boundary_us"]:
            v = vals(k)
            entry[f"{k}_mean"] = _mean(v)
            entry[f"{k}_ci95"] = _ci95(v)
        # residual from means
        entry["residual_us_mean"] = float(entry["t_iter_us_mean"]) - (
            float(entry["t_post_us_mean"]) + float(entry["t_interior_us_mean"]) + float(entry["t_wait_us_mean"]) + float(entry["t_boundary_us_mean"])
        )
        out.append(entry)

    out.sort(key=lambda x: (str(x["scenario"]), str(x["mode"]), int(x["ranks"])))
    return out


def _line_plot_svg(rows: List[Dict[str, object]], metric: str, ylabel: str, title: str, out_path: Path) -> None:
    modes = sorted({str(r["mode"]) for r in rows})
    ranks = sorted({int(r["ranks"]) for r in rows})
    width, height = 1000, 420
    ml, mr, mt, mb = 70, 20, 50, 50
    pw, ph = width - ml - mr, height - mt - mb

    values = []
    for r in rows:
        m = float(r[f"{metric}_mean"])
        c = float(r[f"{metric}_ci95"])
        values.extend([m - c, m, m + c])
    ymin, ymax = min(values), max(values)
    if ymax <= ymin:
        ymax = ymin + 1.0
    pad = 0.1 * (ymax - ymin)
    ymin -= pad
    ymax += pad

    xmin, xmax = min(ranks), max(ranks)
    if xmin == xmax:
        xmin -= 1
        xmax += 1

    def sx(x: int) -> float:
        return ml + (x - xmin) * pw / (xmax - xmin)

    def sy(y: float) -> float:
        return mt + (ymax - y) * ph / (ymax - ymin)

    mode_color = {m: MODE_COLORS[i % len(MODE_COLORS)] for i, m in enumerate(modes)}

    svg = [
        f"<svg xmlns='http://www.w3.org/2000/svg' width='{width}' height='{height}' viewBox='0 0 {width} {height}'>",
        "<rect width='100%' height='100%' fill='white'/>",
        f"<text x='{width/2:.1f}' y='26' text-anchor='middle' font-family='sans-serif' font-size='18' font-weight='600'>{html.escape(title)}</text>",
        f"<line x1='{ml}' y1='{mt+ph}' x2='{ml+pw}' y2='{mt+ph}' stroke='#444'/>",
        f"<line x1='{ml}' y1='{mt}' x2='{ml}' y2='{mt+ph}' stroke='#444'/>",
    ]

    for x in ranks:
        px = sx(x)
        svg.append(f"<line x1='{px:.1f}' y1='{mt}' x2='{px:.1f}' y2='{mt+ph}' stroke='#eee'/>")
        svg.append(f"<text x='{px:.1f}' y='{mt+ph+18:.1f}' text-anchor='middle' font-family='sans-serif' font-size='11'>{x}</text>")

    for i in range(6):
        v = ymin + i * (ymax - ymin) / 5.0
        py = sy(v)
        svg.append(f"<line x1='{ml}' y1='{py:.1f}' x2='{ml+pw}' y2='{py:.1f}' stroke='#f0f0f0'/>")
        svg.append(f"<text x='{ml-6}' y='{py+4:.1f}' text-anchor='end' font-family='sans-serif' font-size='11'>{v:.3g}</text>")

    for mode in modes:
        mrows = [r for r in rows if str(r["mode"]) == mode]
        mrows.sort(key=lambda r: int(r["ranks"]))
        pts = []
        for r in mrows:
            x = sx(int(r["ranks"]))
            y = sy(float(r[f"{metric}_mean"]))
            c = float(r[f"{metric}_ci95"])
            y0 = sy(float(r[f"{metric}_mean"]) - c)
            y1 = sy(float(r[f"{metric}_mean"]) + c)
            svg.append(f"<line x1='{x:.1f}' y1='{y0:.1f}' x2='{x:.1f}' y2='{y1:.1f}' stroke='{mode_color[mode]}' stroke-width='1.4'/>")
            svg.append(f"<circle cx='{x:.1f}' cy='{y:.1f}' r='3' fill='{mode_color[mode]}'/>")
            pts.append(f"{x:.1f},{y:.1f}")
        if pts:
            svg.append(f"<polyline fill='none' stroke='{mode_color[mode]}' stroke-width='1.7' points='{' '.join(pts)}'/>")

    lx, ly = ml + 8, mt + 12
    for i, mode in enumerate(modes):
        y = ly + i * 16
        svg.append(f"<line x1='{lx}' y1='{y}' x2='{lx+16}' y2='{y}' stroke='{mode_color[mode]}' stroke-width='2'/>")
        svg.append(f"<text x='{lx+20}' y='{y+4}' font-family='sans-serif' font-size='11'>{html.escape(mode)}</text>")

    svg.append(f"<text x='{ml+pw/2:.1f}' y='{height-12}' text-anchor='middle' font-family='sans-serif' font-size='12'>ranks</text>")
    svg.append(f"<text x='18' y='{mt+ph/2:.1f}' text-anchor='middle' font-family='sans-serif' font-size='12' transform='rotate(-90 18 {mt+ph/2:.1f})'>{html.escape(ylabel)}</text>")
    svg.append("</svg>")
    out_path.write_text("\n".join(svg), encoding="utf-8")


def _stacked_plot_svg(rows: List[Dict[str, object]], title: str, out_path: Path) -> None:
    modes = sorted({str(r["mode"]) for r in rows})
    ranks = sorted({int(r["ranks"]) for r in rows})

    panel_w = 300
    panel_h = 360
    gap = 20
    width = len(modes) * panel_w + (len(modes) + 1) * gap
    height = panel_h + 120
    ml, mr, mt, mb = 50, 14, 40, 40
    ph = panel_h - mt - mb

    ymax = 0.0
    for r in rows:
        stack = float(r["t_post_us_mean"]) + float(r["t_interior_us_mean"]) + float(r["t_wait_us_mean"]) + float(r["t_boundary_us_mean"]) + max(0.0, float(r["residual_us_mean"]))
        ymax = max(ymax, stack, float(r["t_iter_us_mean"]) + float(r["t_iter_us_ci95"]))
    if ymax <= 0:
        ymax = 1.0
    ymax *= 1.15

    def sy(v: float, top: float) -> float:
        return top + mt + (ymax - v) * ph / ymax

    svg = [
        f"<svg xmlns='http://www.w3.org/2000/svg' width='{width}' height='{height}' viewBox='0 0 {width} {height}'>",
        "<rect width='100%' height='100%' fill='white'/>",
        f"<text x='{width/2:.1f}' y='24' text-anchor='middle' font-family='sans-serif' font-size='18' font-weight='600'>{html.escape(title)}</text>",
    ]

    for mi, mode in enumerate(modes):
        left = gap + mi * (panel_w + gap)
        top = 40
        pw = panel_w - ml - mr
        svg.append(f"<rect x='{left}' y='{top}' width='{panel_w}' height='{panel_h}' fill='#fcfcfc' stroke='#ddd'/>")
        svg.append(f"<text x='{left+panel_w/2:.1f}' y='{top+18}' text-anchor='middle' font-family='sans-serif' font-size='13'>{html.escape(mode)}</text>")
        svg.append(f"<line x1='{left+ml}' y1='{top+mt+ph}' x2='{left+ml+pw}' y2='{top+mt+ph}' stroke='#444'/>")
        svg.append(f"<line x1='{left+ml}' y1='{top+mt}' x2='{left+ml}' y2='{top+mt+ph}' stroke='#444'/>")

        for yi in range(5):
            vv = yi * ymax / 4.0
            yy = sy(vv, top)
            svg.append(f"<line x1='{left+ml}' y1='{yy:.1f}' x2='{left+ml+pw}' y2='{yy:.1f}' stroke='#f0f0f0'/>")

        mrows = [r for r in rows if str(r["mode"]) == mode]
        mrows.sort(key=lambda r: int(r["ranks"]))
        if not mrows:
            continue
        bw = max(10.0, pw / (len(ranks) * 1.7))
        step = pw / max(1, len(ranks))

        for i, rank in enumerate(ranks):
            xmid = left + ml + step * (i + 0.5)
            x0 = xmid - bw / 2.0
            svg.append(f"<text x='{xmid:.1f}' y='{top+mt+ph+16:.1f}' text-anchor='middle' font-family='sans-serif' font-size='10'>{rank}</text>")

            rrow = next((rr for rr in mrows if int(rr["ranks"]) == rank), None)
            if rrow is None:
                continue

            y_base = 0.0
            for comp in COMPONENTS + ["residual_us"]:
                val = float(rrow.get(f"{comp}_mean", 0.0)) if comp != "residual_us" else max(0.0, float(rrow.get("residual_us_mean", 0.0)))
                if val <= 0:
                    continue
                y1 = sy(y_base, top)
                y2 = sy(y_base + val, top)
                svg.append(
                    f"<rect x='{x0:.1f}' y='{y2:.1f}' width='{bw:.1f}' height='{(y1-y2):.1f}' fill='{COMP_COLORS[comp]}' stroke='none'/>"
                )
                y_base += val

            t_mean = float(rrow["t_iter_us_mean"])
            t_ci = float(rrow["t_iter_us_ci95"])
            y_mean = sy(t_mean, top)
            y_lo = sy(max(0.0, t_mean - t_ci), top)
            y_hi = sy(t_mean + t_ci, top)
            svg.append(f"<line x1='{xmid:.1f}' y1='{y_lo:.1f}' x2='{xmid:.1f}' y2='{y_hi:.1f}' stroke='black' stroke-width='1.4'/>")
            svg.append(f"<circle cx='{xmid:.1f}' cy='{y_mean:.1f}' r='2.6' fill='black'/>")

    # legend
    lx = 10
    ly = height - 62
    legend_items = [
        ("t_post_us", "t_post"),
        ("t_interior_us", "t_interior"),
        ("t_wait_us", "t_wait"),
        ("t_boundary_us", "t_boundary"),
        ("residual_us", "residual"),
    ]
    for i, (k, label) in enumerate(legend_items):
        x = lx + i * 120
        svg.append(f"<rect x='{x}' y='{ly}' width='14' height='10' fill='{COMP_COLORS[k]}'/>")
        svg.append(f"<text x='{x+20}' y='{ly+9}' font-family='sans-serif' font-size='11'>{label}</text>")
    svg.append(f"<circle cx='{lx+620}' cy='{ly+6}' r='3' fill='black'/><text x='{lx+632}' y='{ly+9}' font-family='sans-serif' font-size='11'>t_iter mean +/- 95% CI</text>")
    svg.append("</svg>")
    out_path.write_text("\n".join(svg), encoding="utf-8")


def write_dashboard(out_dir: Path, scenario_dirs: List[Tuple[str, Path]], raw_count: int, agg_count: int, ranks: List[int]) -> None:
    cards = [
        ("Raw rows", str(raw_count)),
        ("Aggregated rows", str(agg_count)),
        ("Scenarios", ", ".join(s for s, _ in scenario_dirs) or "n/a"),
        ("Ranks", ", ".join(str(r) for r in ranks) or "n/a"),
    ]
    card_html = "".join(
        f"<div class='card'><div class='k'>{html.escape(k)}</div><div class='v'>{html.escape(v)}</div></div>" for k, v in cards
    )

    sections = []
    for scenario, sdir in scenario_dirs:
        imgs = [
            "t_iter_vs_ranks.svg",
            "t_wait_vs_ranks.svg",
            "wait_frac_vs_ranks.svg",
            "overlap_ratio_vs_ranks.svg",
            "stacked_components_vs_titer.svg",
        ]
        blocks = []
        for img in imgs:
            rel = f"{sdir.name}/{img}"
            blocks.append(f"<section class='plot'><h3>{html.escape(img)}</h3><img src='{html.escape(rel)}' alt='{html.escape(rel)}'></section>")
        sections.append(f"<h2>{html.escape(scenario)}</h2>{''.join(blocks)}")

    page = f"""<!doctype html>
<html lang='en'>
<head>
  <meta charset='utf-8'/>
  <meta name='viewport' content='width=device-width, initial-scale=1'/>
  <title>PhaseGap Netem Dashboard</title>
  <style>
    body {{ font-family: -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif; margin:24px; color:#1a1a1a; }}
    .cards {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(220px,1fr)); gap:12px; margin:16px 0 24px; }}
    .card {{ border:1px solid #ddd; border-radius:10px; padding:12px; background:#fafafa; }}
    .card .k {{ font-size:12px; color:#555; }}
    .card .v {{ font-size:16px; font-weight:600; margin-top:4px; }}
    .plot {{ margin: 18px 0 28px; }}
    .plot img {{ max-width: 100%; border: 1px solid #ddd; border-radius: 8px; }}
  </style>
</head>
<body>
  <h1>PhaseGap Netem Rank-Sweep Dashboard</h1>
  <p>Generated by scripts/analyze_multihost_netem.py</p>
  <div class='cards'>{card_html}</div>
  {''.join(sections)}
</body>
</html>
"""
    (out_dir / "dashboard.html").write_text(page, encoding="utf-8")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Analyze multihost netem rank sweep")
    p.add_argument("--input-root", default="runs/multihost-rank-sweep")
    p.add_argument("--out-dir", default="runs/multihost-rank-sweep/analysis-netem")
    p.add_argument("--scenario-kind", default="netem", help="netem|tcpcc|legacy|any")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    input_root = Path(args.input_root).resolve()
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(input_root, args.scenario_kind)
    if not rows:
        print(f"[analyze_multihost_netem] fail: no rows for scenario_kind={args.scenario_kind} under {input_root}")
        return 1

    agg = aggregate(rows)
    ranks = sorted({int(r["ranks"]) for r in rows})
    scenarios = sorted({str(r["scenario"]) for r in rows})

    agg_csv = out_dir / "aggregate.csv"
    with agg_csv.open("w", encoding="utf-8", newline="") as f:
        fields = [
            "scenario", "mode", "ranks", "samples",
            "t_iter_us_mean", "t_iter_us_ci95", "t_wait_us_mean", "t_wait_us_ci95",
            "wait_frac_mean", "wait_frac_ci95", "overlap_ratio_mean", "overlap_ratio_ci95",
            "t_post_us_mean", "t_interior_us_mean", "t_boundary_us_mean", "residual_us_mean",
        ]
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in agg:
            w.writerow({k: r.get(k, "") for k in fields})

    scenario_dirs: List[Tuple[str, Path]] = []
    for scenario in scenarios:
        sdir = out_dir / scenario
        sdir.mkdir(parents=True, exist_ok=True)
        srows = [r for r in agg if str(r["scenario"]) == scenario]

        for metric, ylabel, fname in METRICS:
            _line_plot_svg(
                srows,
                metric=metric,
                ylabel=ylabel,
                title=f"{scenario}: {metric} vs ranks (mean +/-95% CI)",
                out_path=sdir / fname,
            )

        _stacked_plot_svg(
            srows,
            title=f"{scenario}: stacked phase means + residual + t_iter CI",
            out_path=sdir / "stacked_components_vs_titer.svg",
        )
        scenario_dirs.append((scenario, sdir))

    write_dashboard(out_dir, scenario_dirs, raw_count=len(rows), agg_count=len(agg), ranks=ranks)

    print(f"[analyze_multihost_netem] rows={len(rows)}")
    print(f"[analyze_multihost_netem] scenarios={','.join(scenarios)}")
    print(f"[analyze_multihost_netem] ranks={','.join(str(r) for r in ranks)}")
    print(f"[analyze_multihost_netem] wrote {agg_csv}")
    print(f"[analyze_multihost_netem] wrote {out_dir / 'dashboard.html'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
