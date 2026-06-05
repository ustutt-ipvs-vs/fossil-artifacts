#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import math
import statistics
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt


DISPLAY_NAMES = {
    "leveldb": "LevelDB",
    "bdb": "Berkeley DB",
    "fossildb": "FossilDB",
    "unordered-map-rwlock": "volatile hash map",
    "trinitydb-fc": "TrinityDB (VR+FC)",
    "trinitydb-tl2": "TrinityDB (VR+TL2)",
    "romulusdb": "RomulusDB",
}

PANEL_SPECS = (
    {"title": "fillseq", "benchmark": "fill-seq", "axis_group": "write"},
    {"title": "fillrandom", "benchmark": "fill-random", "axis_group": "write"},
    {"title": "overwrite", "benchmark": "override", "axis_group": "write"},
    {"title": "readrandom", "benchmark": "read-random", "axis_group": "read"},
    {"title": "readseq", "benchmark": "read-seq", "axis_group": "read"},
    {"title": "readwrite 50/50", "benchmark": "read-write", "axis_group": "mixed"},
)

SERIES_STYLE = {
    "leveldb": {"color": "#1f77b4", "marker": "^", "linestyle": "-", "linewidth": 1.2},
    "bdb": {"color": "#9467bd", "marker": "s", "linestyle": ":", "linewidth": 1.2},
    "fossildb": {"color": "#d62728", "marker": "o", "linestyle": "--", "linewidth": 1.3},
    "unordered-map-rwlock": {
        "color": "#ff7f0e",
        "marker": "D",
        "linestyle": "-.",
        "linewidth": 1.2,
    },
    "trinitydb-fc": {"color": "#2ca02c", "marker": "*", "linestyle": "-", "linewidth": 1.3},
    "trinitydb-tl2": {"color": "#bcbd22", "marker": "x", "linestyle": "-", "linewidth": 1.3},
    "romulusdb": {"color": "#8c564b", "marker": "P", "linestyle": "--", "linewidth": 1.2},
}

PREFERRED_BACKEND_ORDER = (
    "trinitydb-tl2",
    "trinitydb-fc",
    "fossildb",
    "unordered-map-rwlock",
    "leveldb",
    "romulusdb",
    "bdb",
)


@dataclass(frozen=True)
class MetricSummary:
    center: float
    spread: float


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parent.parent
    default_input = repo_root / "build" / "db_benchmarks.csv"

    parser = argparse.ArgumentParser(
        description=(
            "Plot DB benchmark results as a compact multi-panel workload figure "
            "with backend curves over thread count."
        )
    )
    parser.add_argument(
        "--input",
        type=Path,
        default=default_input,
        help=f"Input CSV path (default: {default_input})",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Output image path. Defaults depend on --metric.",
    )
    parser.add_argument(
        "--metric",
        choices=("average_ns", "median_ns", "throughput_ops_per_s"),
        default="throughput_ops_per_s",
        help="Per-run metric to plot.",
    )
    parser.add_argument(
        "--aggregate",
        choices=("mean", "median"),
        default="mean",
        help="How to aggregate repeated samples for each backend/thread pair.",
    )
    args = parser.parse_args()
    if args.output is None:
        if args.metric == "throughput_ops_per_s":
            args.output = repo_root / "build" / "db_benchmarks_throughput_ops_per_s.png"
        else:
            args.output = repo_root / "build" / "db_benchmarks_micros_per_op.png"
    return args


