#!/usr/bin/env python3
"""
N7 Low-Latency Order Book Benchmark & Visualizer
Generates publication-quality charts using matplotlib and seaborn from data/latency_metrics.csv.
"""

import os
import sys

# Configure matplotlib cache directory
os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib_cache")
os.makedirs("/tmp/matplotlib_cache", exist_ok=True)

# Set matplotlib to non-interactive backend
import matplotlib
import numpy as np
import pandas as pd

matplotlib.use("Agg")
import matplotlib.pyplot as plt

# Global styling configuration
plt.rcParams["figure.facecolor"] = "#0f172a"
plt.rcParams["axes.facecolor"] = "#1e293b"
plt.rcParams["axes.edgecolor"] = "#334155"
plt.rcParams["axes.labelcolor"] = "#94a3b8"
plt.rcParams["text.color"] = "#f8fafc"
plt.rcParams["xtick.color"] = "#94a3b8"
plt.rcParams["ytick.color"] = "#94a3b8"
plt.rcParams["grid.color"] = "#334155"
plt.rcParams["grid.linestyle"] = "--"
plt.rcParams["grid.alpha"] = 0.5
plt.rcParams["font.sans-serif"] = ["DejaVu Sans", "Arial", "Helvetica", "sans-serif"]


def load_data(csv_path: str = "data/latency_metrics.csv"):
    if not os.path.exists(csv_path):
        print(f"Error: {csv_path} not found. Please run ./build/n7_order_book first.")
        sys.exit(1)
    df = pd.read_csv(csv_path)
    return df


def plot_percentile_comparison(df: pd.DataFrame, output_dir: str):
    """Grouped bar chart for P50, P90, P95, P99, P99.9 across scenarios."""
    # Filter target actions
    plot_df = df[
        df["scenario"].str.startswith("Mixed:")
        | (df["scenario"] == "Bulk Limit Insertions")
    ].copy()
    plot_df["short_name"] = (
        plot_df["scenario"].str.replace("Mixed: ", "").str.replace("Workload", "")
    )

    percentiles = ["p50_ns", "p90_ns", "p95_ns", "p99_ns", "p99_9_ns"]
    labels = ["P50 (Median)", "P90", "P95", "P99", "P99.9"]
    palette = ["#38bdf8", "#818cf8", "#c084fc", "#f472b6", "#fb7185"]

    fig, ax = plt.subplots(figsize=(12, 6.5), dpi=300)

    n_groups = len(plot_df)
    n_bars = len(percentiles)
    bar_width = 0.15
    indices = np.arange(n_groups)

    for i, (pct, label, color) in enumerate(zip(percentiles, labels, palette)):
        offset = (i - n_bars / 2 + 0.5) * bar_width
        bars = ax.bar(
            indices + offset,
            plot_df[pct],
            bar_width,
            label=label,
            color=color,
            alpha=0.9,
            edgecolor="none",
            zorder=3,
        )

        # Annotate values for P50 and P99
        if i in (0, 3):
            for bar in bars:
                height = bar.get_height()
                val_text = (
                    f"{int(height)}" if height < 1000 else f"{height/1000.0:.1f}k"
                )
                ax.annotate(
                    val_text,
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 4),
                    textcoords="offset points",
                    ha="center",
                    va="bottom",
                    fontsize=8,
                    fontweight="bold",
                    color="#e2e8f0",
                )

    ax.set_title(
        "N7 Order Book: Latency Distribution by Percentile",
        fontsize=16,
        fontweight="bold",
        pad=15,
        color="#f8fafc",
    )
    ax.set_ylabel("Latency (Nanoseconds)", fontsize=12, labelpad=10)
    ax.set_xticks(indices)
    ax.set_xticklabels(plot_df["short_name"], fontsize=11, fontweight="600")
    ax.grid(True, axis="y", zorder=0)
    ax.legend(
        title="Percentiles",
        loc="upper left",
        framealpha=0.4,
        facecolor="#0f172a",
        edgecolor="#334155",
        fontsize=10,
    )

    plt.tight_layout()
    png_path = os.path.join(output_dir, "latency_percentiles.png")
    svg_path = os.path.join(output_dir, "latency_percentiles.svg")
    plt.savefig(png_path)
    plt.savefig(svg_path)
    plt.close()
    print(f"--> Saved: {png_path} and {svg_path}")


