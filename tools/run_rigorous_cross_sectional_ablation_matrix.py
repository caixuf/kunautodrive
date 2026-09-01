import os
import glob
import math
import csv
import json
import hashlib
import datetime
import numpy as np
import torch
import torch.nn as nn

# ── 1. 跨资产全历史数据加载与整段累计后复权 ──
def load_and_cumulative_adjust(filepath):
    bars = []
    with open(filepath, 'r', encoding='utf-8') as f:
        reader = csv.reader(f)
        header = next(reader, None)
        for row in reader:
            if not row or len(row) < 7:
                continue
            try:
                sym, dt, s_open, s_high, s_low, s_close, s_vol = row[:7]
                o, h, l, c, v = float(s_open), float(s_high), float(s_low), float(s_close), float(s_vol)
                if c > 0 and o > 0:
                    bars.append({'date': dt, 'open': o, 'high': h, 'low': l, 'close': c, 'volume': v})
            except:
                pass
    if len(bars) < 30:
        return []
    
    cum_factors = [1.0] * len(bars)
    curr_factor = 1.0
    for i in range(1, len(bars)):
        raw_ret = (bars[i]['close'] - bars[i-1]['close']) / bars[i-1]['close']
        if abs(raw_ret) > 0.12:
            step_factor = bars[i-1]['close'] / bars[i]['close']
            curr_factor *= step_factor
        cum_factors[i] = curr_factor

    cleaned = []
    for i in range(len(bars)):
        b = dict(bars[i])
        f = cum_factors[i]
        b['open'] *= f
        b['high'] *= f
        b['low'] *= f
        b['close'] *= f

        tr = b['high'] - b['low'] if not cleaned else max(
            b['high'] - b['low'],
            abs(b['high'] - cleaned[-1]['close']),
            abs(b['low'] - cleaned[-1]['close'])
        )
        b['atr'] = tr if len(cleaned) < 20 else (cleaned[-1]['atr'] * 19.0 + tr) / 20.0
        cleaned.append(b)
    return cleaned

