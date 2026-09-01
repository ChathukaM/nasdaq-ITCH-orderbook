#!/usr/bin/env python3
"""Plot fill probability by queue position from --queue output.

    ./build/itch_replay data/07302019.NASDAQ_ITCH50 --queue AAPL > aapl.txt
    python3 scripts/plot_queue.py aapl.txt spy.txt
"""
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def read(path):
    symbol, rows = path, []
    for line in open(path):
        if line.startswith("symbol:"):
            symbol = line.split()[1]
        elif line.startswith("csv,"):
            _, bucket, joined, filled, rate, wait = line.strip().split(",")
            rows.append((bucket, int(joined), int(filled), float(rate), float(wait)))
    return symbol, rows


def main(paths):
    fig, (ax_rate, ax_wait) = plt.subplots(1, 2, figsize=(12, 4.5))

    for path in paths:
        symbol, rows = read(path)
        if not rows:
            print(f"no csv rows in {path}", file=sys.stderr)
            continue
        buckets = [r[0] for r in rows]
        ax_rate.plot(buckets, [r[3] * 100 for r in rows], marker="o", label=symbol)
        ax_wait.plot(buckets, [r[4] for r in rows], marker="o", label=symbol)

    ax_rate.set(xlabel="queue position on join", ylabel="fill rate (%)",
                title="Fill probability by queue position")
    ax_wait.set(xlabel="queue position on join", ylabel="mean wait to fill (s)",
                title="Time to fill by queue position", yscale="log")
    for ax in (ax_rate, ax_wait):
        ax.grid(alpha=0.3)
        ax.legend()

    fig.tight_layout()
    fig.savefig("queue_position.png", dpi=140)
    print("wrote queue_position.png")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
