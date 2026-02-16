#!/usr/bin/env python3
"""PhaseGap analysis utility (bd-1gh.5.3).

Generates default plots:
- wait_frac vs threads
- overlap_ratio vs threads
- wait_skew vs threads

Inputs may be:
- run directories (containing results.csv)
- directories to scan recursively for results.csv
- CSV files (either metrics CSV with wait_frac columns, or matrix index CSV with run_dir column)
"""

from __future__ import annotations

import argparse
import csv
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
    write_metric_summary(rows, out_dir / "metric_summary.csv")

    if wrote_wait and wrote_overlap and wrote_skew:
        print(f"[analyze] wrote plots to {out_dir}")
    else:
        print(
            f"[analyze] matplotlib not available; wrote {out_dir / 'metric_summary.csv'} only "
            "(install matplotlib to enable PNG plots)"
        )
    print_trace_suggestions(rows, max_suggestions=max(1, args.max_trace_suggestions))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