def load_rows(csv_path: Path) -> list[dict[str, object]]:
    if not csv_path.is_file():
        raise FileNotFoundError(f"CSV file not found: {csv_path}")

    required_columns = {
        "backend",
        "backend_display",
        "benchmark",
        "threads",
        "shards",
        "run",
        "num",
        "reads",
        "value_size",
        "average_ns",
        "median_ns",
        "min_ns",
        "max_ns",
        "stddev_ns",
    }

    rows: list[dict[str, object]] = []
    with csv_path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError(f"CSV file has no header: {csv_path}")

        missing = required_columns.difference(reader.fieldnames)
        if missing:
            missing_list = ", ".join(sorted(missing))
            raise ValueError(f"CSV file is missing columns: {missing_list}")

        has_wall_time = "wall_time_ns" in reader.fieldnames
        has_throughput = "throughput_ops_per_s" in reader.fieldnames

        for line_number, row in enumerate(reader, start=2):
            try:
                rows.append(
                    {
                        "backend": str(row["backend"]),
                        "backend_display": str(row["backend_display"]),
                        "benchmark": str(row["benchmark"]),
                        "threads": int(row["threads"]),
                        "shards": int(row["shards"]),
                        "run": int(row["run"]),
                        "num": int(row["num"]),
                        "reads": int(row["reads"]),
                        "value_size": int(row["value_size"]),
                        "average_ns": float(row["average_ns"]),
                        "median_ns": float(row["median_ns"]),
                        "min_ns": float(row["min_ns"]),
                        "max_ns": float(row["max_ns"]),
                        "stddev_ns": float(row["stddev_ns"]),
                        "wall_time_ns": (
                            float(row["wall_time_ns"]) if has_wall_time else None
                        ),
                        "throughput_ops_per_s": (
                            float(row["throughput_ops_per_s"]) if has_throughput else None
                        ),
                    }
                )
            except (TypeError, ValueError) as exc:
                raise ValueError(
                    f"Could not parse CSV row {line_number} in {csv_path}: {exc}"
                ) from exc

    if not rows:
        raise ValueError(f"CSV file is empty: {csv_path}")
    return rows


def is_power_of_two(value: int) -> bool:
    return value > 0 and (value & (value - 1)) == 0


def build_aggregated_metrics(
    rows: list[dict[str, object]], metric: str, aggregate: str
) -> dict[tuple[str, str, int], MetricSummary]:
    metric_samples: dict[tuple[str, str, int], list[float]] = defaultdict(list)
    spread_samples: dict[tuple[str, str, int], list[float]] = defaultdict(list)
    shard_values: dict[tuple[str, str, int], set[int]] = defaultdict(set)

    for row in rows:
        backend = str(row["backend"])
        benchmark = str(row["benchmark"])
        threads = int(row["threads"])
        current_raw = row[metric]
        if current_raw is None:
            raise ValueError(
                f"CSV file does not contain metric {metric!r}. Re-run "
                "scripts/run_db_benchmarks.sh with the updated benchmark harness."
            )

        current = float(current_raw)
        spread = float(row["stddev_ns"])
        if metric == "throughput_ops_per_s":
            average_ns = float(row["average_ns"])
            if average_ns <= 0.0:
                raise ValueError(
                    f"Encountered non-positive average latency for backend={backend!r}, "
                    f"benchmark={benchmark!r}, threads={threads}, run={row['run']}: {average_ns}"
                )
            spread = current * spread / average_ns

        if current <= 0.0:
            raise ValueError(
                f"Encountered non-positive metric for backend={backend!r}, "
                f"benchmark={benchmark!r}, threads={threads}, run={row['run']}: {current}"
            )
        if spread < 0.0:
            raise ValueError(
                f"Encountered negative stddev for backend={backend!r}, "
                f"benchmark={benchmark!r}, threads={threads}, run={row['run']}: {spread}"
            )

        metric_samples[(backend, benchmark, threads)].append(current)
        spread_samples[(backend, benchmark, threads)].append(spread)
        shard_values[(backend, benchmark, threads)].add(int(row["shards"]))

    aggregated: dict[tuple[str, str, int], MetricSummary] = {}
    for key, samples in sorted(metric_samples.items()):
        if len(shard_values[key]) > 1:
            raise ValueError(
                "Plot expects one shard setting per backend/benchmark/thread pair, but "
                f"found {sorted(shard_values[key])} for backend={key[0]!r}, "
                f"benchmark={key[1]!r}, threads={key[2]}"
            )

        center = (
            statistics.mean(samples) if aggregate == "mean" else statistics.median(samples)
        )
        spread = (
            statistics.mean(spread_samples[key])
            if aggregate == "mean"
            else statistics.median(spread_samples[key])
        )
        aggregated[key] = MetricSummary(center=center, spread=spread)

    if not aggregated:
        raise ValueError("No aggregated benchmark values were produced from the CSV input")
    return aggregated


