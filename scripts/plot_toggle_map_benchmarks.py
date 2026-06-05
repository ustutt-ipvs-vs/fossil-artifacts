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
from matplotlib.patches import Patch


DISPLAY_NAMES = {
    "fossil-prbtree": "Fossil pRBTree",
    "fossil-pmap": "Fossil pmap",
    "fossil-sharded-pmap": "FossilDB",
    "pmdk-hashmap": "PMDK hashmap",
    "pmdk-rbtree": "PMDK RBTree",
    "pmdk-vector": "PMDK vector",
    "std-map": "std::map",
    "std-unordered-map": "std::unordered_map",
    "std-vector": "std::vector",
    "fossil-pvec": "Fossil pvec",
    "romulus-rbtree": "RomulusLog RBTree",
    "romulus-hashmap": "RomulusLog hashmap",
    "romulus-vector": "RomulusLog vector",
    "trinity-fc-rbtree": "Trinity FC RBTree",
    "trinity-fc-hashmap": "Trinity FC hashmap",
    "trinity-fc-vector": "Trinity FC vector",
    "trinity-tl2-rbtree": "Trinity TL2 RBTree",
    "trinity-tl2-hashmap": "Trinity TL2 hashmap",
    "trinity-tl2-vector": "Trinity TL2 vector",
    "leveldb": "LevelDB",
}

PANEL_SPECS = (
    {
        "name": "vector",
        "title": "Vector",
        "benchmarks": (
            "std-vector",
            "fossil-pvec",
            # "romulus-vector",
            # "trinity-fc-vector",
            # "trinity-tl2-vector",
            "pmdk-vector",
        ),
        "baseline": "std-vector",
    },
    {
        "name": "map",
        "title": "Hash Map",
        "benchmarks": (
            "std-unordered-map",
            "fossil-pmap",
            # "fossil-sharded-pmap",
            # "romulus-hashmap",
            # "leveldb",
            # "trinity-fc-hashmap",
            # "trinity-tl2-hashmap",
            "pmdk-hashmap",
        ),
        "baseline": "std-unordered-map",
    },
    {
        "name": "rbtree",
        "title": "Red-Black Tree",
        "benchmarks": (
            "std-map",
            "fossil-prbtree",
            # "romulus-rbtree",
            # "trinity-fc-rbtree",
            # "trinity-tl2-rbtree",
            "pmdk-rbtree",
        ),
        "baseline": "std-map",
    },
)


FOSSIL_STYLE = {"color": "#ff7f0e", "marker": "D", "linestyle": "-.", "linewidth": 1.3}
VOLATILE_STYLE = {"color": "#1f77b4", "marker": "^", "linestyle": "-.", "linewidth": 1.2,}
PMDK_STYLE = {"color": "#2ca02c", "marker": "*", "linestyle": "-", "linewidth": 1.2,}
FOSSILDB_STYLE = {"color": "#d62728", "marker": "o", "linestyle": "--", "linewidth": 1.3}

SERIES_STYLE = {
    "fossil-pvec": FOSSILDB_STYLE,
    "fossil-pmap": FOSSILDB_STYLE,
    "fossil-prbtree": FOSSILDB_STYLE,
    "fossil-sharded-pmap": FOSSIL_STYLE,

    "std-vector": VOLATILE_STYLE,
    "std-unordered-map": VOLATILE_STYLE,
    "std-map": VOLATILE_STYLE,

    "pmdk-vector": PMDK_STYLE,
    "pmdk-rbtree": PMDK_STYLE,
    "pmdk-hashmap": PMDK_STYLE,

    "romulus-rbtree": {"color": "#cca02c", "marker": "x", "linestyle": "-", "linewidth": 1.2,},
    "romulus-hashmap": {"color": "#cca02c", "marker": "x", "linestyle": "-", "linewidth": 1.2,},
    "romulus-vector": {"color": "#cca02c", "marker": "x", "linestyle": "-", "linewidth": 1.2,},

    "leveldb": {"color": "#1f77b4", "marker": "^", "linestyle": "-", "linewidth": 1.2},

    "trinity-fc-vector": {"color": "#2ca02c", "marker": "*", "linestyle": "-", "linewidth": 1.3},
    "trinity-fc-hashmap": {"color": "#2ca02c", "marker": "*", "linestyle": "-", "linewidth": 1.3},
    "trinity-fc-rbtree": {"color": "#2ca02c", "marker": "*", "linestyle": "-", "linewidth": 1.3},
    "trinity-tl2-vector": {"color": "#bcbd22", "marker": "x", "linestyle": "-", "linewidth": 1.3},
    "trinity-tl2-hashmap": {"color": "#bcbd22", "marker": "x", "linestyle": "-", "linewidth": 1.3},
    "trinity-tl2-rbtree": {"color": "#bcbd22", "marker": "x", "linestyle": "-", "linewidth": 1.3},
}


