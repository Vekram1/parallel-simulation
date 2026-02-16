#!/usr/bin/env python3
"""PhaseGap analysis utility.

Generates a default report pack:
- wait_frac vs threads
- overlap_ratio vs threads
- wait_skew vs threads
- stacked phase-time breakdown vs threads
- wait p50/p95 vs threads
- nb_test polling tradeoff scatter
- confidence summary markdown

Inputs may be:
- run directories (containing results.csv)
- directories to scan recursively for results.csv
- CSV files (either metrics CSV with wait_frac columns, or matrix index CSV with run_dir column)
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

try:  # pragma: no cover - runtime dependency check
    import matplotlib.pyplot as plt
    HAS_MATPLOTLIB = True
except ImportError:
    plt = None  # type: ignore[assignment]
    HAS_MATPLOTLIB = False


def _to_float(row: Dict[str, str], key: str, default: float = 0.0) -> float:
    value = row.get(key, "")
    if value is None or value == "":
        return default
    try:
        return float(value)
    except ValueError:
        return default


def _to_int(row: Dict[str, str], key: str, default: int = 0) -> int:
    value = row.get(key, "")
    if value is None or value == "":
        return default
    try:
        return int(float(value))
    except ValueError:
        return default


def _extract_intensity_from_run_id(run_id: str) -> Tuple[int, int]:
    match = re.search(r"_f(\d+)_b(\d+)", run_id)
    if not match:
        return (0, 0)
    return (int(match.group(1)), int(match.group(2)))


def _read_csv_rows(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        return list(reader)

def _read_csv_header(path: Path) -> List[str]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        return list(reader.fieldnames or [])


def _looks_like_metrics_csv(header: Iterable[str]) -> bool:
    keys = set(header)
    return {"wait_frac", "overlap_ratio", "wait_skew", "mode"}.issubset(keys)


def _looks_like_index_csv(header: Iterable[str]) -> bool:
    keys = set(header)
    return "run_dir" in keys and "status" in keys


def _load_metrics_from_results_csv(csv_path: Path, run_dir: Optional[Path]) -> List[Dict[str, object]]:
    rows = _read_csv_rows(csv_path)
    loaded: List[Dict[str, object]] = []
    for row in rows:
        run_id = row.get("run_id", "")
        flops = _to_int(row, "flops_per_point", -1)
        bytes_pp = _to_int(row, "bytes_per_point", -1)
        if flops < 0 or bytes_pp < 0:
            flops, bytes_pp = _extract_intensity_from_run_id(run_id)
        loaded.append(
            {
                "source_csv": csv_path,
                "run_dir": run_dir,
                "run_id": run_id,
                "mode": row.get("mode", "unknown"),
                "halo": _to_int(row, "H", 0),
                "threads": _to_int(row, "omp_threads", 0),
                "wait_frac": _to_float(row, "wait_frac", 0.0),
                "overlap_ratio": _to_float(row, "overlap_ratio", 0.0),
                "wait_skew": _to_float(row, "wait_skew", 0.0),
                "t_post_us": _to_float(row, "t_post_us", 0.0),
                "t_interior_us": _to_float(row, "t_interior_us", 0.0),
                "t_wait_us": _to_float(row, "t_wait_us", 0.0),
                "t_boundary_us": _to_float(row, "t_boundary_us", 0.0),
                "t_wait_p50_us": _to_float(row, "t_wait_p50_us", 0.0),
                "t_wait_p95_us": _to_float(row, "t_wait_p95_us", 0.0),
                "mpi_test_calls": _to_float(row, "mpi_test_calls", 0.0),
                "polls_to_complete_mean": _to_float(row, "polls_to_complete_mean", 0.0),
                "flops_per_point": flops,
                "bytes_per_point": bytes_pp,
            }
        )
    return loaded


def _load_from_index_csv(index_path: Path) -> List[Dict[str, object]]:
    rows = _read_csv_rows(index_path)
    loaded: List[Dict[str, object]] = []
    for row in rows:
        if row.get("status", "ok") != "ok":
            continue
        run_dir_value = row.get("run_dir", "")
        if not run_dir_value:
            continue
        run_dir = Path(run_dir_value)
        if not run_dir.is_absolute():
            candidate_from_index = (index_path.parent / run_dir).resolve()
            candidate_from_cwd = (Path.cwd() / run_dir).resolve()
            if candidate_from_index.exists():
                run_dir = candidate_from_index
            else:
                run_dir = candidate_from_cwd
        results_csv = run_dir / "results.csv"
        if not results_csv.exists():
            continue
        loaded.extend(_load_metrics_from_results_csv(results_csv, run_dir))
    return loaded


def discover_rows(inputs: List[Path]) -> List[Dict[str, object]]:
    discovered: List[Dict[str, object]] = []
    for path in inputs:
        if path.is_dir():
            direct_csv = path / "results.csv"
            if direct_csv.exists():
                discovered.extend(_load_metrics_from_results_csv(direct_csv, path))
                continue
            for csv_path in sorted(path.rglob("results.csv")):
                discovered.extend(_load_metrics_from_results_csv(csv_path, csv_path.parent))
            continue

        if path.is_file():
            header = _read_csv_header(path)
            if _looks_like_metrics_csv(header):
                discovered.extend(_load_metrics_from_results_csv(path, path.parent))
                continue
            if _looks_like_index_csv(header):
                discovered.extend(_load_from_index_csv(path))
                continue
            print(f"[analyze] warn: unrecognized CSV schema, skipping {path}", file=sys.stderr)
            continue

        print(f"[analyze] warn: input path does not exist, skipping {path}", file=sys.stderr)
    return discovered


def apply_filters(
    rows: List[Dict[str, object]],
    mode_filter: Optional[List[str]],
    halo_filter: Optional[List[int]],
    flops_filter: Optional[List[int]],
    bytes_filter: Optional[List[int]],
) -> List[Dict[str, object]]:
    filtered = rows
    if mode_filter:
        modes = {m.strip() for m in mode_filter if m.strip()}
        filtered = [r for r in filtered if str(r["mode"]) in modes]
    if halo_filter:
        halos = set(halo_filter)
        filtered = [r for r in filtered if int(r["halo"]) in halos]
    if flops_filter:
        allowed = set(flops_filter)
        filtered = [r for r in filtered if int(r["flops_per_point"]) in allowed]
    if bytes_filter:
        allowed = set(bytes_filter)
        filtered = [r for r in filtered if int(r["bytes_per_point"]) in allowed]
    return filtered


def _aggregate_by_thread(rows: List[Dict[str, object]], metric: str) -> Dict[str, List[Tuple[int, float]]]:
    grouped: Dict[str, Dict[int, List[float]]] = defaultdict(lambda: defaultdict(list))
    for row in rows:
        mode = str(row["mode"])
        threads = int(row["threads"])
        value = float(row[metric])
        if threads <= 0:
            continue
        grouped[mode][threads].append(value)

    out: Dict[str, List[Tuple[int, float]]] = {}
    for mode, by_threads in grouped.items():
        pairs: List[Tuple[int, float]] = []
        for threads, values in by_threads.items():
            pairs.append((threads, sum(values) / len(values)))
        pairs.sort(key=lambda item: item[0])
        out[mode] = pairs
    return out


def _aggregate_phase_components(rows: List[Dict[str, object]]) -> Dict[str, Dict[int, Dict[str, float]]]:
    grouped: Dict[str, Dict[int, Dict[str, List[float]]]] = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    for row in rows:
        mode = str(row["mode"])
        threads = int(row["threads"])
        if threads <= 0:
            continue
        grouped[mode][threads]["t_post_us"].append(float(row["t_post_us"]))
        grouped[mode][threads]["t_interior_us"].append(float(row["t_interior_us"]))
        grouped[mode][threads]["t_wait_us"].append(float(row["t_wait_us"]))
        grouped[mode][threads]["t_boundary_us"].append(float(row["t_boundary_us"]))

    out: Dict[str, Dict[int, Dict[str, float]]] = defaultdict(dict)
    for mode, by_threads in grouped.items():
        for threads, metrics in by_threads.items():
            out[mode][threads] = {
                key: (sum(vals) / len(vals) if vals else 0.0) for key, vals in metrics.items()
            }
    return out


def _mean(values: List[float]) -> float:
    if not values:
        return 0.0
    return sum(values) / len(values)


def _stddev(values: List[float]) -> float:
    if len(values) <= 1:
        return 0.0
    mu = _mean(values)
    return math.sqrt(sum((v - mu) ** 2 for v in values) / (len(values) - 1))


def _percentile(values: List[float], p: float) -> float:
    if not values:
        return 0.0
    if p <= 0:
        return min(values)
    if p >= 100:
        return max(values)
    sorted_vals = sorted(values)
    idx = (len(sorted_vals) - 1) * (p / 100.0)
    lo = int(math.floor(idx))
    hi = int(math.ceil(idx))
    if lo == hi:
        return sorted_vals[lo]
    weight = idx - lo
    return sorted_vals[lo] * (1.0 - weight) + sorted_vals[hi] * weight


def write_metric_summary(rows: List[Dict[str, object]], out_path: Path) -> None:
    grouped: Dict[Tuple[str, int, int, int, int], List[Dict[str, object]]] = defaultdict(list)
    for row in rows:
        grouped[
            (
                str(row["mode"]),
                int(row["halo"]),
                int(row["flops_per_point"]),
                int(row["bytes_per_point"]),
                int(row["threads"]),
            )
        ].append(row)

    with out_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "mode",
                "halo",
                "flops_per_point",
                "bytes_per_point",
                "threads",
                "wait_frac",
                "overlap_ratio",
                "wait_skew",
                "samples",
            ]
        )
        for key in sorted(grouped.keys()):
            samples = grouped[key]
            wait_frac = sum(float(r["wait_frac"]) for r in samples) / len(samples)
            overlap_ratio = sum(float(r["overlap_ratio"]) for r in samples) / len(samples)
            wait_skew = sum(float(r["wait_skew"]) for r in samples) / len(samples)
            writer.writerow(
                [
                    key[0],
                    key[1],
                    key[2],
                    key[3],
                    key[4],
                    f"{wait_frac:.8f}",
                    f"{overlap_ratio:.8f}",
                    f"{wait_skew:.8f}",
                    len(samples),
                ]
            )


def plot_metric(rows: List[Dict[str, object]], metric: str, title: str, ylabel: str, out_path: Path) -> bool:
    if not HAS_MATPLOTLIB:
        return False
    facets = sorted(
        {
            (
                int(r["halo"]),
                int(r["flops_per_point"]),
                int(r["bytes_per_point"]),
            )
            for r in rows
        }
    )
    if not facets:
        return False
    ncols = min(3, len(facets))
    nrows = (len(facets) + ncols - 1) // ncols
    fig, axes = plt.subplots(nrows=nrows, ncols=ncols, figsize=(5 * ncols, 3.5 * nrows), squeeze=False)

    for idx, (halo, flops_per_point, bytes_per_point) in enumerate(facets):
        ax = axes[idx // ncols][idx % ncols]
        facet_rows = [
            r
            for r in rows
            if int(r["halo"]) == halo
            and int(r["flops_per_point"]) == flops_per_point
            and int(r["bytes_per_point"]) == bytes_per_point
        ]
        series = _aggregate_by_thread(facet_rows, metric)
        for mode in sorted(series.keys()):
            xs = [p[0] for p in series[mode]]
            ys = [p[1] for p in series[mode]]
            ax.plot(xs, ys, marker="o", label=mode)
        ax.set_title(f"H={halo} f={flops_per_point} b={bytes_per_point}")
        ax.set_xlabel("threads")
        ax.set_ylabel(ylabel)
        ax.grid(True, alpha=0.3)
        if series:
            ax.legend(fontsize=8)

    for idx in range(len(facets), nrows * ncols):
        axes[idx // ncols][idx % ncols].axis("off")

    fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    return True


def plot_phase_stack(rows: List[Dict[str, object]], out_path: Path) -> bool:
    if not HAS_MATPLOTLIB:
        return False
    facets = sorted(
        {
            (int(r["halo"]), int(r["flops_per_point"]), int(r["bytes_per_point"]))
            for r in rows
        }
    )
    if not facets:
        return False
    ncols = min(2, len(facets))
    nrows = (len(facets) + ncols - 1) // ncols
    fig, axes = plt.subplots(nrows=nrows, ncols=ncols, figsize=(7 * ncols, 4 * nrows), squeeze=False)

    for idx, facet in enumerate(facets):
        halo, flops_per_point, bytes_per_point = facet
        ax = axes[idx // ncols][idx % ncols]
        facet_rows = [
            r
            for r in rows
            if int(r["halo"]) == halo
            and int(r["flops_per_point"]) == flops_per_point
            and int(r["bytes_per_point"]) == bytes_per_point
        ]
        agg = _aggregate_phase_components(facet_rows)
        phase_keys = ["t_post_us", "t_interior_us", "t_wait_us", "t_boundary_us"]
        phase_labels = ["post", "interior", "wait", "boundary"]
        mode_names = sorted(agg.keys())
        if not mode_names:
            ax.axis("off")
            continue

        x_labels: List[str] = []
        phase_values: Dict[str, List[float]] = {key: [] for key in phase_keys}
        for mode in mode_names:
            for threads in sorted(agg[mode].keys()):
                x_labels.append(f"{mode}\\nT{threads}")
                for key in phase_keys:
                    phase_values[key].append(agg[mode][threads].get(key, 0.0))

        x = list(range(len(x_labels)))
        bottom = [0.0] * len(x_labels)
        for key, label in zip(phase_keys, phase_labels):
            vals = phase_values[key]
            ax.bar(x, vals, bottom=bottom, label=label, width=0.75)
            bottom = [b + v for b, v in zip(bottom, vals)]

        ax.set_title(f"H={halo} f={flops_per_point} b={bytes_per_point}")
        ax.set_ylabel("time (us)")
        ax.set_xticks(x)
        ax.set_xticklabels(x_labels, fontsize=8)
        ax.grid(True, axis="y", alpha=0.25)
        ax.legend(fontsize=8, ncol=2)

    for idx in range(len(facets), nrows * ncols):
        axes[idx // ncols][idx % ncols].axis("off")

    fig.suptitle("Phase Time Breakdown (stacked means by mode/thread)")
    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    return True


def plot_wait_tail(rows: List[Dict[str, object]], out_path: Path) -> bool:
    if not HAS_MATPLOTLIB:
        return False
    facets = sorted(
        {
            (int(r["halo"]), int(r["flops_per_point"]), int(r["bytes_per_point"]))
            for r in rows
        }
    )
    if not facets:
        return False
    ncols = min(3, len(facets))
    nrows = (len(facets) + ncols - 1) // ncols
    fig, axes = plt.subplots(nrows=nrows, ncols=ncols, figsize=(5 * ncols, 3.5 * nrows), squeeze=False)

    for idx, facet in enumerate(facets):
        halo, flops_per_point, bytes_per_point = facet
        ax = axes[idx // ncols][idx % ncols]
        facet_rows = [
            r
            for r in rows
            if int(r["halo"]) == halo
            and int(r["flops_per_point"]) == flops_per_point
            and int(r["bytes_per_point"]) == bytes_per_point
        ]
        p50_series = _aggregate_by_thread(facet_rows, "t_wait_p50_us")
        p95_series = _aggregate_by_thread(facet_rows, "t_wait_p95_us")
        for mode in sorted(set(p50_series.keys()) | set(p95_series.keys())):
            if mode in p50_series:
                xs = [p[0] for p in p50_series[mode]]
                ys = [p[1] for p in p50_series[mode]]
                ax.plot(xs, ys, marker="o", label=f"{mode} p50")
            if mode in p95_series:
                xs = [p[0] for p in p95_series[mode]]
                ys = [p[1] for p in p95_series[mode]]
                ax.plot(xs, ys, marker="x", linestyle="--", label=f"{mode} p95")
        ax.set_title(f"H={halo} f={flops_per_point} b={bytes_per_point}")
        ax.set_xlabel("threads")
        ax.set_ylabel("t_wait (us)")
        ax.grid(True, alpha=0.3)
        if p50_series or p95_series:
            ax.legend(fontsize=7)

    for idx in range(len(facets), nrows * ncols):
        axes[idx // ncols][idx % ncols].axis("off")

    fig.suptitle("Wait Tail Behavior (p50/p95)")
    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    return True


def plot_nbtest_poll_tradeoff(rows: List[Dict[str, object]], out_path: Path) -> bool:
    if not HAS_MATPLOTLIB:
        return False
    nb_rows = [r for r in rows if str(r["mode"]) == "nb_test"]
    if not nb_rows:
        return False
    fig, ax = plt.subplots(figsize=(7, 5))
    xs = [float(r["polls_to_complete_mean"]) for r in nb_rows]
    ys = [float(r["wait_frac"]) for r in nb_rows]
    sizes = [max(12.0, float(r["mpi_test_calls"]) / 5.0) for r in nb_rows]
    ax.scatter(xs, ys, s=sizes, alpha=0.6)
    ax.set_xlabel("polls_to_complete_mean")
    ax.set_ylabel("wait_frac")
    ax.set_title("nb_test Polling Tradeoff (marker size ~ mpi_test_calls)")
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    return True


def write_confidence_summary(rows: List[Dict[str, object]], out_path: Path) -> None:
    grouped: Dict[Tuple[str, int, int, int, int], List[Dict[str, object]]] = defaultdict(list)
    for row in rows:
        grouped[
            (
                str(row["mode"]),
                int(row["halo"]),
                int(row["flops_per_point"]),
                int(row["bytes_per_point"]),
                int(row["threads"]),
            )
        ].append(row)

    sample_sizes = [len(v) for v in grouped.values()]
    min_repeats = min(sample_sizes) if sample_sizes else 0
    median_repeats = int(_percentile([float(x) for x in sample_sizes], 50)) if sample_sizes else 0

    cv_values = []
    for samples in grouped.values():
        vals = [float(r["wait_frac"]) for r in samples]
        mu = _mean(vals)
        if mu > 0:
            cv_values.append(_stddev(vals) / mu)
    median_cv = _percentile(cv_values, 50) if cv_values else 0.0

    blk_overlap = [float(r["overlap_ratio"]) for r in rows if str(r["mode"]) == "phase_blk"]
    blk_overlap_p95 = _percentile(blk_overlap, 95) if blk_overlap else 0.0
    blk_sane = bool(blk_overlap) and blk_overlap_p95 <= 0.15

    by_signature: Dict[Tuple[int, int, int, int], Dict[str, List[float]]] = defaultdict(lambda: defaultdict(list))
    for r in rows:
        sig = (int(r["halo"]), int(r["flops_per_point"]), int(r["bytes_per_point"]), int(r["threads"]))
        by_signature[sig][str(r["mode"])].append(float(r["overlap_ratio"]))
    comparisons = 0
    nb_better = 0
    for sig, bucket in by_signature.items():
        if "phase_nb" in bucket and "phase_blk" in bucket:
            comparisons += 1
            if _mean(bucket["phase_nb"]) > _mean(bucket["phase_blk"]):
                nb_better += 1
    nb_better_ratio = (nb_better / comparisons) if comparisons else 0.0

    confidence = "LOW"
    if min_repeats >= 5 and median_cv <= 0.15 and blk_sane and nb_better_ratio >= 0.7:
        confidence = "HIGH"
    elif min_repeats >= 3 and median_cv <= 0.25 and blk_sane and nb_better_ratio >= 0.6:
        confidence = "MEDIUM"

    lines = [
        "# PhaseGap Confidence Summary",
        "",
        f"- Overall confidence: **{confidence}**",
        f"- Config groups analyzed: **{len(grouped)}**",
        f"- Min repeats per group: **{min_repeats}**",
        f"- Median repeats per group: **{median_repeats}**",
        f"- Median wait_frac CV: **{median_cv:.3f}**",
        f"- phase_blk overlap p95: **{blk_overlap_p95:.4f}** (sanity target <= 0.15)",
        f"- phase_nb overlap > phase_blk in matched signatures: **{nb_better}/{comparisons} ({nb_better_ratio:.1%})**",
        "",
        "Interpretation hints:",
        "- If confidence is LOW, add repeats and stabilize host load/affinity before making strong claims.",
        "- If phase_blk overlap sanity fails, re-check orchestration semantics before interpreting trends.",
        "- Use p95 wait trends and wait_skew to separate bottlenecks from imbalance/noise.",
    ]
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def print_trace_suggestions(rows: List[Dict[str, object]], max_suggestions: int) -> None:
    ranked = sorted(rows, key=lambda r: float(r["wait_frac"]), reverse=True)
    suggestions: List[Tuple[Path, Dict[str, object]]] = []
    seen = set()
    for row in ranked:
        run_dir = row.get("run_dir")
        if not isinstance(run_dir, Path):
            continue
        trace_path = run_dir / "trace.json"
        if trace_path.exists() and trace_path not in seen:
            suggestions.append((trace_path, row))
            seen.add(trace_path)
        if len(suggestions) >= max_suggestions:
            break

    if not suggestions:
        print("[analyze] trace suggestions: none (no trace.json files found in selected rows)")
        return

    print("[analyze] representative trace suggestions:")
    for trace_path, row in suggestions:
        print(
            f"  - {trace_path} "
            f"(mode={row['mode']}, H={row['halo']}, T={row['threads']}, wait_frac={row['wait_frac']:.4f})"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate default PhaseGap analysis plots")
    parser.add_argument(
        "--input",
        action="append",
        required=True,
        help="Input path(s): run dir, parent dir, metrics CSV, or matrix index CSV (repeatable)",
    )
    parser.add_argument("--out-dir", default="runs/analysis", help="Output directory for plots")
    parser.add_argument("--mode", action="append", help="Filter mode(s), e.g. --mode phase_nb")
    parser.add_argument("--halo", action="append", type=int, help="Filter halo widths, e.g. --halo 256")
    parser.add_argument(
        "--flops-per-point",
        action="append",
        type=int,
        help="Filter flops_per_point values, e.g. --flops-per-point 64",
    )
    parser.add_argument(
        "--bytes-per-point",
        action="append",
        type=int,
        help="Filter bytes_per_point values, e.g. --bytes-per-point 0",
    )
    parser.add_argument(
        "--max-trace-suggestions",
        type=int,
        default=3,
        help="Number of representative trace files to print",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    input_paths = [Path(p).resolve() for p in args.input]
    rows = discover_rows(input_paths)
    rows = apply_filters(rows, args.mode, args.halo, args.flops_per_point, args.bytes_per_point)
    if not rows:
        print("[analyze] fail: no usable rows found after loading/filtering", file=sys.stderr)
        return 1

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    wrote_wait = plot_metric(
        rows,
        metric="wait_frac",
        title="wait_frac vs threads (faceted by halo + intensity)",
        ylabel="wait_frac",
        out_path=out_dir / "wait_frac_vs_threads.png",
    )
    wrote_overlap = plot_metric(
        rows,
        metric="overlap_ratio",
        title="overlap_ratio vs threads (faceted by halo + intensity)",
        ylabel="overlap_ratio",
        out_path=out_dir / "overlap_ratio_vs_threads.png",
    )
    wrote_skew = plot_metric(
        rows,
        metric="wait_skew",
        title="wait_skew vs threads (faceted by halo + intensity)",
        ylabel="wait_skew",
        out_path=out_dir / "wait_skew_vs_threads.png",
    )
    wrote_phase_stack = plot_phase_stack(rows, out_dir / "phase_stack_vs_threads.png")
    wrote_wait_tail = plot_wait_tail(rows, out_dir / "wait_p50_p95_vs_threads.png")
    wrote_poll_tradeoff = plot_nbtest_poll_tradeoff(rows, out_dir / "nbtest_poll_tradeoff.png")
    write_metric_summary(rows, out_dir / "metric_summary.csv")
    write_confidence_summary(rows, out_dir / "confidence_summary.md")

    if wrote_wait and wrote_overlap and wrote_skew:
        print(f"[analyze] wrote plots to {out_dir}")
        if wrote_phase_stack:
            print(f"[analyze] wrote {out_dir / 'phase_stack_vs_threads.png'}")
        if wrote_wait_tail:
            print(f"[analyze] wrote {out_dir / 'wait_p50_p95_vs_threads.png'}")
        if wrote_poll_tradeoff:
            print(f"[analyze] wrote {out_dir / 'nbtest_poll_tradeoff.png'}")
    else:
        print(
            f"[analyze] matplotlib not available; wrote {out_dir / 'metric_summary.csv'} only "
            "(install matplotlib to enable PNG plots)"
        )
    print(f"[analyze] wrote {out_dir / 'confidence_summary.md'}")
    print_trace_suggestions(rows, max_suggestions=max(1, args.max_trace_suggestions))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