def metric_label(metric: str) -> str:
    if metric == "throughput_ops_per_s":
        return "Operations (x$10^6$/s)"
    return "Latency ($\\mu$s/op)"


def metric_transform(metric: str, value: float) -> float:
    if metric == "throughput_ops_per_s":
        return value / 1_000_000.0
    return value / 1000.0


def ordered_backends(aggregated: dict[tuple[str, str, int], MetricSummary]) -> list[str]:
    seen = {backend for backend, _benchmark, _threads in aggregated}
    return [backend for backend in PREFERRED_BACKEND_ORDER if backend in seen]


def compute_group_y_limits(
    aggregated: dict[tuple[str, str, int], MetricSummary],
    metric: str,
) -> dict[str, tuple[float, float]]:
    benchmarks_by_group: dict[str, set[str]] = defaultdict(set)
    for panel in PANEL_SPECS:
        benchmarks_by_group[str(panel["axis_group"])].add(str(panel["benchmark"]))

    limits: dict[str, tuple[float, float]] = {}
    for axis_group, benchmarks in benchmarks_by_group.items():
        plotted_ranges = [
            (
                max(
                    metric_transform(metric, summary.center)
                    - metric_transform(metric, summary.spread),
                    0.0,
                ),
                metric_transform(metric, summary.center)
                + metric_transform(metric, summary.spread),
            )
            for (_backend, benchmark, threads), summary in aggregated.items()
            if benchmark in benchmarks
        ]
        if not plotted_ranges:
            continue

        lower = min(value_range[0] for value_range in plotted_ranges)
        upper = max(value_range[1] for value_range in plotted_ranges)
        limits[axis_group] = (max(lower * 0.92, 1e-9), upper * 1.08)

    if not limits:
        raise ValueError(
            "No benchmark data with power-of-two thread counts was found in the CSV input"
        )

    return limits


def plot_series(
    axis: plt.Axes,
    aggregated: dict[tuple[str, str, int], MetricSummary],
    metric: str,
    benchmark_name: str,
    backends: list[str],
) -> None:
    for backend in backends:
        points = sorted(
            (
                (
                    threads,
                    metric_transform(metric, summary.center),
                    metric_transform(metric, summary.spread),
                )
                for (point_backend, point_benchmark, threads), summary in aggregated.items()
                if point_backend == backend
                and point_benchmark == benchmark_name
            ),
            key=lambda item: item[0],
        )
        if not points:
            continue

        style = SERIES_STYLE.get(backend, {})
        y_values = [point[1] for point in points]
        y_spreads = [point[2] for point in points]
        lower_errors = [
            min(spread, max(value - 1e-9, 0.0))
            for value, spread in zip(y_values, y_spreads)
        ]
        axis.errorbar(
            [point[0] for point in points],
            y_values,
            yerr=[lower_errors, y_spreads],
            label=DISPLAY_NAMES.get(backend, backend),
            markersize=5,
            capsize=2.5,
            elinewidth=0.8,
            capthick=0.8,
            **style,
        )


def configure_axis(
    axis: plt.Axes,
    title: str,
    x_ticks: list[int],
    y_limits: tuple[float, float],
    show_ylabel: bool,
    show_xlabel: bool,
    metric: str,
) -> None:
    axis.set_title(title, fontsize=10, fontweight="bold", pad=4)
    axis.set_yscale("log")
    axis.set_xlim(1, max(x_ticks) + 1)
    axis.set_xticks(x_ticks)
    axis.set_xticklabels([str(value) for value in x_ticks])
    axis.set_ylim(*y_limits)
    axis.grid(True, axis="y", linestyle=":", linewidth=0.8, color="#8F8F8F")
    axis.grid(False, axis="x")
    axis.tick_params(axis="both", labelsize=8)
    for spine in axis.spines.values():
        spine.set_linewidth(0.9)
        spine.set_color("#4D4D4D")

    if show_ylabel:
        axis.set_ylabel(metric_label(metric), fontsize=10)
    if show_xlabel:
        axis.set_xlabel("Number of threads", fontsize=10)

