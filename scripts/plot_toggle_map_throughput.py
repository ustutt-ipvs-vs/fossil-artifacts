#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt


PLOT_COLORS = (
    "#E69F00",
    "#009E73",
    "#0072B2",
)

DISPLAY_NAMES = {
    "fossil-sharded-pmap": "Fossil sharded pmap",
    "leveldb": "LevelDB",
    "bdb": "Berkeley DB",
}

BENCHMARKS = ("fossil-sharded-pmap", "leveldb", "bdb")


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parent.parent
    default_input = repo_root / "build" / "toggle_map_benchmarks.csv"
    default_output = repo_root / "build" / "toggle_map_benchmarks_throughput_ops_per_s.png"

    parser = argparse.ArgumentParser(
        description=(
            "Plot toggle map benchmark throughput for Fossil sharded pmap, "
            "LevelDB, and Berkeley DB."
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
        default=default_output,
        help=f"Output image path (default: {default_output})",
    )
    parser.add_argument(
        "--metric",
        choices=("average_ns", "median_ns"),
        default="average_ns",
        help="Latency column used to derive per-run throughput.",
    )
    parser.add_argument(
        "--aggregate",
        choices=("mean", "median"),
        default="mean",
        help="How to aggregate repeated throughput samples for each benchmark/thread pair.",
    )
    parser.add_argument(
        "--no-error-band",
        action="store_true",
        help="Disable the +/- one standard deviation band around each curve.",
    )
    parser.add_argument(
        "--no-error-bars",
        action="store_true",
        help="Disable capped +/- one standard deviation error bars around each point.",
    )
    parser.add_argument(
        "--log2-x",
        action="store_true",
        help="Use a base-2 logarithmic x-axis for threads.",
    )
    parser.add_argument(
        "--title",
        default="Toggle map benchmark throughput",
        help="Plot title.",
    )
    return parser.parse_args()


def load_rows(csv_path: Path) -> list[dict[str, object]]:
    if not csv_path.is_file():
        raise FileNotFoundError(f"CSV file not found: {csv_path}")

    required_columns = {
        "benchmark",
        "threads",
        "shards",
        "run",
        "num",
        "average_ns",
        "median_ns",
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

        for line_number, row in enumerate(reader, start=2):
            benchmark = str(row["benchmark"])
            if benchmark not in BENCHMARKS:
                continue

            try:
                rows.append(
                    {
                        "benchmark": benchmark,
                        "threads": int(row["threads"]),
                        "shards": int(row["shards"]),
                        "run": int(row["run"]),
                        "num": int(row["num"]),
                        "average_ns": float(row["average_ns"]),
                        "median_ns": float(row["median_ns"]),
                        "stddev_ns": float(row["stddev_ns"]),
                    }
                )
            except (TypeError, ValueError) as exc:
                raise ValueError(
                    f"Could not parse CSV row {line_number} in {csv_path}: {exc}"
                ) from exc

    if not rows:
        raise ValueError(
            "No rows were found for fossil-sharded-pmap, leveldb, or bdb in the CSV input"
        )
    return rows


def build_throughput_samples(
    rows: list[dict[str, object]], metric: str
) -> dict[tuple[str, int, int], list[tuple[float, float]]]:
    throughput_samples: dict[tuple[str, int, int], list[tuple[float, float]]] = defaultdict(list)

    for row in rows:
        latency_ns = float(row[metric])
        stddev_ns = float(row["stddev_ns"])
        threads = int(row["threads"])

        if latency_ns <= 0.0:
            raise ValueError(
                f"Encountered non-positive latency for benchmark={row['benchmark']!r}, "
                f"threads={threads}, run={row['run']}: {latency_ns}"
            )
        if stddev_ns < 0.0:
            raise ValueError(
                f"Encountered negative stddev for benchmark={row['benchmark']!r}, "
                f"threads={threads}, run={row['run']}: {stddev_ns}"
            )

        throughput_ops_per_s = threads * 1_000_000_000.0 / latency_ns
        throughput_spread = throughput_ops_per_s * stddev_ns / latency_ns
        throughput_samples[
            (str(row["benchmark"]), threads, int(row["shards"]))
        ].append((throughput_ops_per_s, throughput_spread))

    if not throughput_samples:
        raise ValueError("No throughput samples could be derived from the CSV data")
    return throughput_samples


def aggregate_samples(
    throughput_samples: dict[tuple[str, int, int], list[tuple[float, float]]], aggregate: str
) -> dict[tuple[str, int, int], dict[str, object]]:
    aggregated: dict[tuple[str, int, int], dict[str, object]] = {}
    for (benchmark, threads, shards), samples in sorted(throughput_samples.items()):
        centers = [sample[0] for sample in samples]
        spreads = [sample[1] for sample in samples]
        center = statistics.mean(centers) if aggregate == "mean" else statistics.median(centers)
        spread = statistics.mean(spreads) if aggregate == "mean" else statistics.median(spreads)

        aggregated[(benchmark, threads, shards)] = {
            "benchmark": benchmark,
            "threads": threads,
            "shards": shards,
            "center": center,
            "spread": spread,
        }

    return aggregated


def series_label(benchmark: str, shards: int, benchmark_shards: set[int]) -> str:
    label = DISPLAY_NAMES.get(benchmark, benchmark)
    if benchmark == "fossil-sharded-pmap" or len(benchmark_shards) > 1:
        return f"{label} (shards={shards})"
    return label


def plot_throughput(
    aggregated: dict[tuple[str, int, int], dict[str, object]],
    output_path: Path,
    title: str,
    log2_x: bool,
    show_error_band: bool,
    show_error_bars: bool,
) -> None:
    per_series: dict[tuple[str, int], list[dict[str, object]]] = defaultdict(list)
    shard_values: dict[str, set[int]] = defaultdict(set)

    for point in aggregated.values():
        benchmark = str(point["benchmark"])
        shards = int(point["shards"])
        per_series[(benchmark, shards)].append(point)
        shard_values[benchmark].add(shards)

    ordered_series: list[tuple[str, int]] = []
    for benchmark in BENCHMARKS:
        for shards in sorted(shard_values[benchmark]):
            ordered_series.append((benchmark, shards))

    if not ordered_series:
        raise ValueError("No benchmark series were available to plot")

    figure, axis = plt.subplots(figsize=(10, 6))

    for index, (benchmark, shards) in enumerate(ordered_series):
        points = sorted(per_series[(benchmark, shards)], key=lambda item: int(item["threads"]))
        x_values = [int(point["threads"]) for point in points]
        y_values = [float(point["center"]) for point in points]
        spreads = [float(point["spread"]) for point in points]
        color = PLOT_COLORS[index % len(PLOT_COLORS)]

        if show_error_bars:
            lower_errors = [
                min(spread, value)
                for value, spread in zip(y_values, spreads)
            ]
            axis.errorbar(
                x_values,
                y_values,
                yerr=[lower_errors, spreads],
                marker="o",
                linewidth=2.0,
                markersize=5,
                label=series_label(benchmark, shards, shard_values[benchmark]),
                color=color,
                capsize=2.5,
                elinewidth=0.8,
                capthick=0.8,
            )
        else:
            axis.plot(
                x_values,
                y_values,
                marker="o",
                linewidth=2.0,
                markersize=5,
                label=series_label(benchmark, shards, shard_values[benchmark]),
                color=color,
            )

        if show_error_band:
            lower = [max(value - spread, 0.0) for value, spread in zip(y_values, spreads)]
            upper = [value + spread for value, spread in zip(y_values, spreads)]
            axis.fill_between(x_values, lower, upper, color=color, alpha=0.12)

    if log2_x:
        axis.set_xscale("log", base=2)

    axis.set_title(title)
    axis.set_xlabel("threads")
    axis.set_ylabel("throughput (ops/s)")
    axis.grid(True, axis="y", alpha=0.3)
    axis.legend()

    output_path.parent.mkdir(parents=True, exist_ok=True)
    figure.tight_layout()
    figure.savefig(output_path, dpi=200)
    plt.close(figure)


def main() -> int:
    args = parse_args()
    rows = load_rows(args.input)
    throughput_samples = build_throughput_samples(rows, args.metric)
    aggregated = aggregate_samples(throughput_samples, args.aggregate)
    plot_throughput(
        aggregated=aggregated,
        output_path=args.output,
        title=args.title,
        log2_x=args.log2_x,
        show_error_band=not args.no_error_band,
        show_error_bars=not args.no_error_bars,
    )
    print(f"Wrote plot to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
