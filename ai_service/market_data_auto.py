#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
market_data_auto.py — 真实行情数据自动抓取守护 (零第三方依赖)

作为 kunquant-ai 服务的后台线程运行:
  - 启动后 5 秒执行首次全量抓取, 之后每 6 小时自动更新
  - 数据源: 新浪期货日线 API (与 C++ sina_fetcher / fetch_history.py 同源)
  - 输出:   data/history/{symbol}.csv (BacktestEngine / /api/trend / /api/bars 共用)

彻底取代"手动调用 python 脚本"的模式 — 服务在, 数据就是新的。
"""
import json
import os
import re
import threading
import time
import urllib.request
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UNIVERSE_PATH = ROOT / "config/universe.json"
OUT_DIR = ROOT / "data/history"
FETCH_INTERVAL_S = 6 * 3600

SINA_KLINE_URL = (
    "https://stock2.finance.sina.com.cn/futures/api/jsonp.php/"
    "var%20_=/InnerFuturesNewService.getDailyKLine?symbol={code}"
)


def fetch_daily_kline(sina_code: str) -> list:
    symbol = sina_code.removeprefix("nf_")
    url = SINA_KLINE_URL.format(code=symbol)
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (KunQuant-AutoFetcher)"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        text = resp.read().decode("utf-8", errors="replace")
    m = re.search(r"\((\[.*\])\)", text, re.S)
    if not m:
        raise RuntimeError("响应格式异常")
    rows = json.loads(m.group(1))
    if not rows:
        raise RuntimeError("空数据")
    return rows


def run_full_fetch() -> dict:
    """全量抓取宇宙内所有合约, 原子写 CSV。返回统计。"""
    universe = json.loads(UNIVERSE_PATH.read_text(encoding="utf-8"))["universe"]
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    ok = fail = 0
    for sym in universe:
        family = sym["symbol"]
        try:
            rows = fetch_daily_kline(sym["sina_code"])
            tmp = OUT_DIR / f".{family}.csv.tmp"
            with tmp.open("w", encoding="utf-8") as f:
                f.write(f"{family},date,open,high,low,close,volume\n")
                for r in rows:
                    f.write(f"{family},{r['d']},{r['o']},{r['h']},{r['l']},{r['c']},{r['v']}\n")
            tmp.rename(OUT_DIR / f"{family}.csv")
            ok += 1
        except Exception as e:  # noqa: BLE001
            fail += 1
            print(f"[MarketDataAuto] {family} 抓取失败: {e}")
        time.sleep(0.2)  # 控制请求频率, 避免触发新浪限流
    print(f"[MarketDataAuto] 全量更新完成: 成功 {ok} 失败 {fail} @ {datetime.now():%Y-%m-%d %H:%M:%S}")
    return {"ok": ok, "fail": fail}


def _loop():
    # 启动后稍等 (避开服务启动高峰), 首次抓取, 之后每 6 小时一轮
    time.sleep(5)
    while True:
        try:
            run_full_fetch()
        except Exception as e:  # noqa: BLE001
            print(f"[MarketDataAuto] 轮次异常: {e}")
        time.sleep(FETCH_INTERVAL_S)


def start_background_fetcher():
    t = threading.Thread(target=_loop, name="market-data-auto", daemon=True)
    t.start()
    print("[MarketDataAuto] 真实行情自动抓取守护已启动 (启动即抓, 每 6 小时全量更新)")
    return t


if __name__ == "__main__":
    run_full_fetch()