def plot_throughput_comparison(df: pd.DataFrame, output_dir: str):
    """Horizontal bar chart comparing operations throughput across benchmarks."""
    fig, ax = plt.subplots(figsize=(11, 5.5), dpi=300)

    plot_df = df.copy()
    plot_df = plot_df.sort_values(by="throughput_ops_sec", ascending=True)

    colors = [
        "#0ea5e9",
        "#10b981",
        "#6366f1",
        "#a855f7",
        "#ec4899",
        "#f59e0b",
        "#14b8a6",
    ][: len(plot_df)]

    bars = ax.barh(
        plot_df["scenario"],
        plot_df["throughput_ops_sec"] / 1e6,
        color=colors,
        alpha=0.9,
        height=0.6,
        zorder=3,
    )

    for bar in bars:
        width = bar.get_width()
        val_str = (
            f"{width:.2f}M ops/sec" if width >= 1.0 else f"{width * 1000:.0f}k ops/sec"
        )
        ax.annotate(
            val_str,
            xy=(width, bar.get_y() + bar.get_height() / 2),
            xytext=(8, 0),
            textcoords="offset points",
            ha="left",
            va="center",
            fontsize=10,
            fontweight="bold",
            color="#38bdf8",
        )

    ax.set_title(
        "N7 Order Book: Engine Throughput Comparison",
        fontsize=16,
        fontweight="bold",
        pad=15,
        color="#f8fafc",
    )
    ax.set_xlabel("Throughput (Million Operations / Second)", fontsize=12, labelpad=10)
    ax.grid(True, axis="x", zorder=0)
    ax.set_xlim(0, max(plot_df["throughput_ops_sec"] / 1e6) * 1.22)

    plt.tight_layout()
    png_path = os.path.join(output_dir, "throughput_comparison.png")
    svg_path = os.path.join(output_dir, "throughput_comparison.svg")
    plt.savefig(png_path)
    plt.savefig(svg_path)
    plt.close()
    print(f"--> Saved: {png_path} and {svg_path}")


def plot_percentile_curves(df: pd.DataFrame, output_dir: str):
    """Line graph / CDF curve showing latency progression from P50 to P99.99."""
    fig, ax = plt.subplots(figsize=(11, 6), dpi=300)

    mixed_df = df[df["scenario"].str.startswith("Mixed:")].copy()
    pct_cols = [
        "p50_ns",
        "p75_ns",
        "p90_ns",
        "p95_ns",
        "p99_ns",
        "p99_9_ns",
        "p99_99_ns",
    ]
    x_labels = ["P50", "P75", "P90", "P95", "P99", "P99.9", "P99.99"]

    palette = {
        "Mixed: Limit Add": "#38bdf8",
        "Mixed: Cancel": "#34d399",
        "Mixed: Market Order": "#fbbf24",
        "Mixed: Modify": "#f87171",
        "Mixed: Overall Workload": "#c084fc",
    }

    for _, row in mixed_df.iterrows():
        name = row["scenario"]
        clean_name = name.replace("Mixed: ", "")
        y_vals = [row[col] for col in pct_cols]
        color = palette.get(name, "#94a3b8")
        linewidth = 2.8 if "Overall" in name else 1.8
        linestyle = "-" if "Overall" in name else "--"

        ax.plot(
            x_labels,
            y_vals,
            marker="o",
            markersize=6,
            label=clean_name,
            color=color,
            linewidth=linewidth,
            linestyle=linestyle,
            zorder=4,
        )

    ax.set_yscale("log")
    ax.set_title(
        "N7 Order Book: Latency Tail Curves (Log Scale)",
        fontsize=16,
        fontweight="bold",
        pad=15,
        color="#f8fafc",
    )
    ax.set_ylabel("Latency (Nanoseconds - Log Scale)", fontsize=12, labelpad=10)
    ax.set_xlabel("Percentile Rank", fontsize=12, labelpad=10)
    ax.grid(True, which="both", zorder=0)
    ax.legend(
        title="Action Type",
        loc="upper left",
        framealpha=0.4,
        facecolor="#0f172a",
        edgecolor="#334155",
        fontsize=10,
    )

    plt.tight_layout()
    png_path = os.path.join(output_dir, "latency_tail_curves.png")
    svg_path = os.path.join(output_dir, "latency_tail_curves.svg")
    plt.savefig(png_path)
    plt.savefig(svg_path)
    plt.close()
    print(f"--> Saved: {png_path} and {svg_path}")


def main():
    csv_file = "data/latency_metrics.csv"
    output_dir = "charts"
    os.makedirs(output_dir, exist_ok=True)

    df = load_data(csv_file)

    plot_percentile_comparison(df, output_dir)
    plot_throughput_comparison(df, output_dir)
    plot_percentile_curves(df, output_dir)

    print(f"\nAll charts generated successfully in ./{output_dir}/")


if __name__ == "__main__":
    main()