def plot_metrics(
    rows: list[dict[str, object]],
    aggregated: dict[tuple[str, str, int], MetricSummary],
    metric: str,
    output_path: Path,
) -> Path:
    x_ticks = sorted(
        # {2, 8, 16, 24, 32, 40, 48, 56, 64}
        {
            threads
            for (_backend, _benchmark, threads) in aggregated
            if threads % 8 == 0 or threads == 2
        }
    )
    if not x_ticks:
        raise ValueError(
            "No benchmark data with power-of-two thread counts was found in the CSV input"
        )


    active_panels = [
        panel
        for panel in PANEL_SPECS
        if any(
            benchmark == str(panel["benchmark"])
            for (_backend, benchmark, _threads) in aggregated
        )
    ]
    if not active_panels:
        raise ValueError("No configured benchmark panels were present in the CSV input")

    axis_groups = {x['axis_group'] for x in active_panels}

    legend_handles = None
    legend_labels = None

    for axis_group in axis_groups:
        legend_handles, legend_labels = plot_group(rows=rows, aggregated=aggregated, metric=metric, output_path=output_path, active_panels=active_panels, axis_group=axis_group, x_ticks=x_ticks)


    figure, legend_axis = plt.subplots(1, 1, figsize=(3, 4 / 1.618))
    legend_axis.axis("off")
    if legend_handles and legend_labels:
        legend_axis.legend(
            legend_handles,
            legend_labels,
            loc="center",
            frameon=False,
            fontsize=9,
            handlelength=2.6,
        )

    # figure.tight_layout(rect=(0.02, 0.04, 1.0, 0.95))
    figure.tight_layout()
    figure.savefig(str(output_path) + f'_legend.pdf', dpi=200)
    plt.close(figure)

def plot_group(
    rows: list[dict[str, object]],
    aggregated: dict[tuple[str, str, int], MetricSummary],
    metric: str,
    output_path: Path,
    axis_group: str,
    active_panels: dict[str, str],
    x_ticks: list[int]
) -> Path:
    backends = ordered_backends(aggregated)
    if not backends:
        raise ValueError("No benchmark series were available to plot")

    y_limits_by_group = compute_group_y_limits(aggregated, metric)

    num_panels = len([ 0 for x in active_panels if x['axis_group'] == axis_group ])
    legend_handles = None
    legend_labels = None

    figure, axes = plt.subplots(1, num_panels, figsize=(3 * num_panels, 4 / 1.618), sharex=True)
    axes_flat = axes if num_panels > 1 else [axes]

    index = -1
    for panel in active_panels:
        if panel['axis_group'] != axis_group:
            continue
        index += 1

        axis = axes_flat[index]
        benchmark_name = str(panel["benchmark"])
        plot_series(axis, aggregated, metric, benchmark_name, backends)
        configure_axis(
            axis=axis,
            title=str(panel["title"]),
            x_ticks=x_ticks,
            y_limits=y_limits_by_group[axis_group],
            show_ylabel=index == 0,
            show_xlabel=True,
            metric=metric,
        )
        if legend_handles is None:
            legend_handles, legend_labels = axis.get_legend_handles_labels()


    output_path.parent.mkdir(parents=True, exist_ok=True)
    figure.tight_layout(rect=(0.02, 0.04, 1.0, 0.95))
    figure.savefig(str(output_path) + f'_{axis_group}.pdf', dpi=200)
    plt.close(figure)

    return legend_handles, legend_labels


def main() -> int:
    args = parse_args()
    rows = load_rows(args.input)
    aggregated = build_aggregated_metrics(rows, args.metric, args.aggregate)
    plot_metrics(
        rows=rows,
        aggregated=aggregated,
        metric=args.metric,
        output_path=args.output,
    )
    print(f"Wrote plot to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
