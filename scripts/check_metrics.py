#!/usr/bin/env python3
"""Validate PhaseGap summary metrics from a smoke-run log.

This script parses the final `phasegap skeleton ready | key=value ...` line and
applies lightweight invariants to catch obvious timing/interpretation regressions.
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate PhaseGap smoke metrics.")
    parser.add_argument("--log", required=True, help="Path to smoke stdout/stderr log file")
    parser.add_argument(
        "--expect-measured-iters",
        type=int,
        default=None,
        help="Expected measured_iters value (typically iters - warmup)",
    )
    return parser.parse_args()


def parse_summary_line(text: str) -> dict[str, str]:
    summary_line = None
    for line in text.splitlines():
        if "phasegap skeleton ready" in line:
            summary_line = line.strip()
    if summary_line is None:
        raise ValueError("missing 'phasegap skeleton ready' summary line")

    fields: dict[str, str] = {}
    for part in summary_line.split("|"):
        token = part.strip()
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key.strip()] = value.strip()
    return fields


def require_key(fields: dict[str, str], key: str) -> str:
    if key not in fields:
        raise ValueError(f"missing required metric field: {key}")
    return fields[key]


def parse_float(fields: dict[str, str], key: str) -> float:
    raw = require_key(fields, key)
    try:
        value = float(raw)
    except ValueError as exc:
        raise ValueError(f"field {key} is not a float: {raw}") from exc
    if not math.isfinite(value):
        raise ValueError(f"field {key} is not finite: {raw}")
    return value


def parse_int(fields: dict[str, str], key: str) -> int:
    raw = require_key(fields, key)
    if not re.fullmatch(r"-?\d+", raw):
        raise ValueError(f"field {key} is not an integer: {raw}")
    return int(raw)


def main() -> int:
    args = parse_args()
    log_path = Path(args.log)
    if not log_path.is_file():
        print(f"[metrics] fail: log does not exist: {log_path}", file=sys.stderr)
        return 1

    text = log_path.read_text(encoding="utf-8", errors="replace")
    try:
        fields = parse_summary_line(text)
    except ValueError as exc:
        print(f"[metrics] fail: {exc}", file=sys.stderr)
        return 1

    try:
        measured_iters = parse_int(fields, "measured_iters")
        if measured_iters <= 0:
            raise ValueError(f"measured_iters must be > 0, got {measured_iters}")
        if args.expect_measured_iters is not None and measured_iters != args.expect_measured_iters:
            raise ValueError(
                "measured_iters mismatch: "
                f"expected {args.expect_measured_iters}, got {measured_iters}"
            )

        t_post = parse_float(fields, "t_post_us")
        t_interior = parse_float(fields, "t_interior_us")
        t_wait = parse_float(fields, "t_wait_us")
        t_boundary = parse_float(fields, "t_boundary_us")
        t_poll = parse_float(fields, "t_poll_us")
        t_comm = parse_float(fields, "t_comm_window_us")
        t_iter = parse_float(fields, "t_iter_us")

        for key, value in [
            ("t_post_us", t_post),
            ("t_interior_us", t_interior),
            ("t_wait_us", t_wait),
            ("t_boundary_us", t_boundary),
            ("t_poll_us", t_poll),
            ("t_comm_window_us", t_comm),
            ("t_iter_us", t_iter),
        ]:
            if value < 0.0:
                raise ValueError(f"{key} must be nonnegative, got {value}")

        eps = 1e-9
        if t_comm + eps < t_wait:
            raise ValueError(
                "t_comm_window_us must be >= t_wait_us "
                f"(got t_comm_window_us={t_comm}, t_wait_us={t_wait})"
            )

        wait_frac = parse_float(fields, "wait_frac")
        wait_skew = parse_float(fields, "wait_skew")
        overlap_ratio = parse_float(fields, "overlap_ratio")
        mpi_test_calls = parse_float(fields, "mpi_test_calls")
        mpi_wait_calls = parse_float(fields, "mpi_wait_calls")

        if wait_frac < -eps:
            raise ValueError(f"wait_frac must be >= 0, got {wait_frac}")
        if wait_skew < -eps:
            raise ValueError(f"wait_skew must be >= 0, got {wait_skew}")
        if overlap_ratio < -1e-6 or overlap_ratio > 1.0 + 1e-6:
            raise ValueError(f"overlap_ratio must be in [0,1], got {overlap_ratio}")
        if mpi_test_calls < -eps:
            raise ValueError(f"mpi_test_calls must be >= 0, got {mpi_test_calls}")
        if mpi_wait_calls < -eps:
            raise ValueError(f"mpi_wait_calls must be >= 0, got {mpi_wait_calls}")

        if t_iter > eps:
            implied_wait_frac = t_wait / t_iter
            if abs(implied_wait_frac - wait_frac) > 0.10:
                raise ValueError(
                    "wait_frac inconsistent with t_wait_us/t_iter_us "
                    f"(implied={implied_wait_frac}, reported={wait_frac})"
                )

    except ValueError as exc:
        print(f"[metrics] fail: {exc}", file=sys.stderr)
        return 1

    print("[metrics] pass: summary metrics invariants satisfied")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
