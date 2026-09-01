#!/usr/bin/env python3
"""Quant-specific ten-million-cell feasibility and IS evolution run."""

import os
import torch

from train_cross_sectional_cellular_evolution import (
    CrossSectionalMillionCellBrain,
    load_and_cumulative_adjust,
    evaluate_cross_sectional_brain,
)


def load_history():
    multipliers = {
        "cu": 5, "ru": 10, "rb": 10, "ta": 5, "m": 10, "a": 10,
        "cf": 5, "i": 100, "j": 100, "au": 1000, "ag": 15, "zn": 5,
        "al": 5, "hc": 10, "bu": 10, "MA": 10, "pp": 5, "p": 10,
    }
    tick_sizes = {
        "cu": 10.0, "ru": 5.0, "rb": 1.0, "ta": 2.0, "m": 1.0, "a": 1.0,
        "cf": 5.0, "i": 0.5, "j": 0.5, "au": 0.02, "ag": 1.0, "zn": 5.0,
        "al": 5.0, "hc": 1.0, "bu": 1.0, "MA": 1.0, "pp": 1.0, "p": 2.0,
    }
    data_dir = "data/history" if os.path.exists("data/history") else "../data/history"
    data = {}
    dates = set()
    for symbol, multiplier in multipliers.items():
        path = os.path.join(data_dir, f"{symbol}.csv")
        if not os.path.exists(path):
            continue
        bars = load_and_cumulative_adjust(path)
        if len(bars) < 200:
            continue
        data[symbol] = {
            "bars": bars,
            "map": {bar["date"]: bar for bar in bars},
            "date_to_idx": {bar["date"]: i for i, bar in enumerate(bars)},
            "multiplier": multiplier,
            "tick_size": tick_sizes[symbol],
        }
        dates.update(bar["date"] for bar in bars)
    return data, sorted(dates)


def clone_with_mutation(parent, seed, noise=0.03):
    child = CrossSectionalMillionCellBrain(
        num_assets=parent.num_assets,
        num_cells=parent.num_cells,
        num_synapses=parent.num_synapses,
        device=parent.device,
    )
    with torch.no_grad():
        child.src_idx.copy_(parent.src_idx)
        child.dst_idx.copy_(parent.dst_idx)
        child.alpha_ema.copy_(parent.alpha_ema)
        child.weights.copy_(parent.weights)
        torch.manual_seed(seed)
        child.weights.add_(torch.randn_like(child.weights) * noise)
    return child


def main():
    if not torch.cuda.is_available():
        raise SystemExit("CUDA is required for the ten-million-cell quant run")
    data, timeline = load_history()
    device = "cuda"
    base = CrossSectionalMillionCellBrain(
        num_assets=len(data),
        num_cells=10_000_000,
        num_synapses=20_000_000,
        device=device,
    )
    print("千万细胞量化训练：同一拓扑、IS 适应度选择、突触突变")
    population = [base, clone_with_mutation(base, 11, 0.02)]
    scored = []
    for index, candidate in enumerate(population):
        result = evaluate_cross_sectional_brain(
            candidate, data, timeline, timeline[0], "2016-01-01",
            fee_multiplier=1.5, slippage_multiplier=1.5, top_k=3,
        )
        scored.append((result["fitness"], index, result))
        print(
            f"IS candidate {index}: CAGR={result['cagr']:+.2f}%, "
            f"MaxDD={result['max_drawdown']:.2f}%"
        )
    scored.sort(reverse=True)
    winner = scored[0][2]
    print(
        f"千万细胞 IS 冠军：CAGR={winner['cagr']:+.2f}%, "
        f"MaxDD={winner['max_drawdown']:.2f}%"
    )


if __name__ == "__main__":
    main()