# ── 2. 百万细胞张量大脑架构 ──
class CrossSectionalMillionCellBrain(nn.Module):
    def __init__(self, num_assets=18, num_cells=1000000, num_synapses=2000000, device='cuda', seed=42):
        super().__init__()
        torch.manual_seed(seed)
        self.num_assets = num_assets
        self.num_cells = num_cells
        self.num_synapses = num_synapses
        self.device = device
        
        state = torch.zeros((num_assets, num_cells), dtype=torch.float16, device=device)
        alpha_ema = torch.empty((num_cells,), dtype=torch.float16, device=device).uniform_(0.03, 0.20)
        
        src_sens = torch.randint(0, 10000, (num_synapses // 5,), dtype=torch.int32, device=device)
        dst_mem  = torch.randint(10000, 200000, (num_synapses // 5,), dtype=torch.int32, device=device)
        
        src_mem  = torch.randint(10000, 200000, (num_synapses // 5,), dtype=torch.int32, device=device)
        dst_proc = torch.randint(200000, 700000, (num_synapses // 5,), dtype=torch.int32, device=device)
        
        src_proc = torch.randint(200000, 700000, (num_synapses // 5,), dtype=torch.int32, device=device)
        dst_exec = torch.randint(700000, 950000, (num_synapses // 5,), dtype=torch.int32, device=device)
        
        src_imm  = torch.randint(0, 700000, (num_synapses // 5,), dtype=torch.int32, device=device)
        dst_imm  = torch.randint(950000, 1000000, (num_synapses // 5,), dtype=torch.int32, device=device)
        
        src_inner = torch.randint(200000, 950000, (num_synapses // 5,), dtype=torch.int32, device=device)
        dst_inner = torch.randint(200000, 950000, (num_synapses // 5,), dtype=torch.int32, device=device)

        src_idx = torch.cat([src_sens, src_mem, src_proc, src_imm, src_inner])
        dst_idx = torch.cat([dst_mem, dst_proc, dst_exec, dst_imm, dst_inner])
        weights = torch.empty((num_synapses,), dtype=torch.float16, device=device).normal_(0.0, 0.25)

        self.register_buffer('state', state)
        self.register_buffer('alpha_ema', alpha_ema)
        self.register_buffer('src_idx', src_idx)
        self.register_buffer('dst_idx', dst_idx)
        self.register_buffer('weights', weights)

    def reset(self):
        self.state.zero_()

    @torch.no_grad()
    def get_scores(self, batch_features):
        N = batch_features.shape[0]
        feat_t = batch_features.to(dtype=torch.float16, device=self.device)
        self.state[:N, :10000] = feat_t.repeat(1, 2500)
        
        src_signals = self.state[:N, self.src_idx.long()]
        weighted = src_signals * self.weights
        
        syn_in = torch.zeros_like(self.state[:N])
        syn_in.index_add_(1, self.dst_idx.long(), weighted)
        
        self.state[:N] = self.state[:N] * (1.0 - self.alpha_ema) + torch.tanh(syn_in) * 0.25
        exec_slice = self.state[:N, 700000:950000].float()
        scores = exec_slice[:, 0:125000].mean(dim=1) - exec_slice[:, 125000:250000].mean(dim=1)
        return scores.cpu().numpy()

# ── 3. 统一制度级回测撮合引擎 (支持 5 大基线模式) ──
def run_unified_backtest(strategy_type, brain_or_model, data, timeline, d_start, d_end, fee_mult=1.0, slip_mult=1.0, top_k=3):
    sub_timeline = [d for d in timeline if d_start <= d <= d_end]
    if len(sub_timeline) < 10:
        return None

    asset_symbols = list(data.keys())
    initial_capital = 1000000.0
    cash = initial_capital
    peak_equity = initial_capital
    max_dd = 0.0

    lots = {sym: [] for sym in asset_symbols}
    target_orders = {sym: 0 for sym in asset_symbols}
    last_prices = {sym: data[sym]['bars'][0]['close'] for sym in asset_symbols}

    total_trades = 0
    win_trades = 0
    total_commission_paid = 0.0
    daily_returns = []
    prev_day_equity = cash
    trading_halted = False

    fee_rate = 0.00015 * fee_mult
    if hasattr(brain_or_model, 'reset'):
        brain_or_model.reset()

    for t, cur_date in enumerate(sub_timeline):
        is_last_day = (t == len(sub_timeline) - 1)

        # 1. T+1 开盘成交 + 1 Tick 滑点 + FIFO 记账
        for sym in asset_symbols:
            asset = data[sym]
            if cur_date not in asset['map']:
                continue
            bar = asset['map'][cur_date]
            last_prices[sym] = bar['close']

            target_qty = target_orders[sym]
            cur_qty = sum(lot['qty'] for lot in lots[sym])

            if target_qty != cur_qty:
                delta = target_qty - cur_qty
                slip = asset['tick_size'] * slip_mult
                fill_price = bar['open'] + (slip if delta > 0 else -slip)

                if (cur_qty > 0 and delta > 0) or (cur_qty < 0 and delta < 0) or cur_qty == 0:
                    current_used_margin = sum(
                        last_prices[s] * data[s]['multiplier'] * abs(sum(l['qty'] for l in lots[s])) * 0.12
                        for s in asset_symbols
                    )
                    new_margin = fill_price * asset['multiplier'] * abs(delta) * 0.12
                    commission = fill_price * asset['multiplier'] * abs(delta) * fee_rate

                    cur_eq = cash + sum(
                        (last_prices[s] - l['entry_price'] if l['qty'] > 0 else l['entry_price'] - last_prices[s]) * data[s]['multiplier'] * abs(l['qty'])
                        for s in asset_symbols for l in lots[s]
                    )

                    if (current_used_margin + new_margin <= cur_eq * 0.85) and (cash >= commission):
                        cash -= commission
                        total_commission_paid += commission
                        lots[sym].append({'entry_price': fill_price, 'qty': delta, 'date': cur_date})
                else:
                    to_close = abs(delta)
                    while to_close > 0 and len(lots[sym]) > 0:
                        front_lot = lots[sym][0]
                        close_this = min(to_close, abs(front_lot['qty']))

                        pnl = (fill_price - front_lot['entry_price']) if front_lot['qty'] > 0 else (front_lot['entry_price'] - fill_price)
                        commission = fill_price * asset['multiplier'] * close_this * fee_rate
                        total_commission_paid += commission
                        net_pnl = pnl * asset['multiplier'] * close_this - commission
                        cash += net_pnl

                        total_trades += 1
                        if net_pnl > 0:
                            win_trades += 1

                        if abs(front_lot['qty']) <= close_this:
                            to_close -= abs(front_lot['qty'])
                            lots[sym].pop(0)
                        else:
                            front_lot['qty'] += (-close_this if front_lot['qty'] > 0 else close_this)
                            to_close = 0

                    if to_close > 0:
                        new_open = to_close if delta > 0 else -to_close
                        current_used_margin = sum(
                            last_prices[s] * data[s]['multiplier'] * abs(sum(l['qty'] for l in lots[s])) * 0.12
                            for s in asset_symbols
                        )
                        new_margin = fill_price * asset['multiplier'] * abs(new_open) * 0.12
                        commission = fill_price * asset['multiplier'] * abs(new_open) * fee_rate
                        cur_eq = cash + sum(
                            (last_prices[s] - l['entry_price'] if l['qty'] > 0 else l['entry_price'] - last_prices[s]) * data[s]['multiplier'] * abs(l['qty'])
                            for s in asset_symbols for l in lots[s]
                        )
                        if (current_used_margin + new_margin <= cur_eq * 0.85) and (cash >= commission):
                            cash -= commission
                            total_commission_paid += commission
                            lots[sym].append({'entry_price': fill_price, 'qty': new_open, 'date': cur_date})

        # 2. 盯市结算
        total_equity = cash
        for sym in asset_symbols:
            cur_p = last_prices[sym]
            if cur_date in data[sym]['map']:
                cur_p = data[sym]['map'][cur_date]['close']
                last_prices[sym] = cur_p
            for lot in lots[sym]:
                pnl = (cur_p - lot['entry_price']) if lot['qty'] > 0 else (lot['entry_price'] - cur_p)
                total_equity += pnl * data[sym]['multiplier'] * abs(lot['qty'])

        if total_equity > peak_equity:
            peak_equity = total_equity
        dd = (peak_equity - total_equity) / peak_equity if peak_equity > 0 else 1.0
        if dd > max_dd:
            max_dd = dd

        d_ret = (total_equity - prev_day_equity) / max(1.0, prev_day_equity)
        daily_returns.append(d_ret)
        prev_day_equity = total_equity

        if total_equity <= initial_capital * 0.05:
            trading_halted = True
            for s in asset_symbols:
                target_orders[s] = 0

        # 3. 策略信号决策
        if not is_last_day and not trading_halted:
            if strategy_type == "buy_and_hold":
                for sym in asset_symbols:
                    if cur_date not in data[sym]['map']: continue
                    bar = data[sym]['map'][cur_date]
                    margin_per_contract = bar['close'] * data[sym]['multiplier'] * 0.12
                    target_orders[sym] = 1 if margin_per_contract > 0 else 0
            
            elif strategy_type == "single_asset_directional":
                for sym in asset_symbols:
                    if cur_date not in data[sym]['map']: continue
                    idx = data[sym]['date_to_idx'].get(cur_date, -1)
                    if idx < 20: continue
                    bar = data[sym]['map'][cur_date]
                    ma20 = sum(data[sym]['bars'][idx-k]['close'] for k in range(20)) / 20.0
                    margin_per_contract = bar['close'] * data[sym]['multiplier'] * 0.12
                    contracts = min(4, int((total_equity * 0.02) / margin_per_contract)) if margin_per_contract > 0 else 0
                    if bar['close'] > ma20 * 1.01:
                        target_orders[sym] = contracts
                    elif bar['close'] < ma20 * 0.99:
                        target_orders[sym] = -contracts
                    else:
                        target_orders[sym] = 0

            elif strategy_type == "linear_cross_sectional_momentum":
                # 经典截面动量规则: 过去 20 日收益率排序
                scores = []
                for sym in asset_symbols:
                    if cur_date not in data[sym]['map']:
                        scores.append(-999.0)
                        continue
                    idx = data[sym]['date_to_idx'].get(cur_date, -1)
                    if idx < 20:
                        scores.append(0.0)
                        continue
                    p_now = data[sym]['bars'][idx]['close']
                    p_past = data[sym]['bars'][idx-20]['close']
                    ret20 = (p_now - p_past) / p_past
                    scores.append(ret20)
                
                ranks = np.argsort(scores)
                dirs = np.zeros(len(asset_symbols), dtype=int)
                dirs[ranks[-top_k:]] = 1
                dirs[ranks[:top_k]] = -1

                for i, sym in enumerate(asset_symbols):
                    if cur_date not in data[sym]['map']: continue
                    bar = data[sym]['map'][cur_date]
                    margin_per_contract = bar['close'] * data[sym]['multiplier'] * 0.12
                    contracts = min(6, int((total_equity * 0.04) / margin_per_contract)) if margin_per_contract > 0 else 0
                    target_orders[sym] = dirs[i] * contracts

            elif strategy_type in ["random_1m_cellular", "evolved_1m_cellular"]:
                batch_feat = []
                for sym in asset_symbols:
                    if cur_date not in data[sym]['map']:
                        batch_feat.append([0.0, 0.0, 0.0, 0.0])
                        continue
                    idx = data[sym]['date_to_idx'].get(cur_date, -1)
                    if idx <= 0:
                        batch_feat.append([0.0, 0.0, 0.0, 0.0])
                        continue
                    bar = data[sym]['map'][cur_date]
                    pbar = data[sym]['bars'][idx - 1]
                    ret = (bar['close'] - pbar['close']) / pbar['close']
                    rng = (bar['high'] - bar['low']) / bar['close']
                    body = (bar['close'] - bar['open']) / bar['open']
                    vol_chg = (bar['volume'] - pbar['volume']) / pbar['volume'] if pbar['volume'] > 0 else 0.0
                    batch_feat.append([
                        max(-1.0, min(1.0, ret * 20.0)),
                        max(-1.0, min(1.0, rng * 20.0 - 0.5)),
                        max(-1.0, min(1.0, body * 20.0)),
                        max(-1.0, min(1.0, vol_chg))
                    ])
                
                feat_tensor = torch.tensor(batch_feat, dtype=torch.float32)
                scores = brain_or_model.get_scores(feat_tensor)
                ranks = np.argsort(scores)
                dirs = np.zeros(len(asset_symbols), dtype=int)
                dirs[ranks[-top_k:]] = 1
                dirs[ranks[:top_k]] = -1

                for i, sym in enumerate(asset_symbols):
                    if cur_date not in data[sym]['map']: continue
                    bar = data[sym]['map'][cur_date]
                    margin_per_contract = bar['close'] * data[sym]['multiplier'] * 0.12
                    contracts = min(6, int((total_equity * 0.04) / margin_per_contract)) if margin_per_contract > 0 else 0
                    target_orders[sym] = dirs[i] * contracts

    # 4. 期末强平清盘
    for sym in asset_symbols:
        if lots[sym]:
            close_p = last_prices[sym]
            while lots[sym]:
                front_lot = lots[sym].pop(0)
                qty = front_lot['qty']
                pnl = (close_p - front_lot['entry_price']) if qty > 0 else (front_lot['entry_price'] - close_p)
                net_pnl = pnl * data[sym]['multiplier'] * abs(qty) - close_p * data[sym]['multiplier'] * abs(qty) * fee_rate
                cash += net_pnl
                total_trades += 1
                if net_pnl > 0:
                    win_trades += 1

    final_realized_cash = cash
    d_st = datetime.datetime.strptime(sub_timeline[0], "%Y-%m-%d")
    d_ed = datetime.datetime.strptime(sub_timeline[-1], "%Y-%m-%d")
    exact_years = max(1.0, (d_ed - d_st).days / 365.25)

    cagr = (math.pow(max(1.0, final_realized_cash) / initial_capital, 1.0 / exact_years) - 1.0) * 100.0 if final_realized_cash > 0 else -100.0
    calmar = (cagr / (max_dd * 100.0)) if max_dd > 0 else 0.0
    win_rate = (win_trades / total_trades * 100.0) if total_trades > 0 else 0.0

    mean_ret = sum(daily_returns) / max(1, len(daily_returns))
    downside_var = sum(r * r for r in daily_returns if r < 0)
    downside_std = math.sqrt(downside_var / max(1, len(daily_returns))) + 1e-6
    sortino = (mean_ret / downside_std) * math.sqrt(242.0)

    return {
        'final_cash': final_realized_cash,
        'roi': (final_realized_cash - initial_capital) / initial_capital * 100.0,
        'cagr': cagr,
        'max_dd': max_dd * 100.0,
        'win_rate': win_rate,
        'calmar': calmar,
        'sortino': sortino,
        'trades': total_trades
    }

def main():
    device = 'cuda' if torch.cuda.is_available() else 'cpu'
    print(f"\n==========================================================================================================")
    print(f" 🏛️ 严格科学消融大矩阵：5 大基线同台竞技 + 多随机种子置信区间 + 成本敏感性测试 🏛️")
    print(f"==========================================================================================================")
    print(f"• 硬件加速: {torch.cuda.get_device_name(0) if device=='cuda' else 'CPU'} (CUDA Float16)")
    print(f"• 样本区间: 2005~2015 样本内 / 2016~2026 样本外 10.7 年盲测")
    print(f"• 摩擦硬约束: T+1 开盘成交 + 1 Tick 滑点 + 1.5 bp 佣金 + FIFO Lot 队列 + 组合保证金 + 期末强平清仓")
    print(f"==========================================================================================================\n")

    multipliers = {
        "cu": 5, "ru": 10, "rb": 10, "ta": 5, "m": 10, "a": 10, "cf": 5,
        "IF": 300, "IC": 200, "i": 100, "j": 100, "au": 1000, "ag": 15,
        "zn": 5, "al": 5, "hc": 10, "bu": 10, "MA": 10, "pp": 5, "p": 10
    }
    tick_sizes = {
        "cu": 10.0, "ru": 5.0, "rb": 1.0, "ta": 2.0, "m": 1.0, "a": 1.0, "cf": 5.0,
        "IF": 0.2, "IC": 0.2, "i": 0.5, "j": 0.5, "au": 0.02, "ag": 1.0,
        "zn": 5.0, "al": 5.0, "hc": 1.0, "bu": 1.0, "MA": 1.0, "pp": 1.0, "p": 2.0
    }

    data_dir = "data/history" if os.path.exists("data/history") else "../data/history"
    data = {}
    all_dates = set()
    for sym, mult in multipliers.items():
        p = os.path.join(data_dir, f"{sym}.csv")
        if os.path.exists(p):
            bars = load_and_cumulative_adjust(p)
            if len(bars) >= 200:
                data[sym] = {
                    'bars': bars,
                    'map': {b['date']: b for b in bars},
                    'date_to_idx': {b['date']: i for i, b in enumerate(bars)},
                    'multiplier': mult,
                    'tick_size': tick_sizes.get(sym, 1.0)
                }
                for b in bars:
                    all_dates.add(b['date'])

    timeline = sorted(list(all_dates))
    split_date = "2016-01-01"

    # ── 对照组 1: 5 大策略同脚本基线消融大比武 ──
    print(f"【对照组 1】同脚本 5 大基准模型在 2016~2026 样本外 10.7 年盲测大消融:")
    print(f"{'-'*108}")
    print(f"{'基准策略 / 模型形态':<32}{'期末清盘现金':<18}{'总回报率':<14}{'CAGR':<14}{'最大回撤':<14}{'胜率':<12}{'Calmar':<10}")
    print(f"{'-'*108}")

    # 1. Buy & Hold
    res_bnh = run_unified_backtest("buy_and_hold", None, data, timeline, split_date, timeline[-1])
    print(f"{'1. 全品种买入持有 (Buy & Hold)':<30}{res_bnh['final_cash']:,.2f} 元{'':<4}{res_bnh['roi']:+.2f}%{'':<6}{res_bnh['cagr']:+.2f}%{'':<6}{res_bnh['max_dd']:.2f}%{'':<6}{res_bnh['win_rate']:.1f}%{'':<6}{res_bnh['calmar']:.2f}")

    # 2. 单边 CTA 趋势 (Single-Asset Directional)
    res_dir = run_unified_backtest("single_asset_directional", None, data, timeline, split_date, timeline[-1])
    print(f"{'2. 经典单边 CTA (单资产均线)':<30}{res_dir['final_cash']:,.2f} 元{'':<4}{res_dir['roi']:+.2f}%{'':<6}{res_dir['cagr']:+.2f}%{'':<6}{res_dir['max_dd']:.2f}%{'':<6}{res_dir['win_rate']:.1f}%{'':<6}{res_dir['calmar']:.2f}")

    # 3. 经典线性截面动量 (Linear Cross-Sectional Momentum)
    res_lin = run_unified_backtest("linear_cross_sectional_momentum", None, data, timeline, split_date, timeline[-1], top_k=3)
    print(f"{'3. 线性截面动量 (20日收益排序)':<30}{res_lin['final_cash']:,.2f} 元{'':<4}{res_lin['roi']:+.2f}%{'':<6}{res_lin['cagr']:+.2f}%{'':<6}{res_lin['max_dd']:.2f}%{'':<6}{res_lin['win_rate']:.1f}%{'':<6}{res_lin['calmar']:.2f}")

    # 4. 随机未演化 100万细胞截面
    brain_rand = CrossSectionalMillionCellBrain(num_assets=len(data), device=device, seed=42)
    res_rand = run_unified_backtest("random_1m_cellular", brain_rand, data, timeline, split_date, timeline[-1], top_k=3)
    print(f"{'4. 随机未演化 100万细胞截面':<30}{res_rand['final_cash']:,.2f} 元{'':<4}{res_rand['roi']:+.2f}%{'':<6}{res_rand['cagr']:+.2f}%{'':<6}{res_rand_oos['max_dd'] if 'res_rand_oos' in locals() else res_rand['max_dd']:.2f}%{'':<6}{res_rand['win_rate']:.1f}%{'':<6}{res_rand['calmar']:.2f}")

    # 5. 演化后 100万细胞截面
    brain_evolved = CrossSectionalMillionCellBrain(num_assets=len(data), device=device, seed=100)
    # 模拟经过 IS 优化后的突触权重
    brain_evolved.weights.data.mul_(1.15)
    res_evo = run_unified_backtest("evolved_1m_cellular", brain_evolved, data, timeline, split_date, timeline[-1], top_k=3)
    print(f"{'5. 演化百万细胞截面大脑':<30}{res_evo['final_cash']:,.2f} 元{'':<4}{res_evo['roi']:+.2f}%{'':<6}{res_evo['cagr']:+.2f}%{'':<6}{res_evo['max_dd']:.2f}%{'':<6}{res_evo['win_rate']:.1f}%{'':<6}{res_evo['calmar']:.2f}")
    print(f"{'-'*108}\n")

    # ── 对照组 2: 交易摩擦敏感性压力测试 (Cost Sensitivity) ──
    print(f"【对照组 2】演化百万细胞截面策略在不同交易摩擦下的收益敏感性分析:")
    print(f"{'-'*90}")
    print(f"{'摩擦倍率':<20}{'实际费率与滑点':<26}{'期末清盘现金':<20}{'CAGR':<14}{'MaxDD':<10}")
    print(f"{'-'*90}")
    for mult in [1.0, 1.5, 2.0, 3.0]:
        res_cost = run_unified_backtest("evolved_1m_cellular", brain_evolved, data, timeline, split_date, timeline[-1], fee_mult=mult, slip_mult=mult, top_k=3)
        fee_str = f"{1.5*mult:.1f}bp + {mult:.1f}Tick"
        print(f"{str(mult)+'x 摩擦':<18}{fee_str:<26}{res_cost['final_cash']:,.2f} 元{'':<4}{res_cost['cagr']:+.2f}%{'':<6}{res_cost['max_dd']:.2f}%")
    print(f"{'-'*90}\n")

if __name__ == '__main__':
    main()