@dataclass(frozen=True)
class MetricSummary:
    center: float
    spread: float



def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parent.parent
    default_input = repo_root / "build" / "toggle_map_benchmarks.csv"
    default_output = repo_root / "build" / "toggle_map_benchmarks_micros_per_op.png"

    parser = argparse.ArgumentParser(
        description=(
            "Plot grouped microbenchmark bars for vector, map, and RBTree "
            "using power-of-two thread counts."
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
        help="Per-run latency metric to plot.",
    )
    parser.add_argument(
        "--aggregate",
        choices=("mean", "median"),
        default="mean",
        help="How to aggregate repeated latency samples for each benchmark/thread pair.",
    )
    parser.add_argument(
        "--title",
        default="Microbenchmarks",
        help="Figure title.",
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

        for line_number, row in enumerate(reader, start=2):
            try:
                rows.append(
                    {
                        "benchmark": str(row["benchmark"]),
                        "threads": int(row["threads"]),
                        "shards": int(row["shards"]),
                        "run": int(row["run"]),
                        "num": int(row["num"]),
                        "average_ns": float(row["average_ns"]),
                        "median_ns": float(row["median_ns"]),
                        "min_ns": float(row["min_ns"]),
                        "max_ns": float(row["max_ns"]),
                        "stddev_ns": float(row["stddev_ns"]),
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
) -> dict[tuple[str, int], MetricSummary]:
    metric_samples: dict[tuple[str, int], list[float]] = defaultdict(list)
    spread_samples: dict[tuple[str, int], list[float]] = defaultdict(list)
    shard_values: dict[tuple[str, int], set[int]] = defaultdict(set)

    for row in rows:
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

        if current <= 0.0:
            raise ValueError(
                f"Encountered non-positive metric for "
                f"benchmark={benchmark!r}, threads={threads}, run={row['run']}: {current}"
            )
        if spread < 0.0:
            raise ValueError(
                f"Encountered negative stddev for benchmark={benchmark!r}, "
                f"threads={threads}, run={row['run']}: {spread}"
            )

        metric_samples[(benchmark, threads)].append(current)
        spread_samples[(benchmark, threads)].append(spread)
        shard_values[(benchmark, threads)].add(int(row["shards"]))

    aggregated: dict[tuple[str, int], MetricSummary] = {}
    for key, samples in sorted(metric_samples.items()):
        if len(shard_values[key]) > 1:
            raise ValueError(
                "Plot expects one shard setting per backend/benchmark/thread pair, but "
                f"found {sorted(shard_values[key])} for benchmark={key[0]!r}, "
                f"threads={key[1]}"
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


def output_path_for_panel(base_output_path: Path, panel_name: str) -> Path:
    return base_output_path.with_name(
        f"{base_output_path.stem}_{panel_name}.pdf"
    )


def render_panel(
    axis: plt.Axes,
    aggregated: dict[tuple[str, int], MetricSummary],
    benchmarks: tuple[str, ...],
    baseline: str,
) -> bool:
    ordered_benchmarks = [
        benchmark
        for benchmark in benchmarks
        if any(key[0] == benchmark for key in aggregated)
    ]
    if not ordered_benchmarks:
        axis.set_visible(False)
        return False

    x_labels = sorted(
        {
            threads
            for benchmark, threads in aggregated
            if benchmark in ordered_benchmarks and (threads % 8 == 0 or threads == 2)
        }
    )

    baseline_by_threads = {
        key[1]: summary
        for key, summary in aggregated.items()
        if key[0] == baseline
    }


    legend_handles = []
    for backend in benchmarks:
        points = sorted([(p[1], y) for p, y in aggregated.items() if p[0] == backend and p[1] in baseline_by_threads],
            key=lambda item: item[0])
        if not points:
            continue

        style = SERIES_STYLE.get(backend, None)
        if style is None:
            raise ValueError(f"no series style for {backend}")

        x_values = [threads for threads, _summary in points]
        y_values = []
        y_spreads = []
        for threads, summary in points:
            baseline_summary = baseline_by_threads[threads]
            ratio = summary.center / baseline_summary.center
            spread = ratio * math.sqrt(
                (summary.spread / summary.center) ** 2
                + (baseline_summary.spread / baseline_summary.center) ** 2
            )
            y_values.append(ratio)
            y_spreads.append(spread)

        lower_errors = [
            min(spread, value)
            for value, spread in zip(y_values, y_spreads)
        ]
        axis.errorbar(
            x_values,
            y_values,
            yerr=[lower_errors, y_spreads],
            linestyle=style['linestyle'],
            marker=style['marker'],
            color=style['color'],
            label=DISPLAY_NAMES[backend],
            capsize=2.5,
            elinewidth=0.8,
            capthick=0.8,
        )

    axis.set_xticks([2, 8, 16, 24, 32, 40, 48, 56, 64])
    axis.ticklabel_format(axis='y', useOffset=False, style='plain')
    axis.set_ylabel(r"Latency overhead factor")
    axis.grid(True, axis="y", linestyle=":", linewidth=0.8, color="#8F8F8F")
    axis.set_axisbelow(True)

    # legend_handles, legend_labels = axis.get_legend_handles_labels()
    # axis.legend(handles=legend_handles, labels=legend_labels, fontsize="small", title_fontsize="small")

    return True


def plot_panel(
    aggregated: dict[tuple[str, int], MetricSummary],
    output_path: Path,
    title: str,
    benchmarks: tuple[str, ...],
    baseline: str,
    ylim: None | tuple[int, int]) -> bool:

    figure, axis = plt.subplots(1, 1, figsize=(3, 3 / 1.168))
    plotted = render_panel(
        axis=axis,
        aggregated=aggregated,
        benchmarks=benchmarks,
        baseline=baseline,
    )

    if not plotted:
        plt.close(figure)
        return False

    axis.set_title(title, fontsize=12, fontweight="bold")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    figure.tight_layout(rect=(0.02, 0.04, 1.0, 0.95))
    figure.savefig(output_path, dpi=200)
    plt.close(figure)
    return True


def plot_metrics(
    aggregated: dict[tuple[str, int], MetricSummary],
    base_output_path: Path,
) -> list[Path]:
    written_paths: list[Path] = []
    for panel in PANEL_SPECS:
        output_path = output_path_for_panel(base_output_path, str(panel["name"]))
        wrote_plot = plot_panel(
            aggregated=aggregated,
            output_path=output_path,
            title=str(panel["title"]),
            benchmarks=tuple(panel["benchmarks"]),
            baseline=panel["baseline"],
            ylim=panel.get("ylim", None)
        )
        if wrote_plot:
            written_paths.append(output_path)

    if not written_paths:
        raise ValueError(
            "No vector, map, or RBTree data with power-of-two thread counts was found in the CSV input"
        )


    figure, legend_axis = plt.subplots(1, 1, figsize=(3, 3 / 1.618))
    legend_axis.axis("off")
    legend_axis.legend(
        [plt.Line2D(
            [0],
            [0],
            color=style["color"],
            marker=style["marker"],
            linestyle=style["linestyle"],
            linewidth=style["linewidth"],
            markersize=6,
        ) for style in [VOLATILE_STYLE, FOSSIL_STYLE, PMDK_STYLE]],
        ["Volatile + RW lock", "Fossil", "Intel PMDK + RW lock"],
        loc="center",
        frameon=False,
        fontsize=9,
        handlelength=2.6,
    )

    # figure.tight_layout(rect=(0.02, 0.04, 1.0, 0.95))
    figure.tight_layout()
    figure.savefig(output_path_for_panel(base_output_path, "legend"), dpi=200)
    plt.close(figure)



    return written_paths


def main() -> int:
    args = parse_args()
    rows = load_rows(args.input)
    aggregated = build_aggregated_metrics(rows, args.metric, args.aggregate)
    written_paths = plot_metrics(
        aggregated=aggregated,
        base_output_path=args.output,
    )
    for output_path in written_paths:
        print(f"Wrote plot to {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
