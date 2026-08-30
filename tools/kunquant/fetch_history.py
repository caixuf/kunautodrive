#!/usr/bin/env python3
"""
KunQuant 历史数据抓取器 — 新浪期货连续合约日线 (与 C++ sina_fetcher 同源, 零第三方依赖)

用法:
    python3 tools/kunquant/fetch_history.py [--years 30] [--out data/history]

- 从 kun_quant/config/quant_config.json 读取 symbols[].sina_code (如 nf_RB0 → RB0)
- 拉取新浪 InnerFuturesNewService.getDailyKLine 全量日线 (自合约上市日起)
- 数据只做真实裁剪 (最近 N 年), 不做任何插值/虚构/前复权魔改
- 输出 CSV 契约: symbol,date,open,high,low,close,volume (与 BacktestEngine::load_csv_data 对齐)

注意: 国内期货上市时间有限 (螺纹 2009 / 沪铜 2004 / 白银 2012 / 黄金 2008),
实际覆盖以抓取结果为准, 脚本会如实打印每个品种的真实数据起点, 绝不补假数据凑 30 年。
"""
import argparse
import json
import re
import sys
import urllib.request
from datetime import datetime, timedelta
from pathlib import Path

SINA_KLINE_URL = (
    "https://stock2.finance.sina.com.cn/futures/api/jsonp.php/"
    "var%20_=/InnerFuturesNewService.getDailyKLine?symbol={code}"
)

CONFIG_PATH = Path(__file__).resolve().parents[2] / "kun_quant/config/quant_config.json"


def fetch_daily_kline(sina_code: str) -> list[dict]:
    """拉取新浪连续合约全量日线, 返回 [{d,o,h,l,c,v}, ...] (真实数据, 不做加工)"""
    symbol = sina_code.removeprefix("nf_")
    url = SINA_KLINE_URL.format(code=symbol)
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (KunQuant-HistoryFetcher)"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        text = resp.read().decode("utf-8", errors="replace")
    m = re.search(r"\((\[.*\])\)", text, re.S)
    if not m:
        raise RuntimeError(f"新浪响应格式异常: {sina_code}")
    rows = json.loads(m.group(1))
    if not rows:
        raise RuntimeError(f"新浪返回空数据: {sina_code}")
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description="KunQuant 历史数据抓取 (新浪期货日线)")
    ap.add_argument("--years", type=float, default=30.0, help="保留最近 N 年 (默认 30, 受品种上市时间限制)")
    ap.add_argument("--out", default="data/history", help="CSV 输出目录")
    args = ap.parse_args()

    config = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    symbols = config.get("symbols", [])
    if not symbols:
        print("错误: quant_config.json 中没有 symbols 配置", file=sys.stderr)
        return 1

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    cutoff = datetime.now() - timedelta(days=int(args.years * 365.25))
    rc = 0
    for sym in symbols:
        base_symbol = sym["symbol"]
        family = re.sub(r"\d+$", "", base_symbol)  # rb2405 → rb
        sina_code = sym.get("sina_code", "")
        if not sina_code:
            print(f"跳过 {base_symbol}: 未配置 sina_code", file=sys.stderr)
            continue
        try:
            rows = fetch_daily_kline(sina_code)
        except Exception as e:  # noqa: BLE001
            print(f"失败 {base_symbol} ({sina_code}): {e}", file=sys.stderr)
            rc = 1
            continue

        # 只做真实时间裁剪, 数据本身保持原样
        kept = [r for r in rows if datetime.strptime(r["d"], "%Y-%m-%d") >= cutoff]
        if not kept:
            print(f"失败 {base_symbol}: 裁剪后无数据", file=sys.stderr)
            rc = 1
            continue

        out_file = out_dir / f"{family}.csv"
        with out_file.open("w", encoding="utf-8") as f:
            f.write(f"{family},date,open,high,low,close,volume\n")
            for r in kept:
                f.write(f"{family},{r['d']},{r['o']},{r['h']},{r['l']},{r['c']},{r['v']}\n")

        first, last = kept[0]["d"], kept[-1]["d"]
        years = (datetime.strptime(last, "%Y-%m-%d") - datetime.strptime(first, "%Y-%m-%d")).days / 365.25
        print(f"✓ {family}.csv  {len(kept)} 根日线  {first} ~ {last}  (真实覆盖 {years:.1f} 年, 请求 {args.years} 年)")
    return rc


if __name__ == "__main__":
    sys.exit(main())
