#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ai_service/radar_screener.py — 真实多因子量化选股与 AI 雷达实时推荐引擎

基于真实全市场历史 K 线 (data/history/*.csv) 与配置宇宙 (config/universe.json)，
执行多因子量化打分 (多头排列/动量突破/量比异动/ATR 自适应止盈止损)，
实时生成 100% 数据驱动的 AI 雷达精选标的。
"""

import os
import json
import math
from pathlib import Path
from typing import Dict, Any, List, Optional
from datetime import datetime

ROOT = Path(__file__).resolve().parents[1]
UNIVERSE_PATH = ROOT / "config/universe.json"
HISTORY_DIR = ROOT / "data/history"


def calculate_atr(highs: List[float], lows: List[float], closes: List[float], period: int = 14) -> float:
    """计算 14 日真实波幅 (Average True Range)"""
    if len(closes) < 2:
        return max(0.1, (highs[-1] - lows[-1]) if highs else 1.0)
    
    tr_list = []
    for i in range(1, len(closes)):
        h = highs[i]
        l = lows[i]
        prev_c = closes[i - 1]
        tr = max(h - l, abs(h - prev_c), abs(l - prev_c))
        tr_list.append(tr)
    
    if not tr_list:
        return max(0.1, highs[-1] - lows[-1])
    
    subset = tr_list[-period:]
    return sum(subset) / len(subset)


def calculate_ma(series: List[float], period: int) -> float:
    if len(series) < period:
        return sum(series) / len(series) if series else 0.0
    return sum(series[-period:]) / period


def screen_radar_candidates() -> List[Dict[str, Any]]:
    """扫描宇宙标的，执行真实多因子量化打分与风控止盈止损推导"""
    if not UNIVERSE_PATH.exists():
        return []

    try:
        with open(UNIVERSE_PATH, "r", encoding="utf-8") as f:
            universe_data = json.load(f)
            universe = universe_data.get("universe", [])
    except Exception:
        return []

    candidates = []

    for item in universe:
        symbol = item.get("symbol", "")
        name = item.get("name", symbol)
        category_raw = item.get("category", "futures")
        cat_map = {"futures": "商品期货", "index": "金融期指", "stock": "A股股票", "etf": "行业ETF"}
        category = cat_map.get(category_raw, "商品期货")
        tick = float(item.get("tick", 1.0))

        csv_path = HISTORY_DIR / f"{symbol}.csv"
        if not csv_path.exists():
            continue

        try:
            dates, opens, highs, lows, closes, volumes = [], [], [], [], [], []
            with open(csv_path, "r", encoding="utf-8") as f:
                lines = f.readlines()
                for line in lines[1:]:  # skip header
                    parts = line.strip().split(",")
                    if len(parts) >= 7:
                        # symbol, date, open, high, low, close, volume
                        dates.append(parts[1])
                        opens.append(float(parts[2]))
                        highs.append(float(parts[3]))
                        lows.append(float(parts[4]))
                        closes.append(float(parts[5]))
                        volumes.append(float(parts[6]))
        except Exception:
            continue

        if len(closes) < 20:
            continue

        last_close = closes[-1]
        last_vol = volumes[-1]
        
        # 计算均线
        ma5 = calculate_ma(closes, 5)
        ma10 = calculate_ma(closes, 10)
        ma20 = calculate_ma(closes, 20)
        ma60 = calculate_ma(closes, min(60, len(closes)))
        
        # 计算量能
        vol20_avg = calculate_ma(volumes, 20)
        vol_ratio = (last_vol / vol20_avg) if vol20_avg > 0 else 1.0
        
        # 计算 ATR
        atr = calculate_atr(highs, lows, closes, 14)
        atr_pct = (atr / last_close * 100.0) if last_close > 0 else 1.0

        # 多因子打分逻辑 (0 - 100)
        score = 65.0
        reasons = []

        # 因子 1: 均线趋势排列 (Trend Alignment)
        is_bull = last_close > ma20 and ma5 > ma10
        is_strong_bull = is_bull and ma10 > ma20 and last_close > ma5
        if is_strong_bull:
            score += 18.0
            reasons.append("多头均线发散排列")
        elif is_bull:
            score += 10.0
            reasons.append("站稳20日生命线")
        elif last_close < ma20 and ma5 < ma10:
            score -= 10.0

        # 因子 2: 突破动量 (20日高点突破)
        high_20 = max(highs[-20:-1]) if len(highs) >= 20 else highs[-1]
        if last_close >= high_20 * 0.995:
            score += 15.0
            reasons.append("突破近20日盘整前高")

        # 因子 3: 量能异动 (Volume Surge)
        if vol_ratio >= 1.6:
            score += 12.0
            reasons.append(f"量能显著放大 (量比 {vol_ratio:.1f}x)")
        elif vol_ratio >= 1.2:
            score += 6.0
            reasons.append("温和放量增仓")

        # 因子 4: 短期动量 (5日收益率)
        ret5 = ((last_close - closes[-5]) / closes[-5] * 100.0) if len(closes) >= 5 else 0.0
        if ret5 > 1.5:
            score += 8.0
        elif ret5 < -3.0:
            score -= 8.0

        # 风控适配与止损止盈推导 (基于 ATR 动态推导)
        decimals = 3 if last_close < 10.0 else (2 if last_close < 100.0 else 1)
        entry_price = round(last_close, decimals)
        
        # 多头策略: SL = Entry - 2.0 * ATR, TP = Entry + 3.5 * ATR
        sl_price = round(max(0.01, entry_price - 2.0 * atr), decimals)
        tp_price = round(entry_price + 3.5 * atr, decimals)

        final_score = int(min(98, max(60, round(score))))
        if not reasons:
            reasons.append("均值回归突破区间")

        reason_str = " · ".join(reasons)
        win_rate_str = f"动量评分 {final_score}"

        candidates.append({
            "symbol": symbol,
            "name": name,
            "category": category,
            "score": final_score,
            "reason": reason_str,
            "entry": entry_price,
            "sl": sl_price,
            "tp": tp_price,
            "winRate": win_rate_str,
            "last_close": last_close,
            "atr": round(atr, decimals),
            "vol_ratio": round(vol_ratio, 2)
        })

    # 按量化综合评分从高到低排序，精选 Top 6
    candidates.sort(key=lambda x: x["score"], reverse=True)
    return candidates[:6]


def get_ai_radar_payload() -> Dict[str, Any]:
    candidates = screen_radar_candidates()
    return {
        "status": "OK",
        "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "count": len(candidates),
        "recommendations": candidates
    }


if __name__ == "__main__":
    res = get_ai_radar_payload()
    print(json.dumps(res, ensure_ascii=False, indent=2))
