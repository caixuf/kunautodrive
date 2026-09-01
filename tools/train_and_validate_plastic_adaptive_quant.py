import os
import glob
import math
import csv
import json
import time
import datetime
import numpy as np
import torch
import torch.nn as nn

SEED = 42
torch.manual_seed(SEED)
if torch.cuda.is_available():
    torch.cuda.manual_seed_all(SEED)

class PlasticAdaptiveMillionCellBrain(nn.Module):
    """
    以变化对抗变化：具备「在线突触慢速塑性」与「多生境专家分工」的百万细胞生命体
    - 细胞规模: 1,000,000 (1M) Float16 动力学细胞
    - 突触规模: 2,000,000 (2M) 稀疏动力学突触
    - 架构图谱分划:
      1. 感知受体 (0 ~ 10,000, 1%)
      2. 状态多尺度记忆 (10,000 ~ 200,000, 19%)
      3. 趋势生境器官 Trend Habitat (200,000 ~ 450,000, 25%) - 适应单边大牛大熊
      4. 震荡生境器官 Chop Habitat (450,000 ~ 700,000, 25%) - 适应均值回归与区间洗盘
      5. 执行决策中枢 Execution Effector (700,000 ~ 950,000, 25%)
      6. 绝对不变性风控免疫 Invariant Immune (950,000 ~ 1,000,000, 5%) - 严格受控禁篡改
    - 慢速在线突触塑性 (Bounded Oja-Hebbian Plasticity):
      在实盘/样本外时空长河中，仅对认知关联突触允许慢速塑性自适应，风控免疫突触绝对冻结！
    """
    def __init__(self, num_assets=18, num_cells=1000000, num_synapses=2000000, device='cuda', seed=42):
        super().__init__()
        torch.manual_seed(seed)
        self.num_assets = num_assets
        self.num_cells = num_cells
        self.device = device
        
        state = torch.zeros((num_assets, num_cells), dtype=torch.float16, device=device)
        alpha_ema = torch.empty((num_cells,), dtype=torch.float16, device=device).uniform_(0.03, 0.20)
        
        # 突触连接分布
        syn_chunk = num_synapses // 6
        # 1. 感知 -> 记忆
        src_sens = torch.randint(0, 10000, (syn_chunk,), dtype=torch.int32, device=device)
        dst_mem  = torch.randint(10000, 200000, (syn_chunk,), dtype=torch.int32, device=device)
        
        # 2. 记忆 -> 趋势生境
        src_mem_trend = torch.randint(10000, 200000, (syn_chunk,), dtype=torch.int32, device=device)
        dst_trend     = torch.randint(200000, 450000, (syn_chunk,), dtype=torch.int32, device=device)
        
        # 3. 记忆 -> 震荡生境
        src_mem_chop  = torch.randint(10000, 200000, (syn_chunk,), dtype=torch.int32, device=device)
        dst_chop      = torch.randint(450000, 700000, (syn_chunk,), dtype=torch.int32, device=device)
        
        # 4. 双生境 -> 执行中枢
        src_exec = torch.cat([
            torch.randint(200000, 450000, (syn_chunk // 2,), dtype=torch.int32, device=device),
            torch.randint(450000, 700000, (syn_chunk - syn_chunk // 2,), dtype=torch.int32, device=device)
        ])
        dst_exec = torch.randint(700000, 950000, (syn_chunk,), dtype=torch.int32, device=device)
        
        # 5. 感知/处理 -> 绝对风控免疫
        src_imm = torch.randint(0, 700000, (syn_chunk,), dtype=torch.int32, device=device)
        dst_imm = torch.randint(950000, 1000000, (syn_chunk,), dtype=torch.int32, device=device)
        
        # 6. 生境内部自演化循环
        src_inner = torch.randint(200000, 950000, (syn_chunk,), dtype=torch.int32, device=device)
        dst_inner = torch.randint(200000, 950000, (syn_chunk,), dtype=torch.int32, device=device)

        src_idx = torch.cat([src_sens, src_mem_trend, src_mem_chop, src_exec, src_imm, src_inner])
        dst_idx = torch.cat([dst_mem, dst_trend, dst_chop, dst_exec, dst_imm, dst_inner])
        
        actual_synapses = src_idx.shape[0]
        self.num_synapses = actual_synapses
        weights = torch.empty((actual_synapses,), dtype=torch.float16, device=device).normal_(0.0, 0.25)

        # 标记哪些突触允许可塑性 (风控免疫突触 dst >= 950000 严格禁止可塑性)
        plastic_mask = (dst_idx < 950000)

        self.register_buffer('state', state)
        self.register_buffer('alpha_ema', alpha_ema)
        self.register_buffer('src_idx', src_idx)
        self.register_buffer('dst_idx', dst_idx)
        self.register_buffer('weights', weights)
        self.register_buffer('plastic_mask', plastic_mask)

    def reset(self):
        self.state.zero_()

    @torch.no_grad()
    def forward_adaptive(self, batch_features, enable_online_plasticity=True, plasticity_lr=0.0002):
        N = batch_features.shape[0]
        feat_t = batch_features.to(dtype=torch.float16, device=self.device)
        
        # 1. 注入感知受体
        self.state[:N, :10000] = feat_t.repeat(1, 2500)
        
        # 2. 突触信号聚合
        src_signals = self.state[:N, self.src_idx.long()]
        weighted = src_signals * self.weights
        
        syn_in = torch.zeros_like(self.state[:N])
        syn_in.index_add_(1, self.dst_idx.long(), weighted)
        
        # 3. 细胞膜电位更新与非线性激活
        self.state[:N] = self.state[:N] * (1.0 - self.alpha_ema) + torch.tanh(syn_in) * 0.25
        
        # 4. 在线慢速赫布/Oja突触塑性 (以变化对抗变化)
        if enable_online_plasticity and plasticity_lr > 0:
            dst_signals = self.state[:N, self.dst_idx.long()]
            # 跨 18 品种的均值协方差驱动
            hebbian_delta = (src_signals * dst_signals).mean(dim=0)
            oja_decay = (dst_signals.pow(2) * self.weights).mean(dim=0) * 0.5
            delta_w = (hebbian_delta - oja_decay) * plasticity_lr
            
            # 仅更新非免疫突触并严格限制在 [-0.8, +0.8]
            self.weights[self.plastic_mask] += delta_w[self.plastic_mask]
            self.weights.clamp_(-0.8, 0.8)
        
        # 5. 提取执行中枢评分 (700k ~ 950k)
        exec_slice = self.state[:N, 700000:950000].float()
        scores = exec_slice[:, 0:125000].mean(dim=1) - exec_slice[:, 125000:250000].mean(dim=1)
        
        # 6. 风控免疫信号
        immune_slice = self.state[:N, 950000:1000000].float()
        immune_signal = (immune_slice.abs().mean(dim=1) > 1.35)
        
        return scores.cpu().numpy(), immune_signal.cpu().numpy()

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

def evaluate_adaptive_brain(brain, data, timeline, d_start, d_end, fee_mult=1.0, slip_mult=1.0, top_k=3, enable_plasticity=True, lr=0.0002):
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
    brain.reset()

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

        # 3. 前向推演 + 在线慢速突触塑性
        if not is_last_day and not trading_halted:
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
            scores, immunes = brain.forward_adaptive(feat_tensor, enable_online_plasticity=enable_plasticity, plasticity_lr=lr)
            
            ranks = np.argsort(scores)
            dirs = np.zeros(len(asset_symbols), dtype=int)
            dirs[ranks[-top_k:]] = 1
            dirs[ranks[:top_k]] = -1
            dirs[immunes] = 0

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
    cost_ratio = total_commission_paid / initial_capital

    multi_objective_fitness = (2.0 * sortino) + (1.5 * calmar) - (max_dd * 10.0) - (cost_ratio * 0.5)

    return {
        'final_cash': final_realized_cash,
        'roi': (final_realized_cash - initial_capital) / initial_capital * 100.0,
        'cagr': cagr,
        'max_dd': max_dd * 100.0,
        'win_rate': win_rate,
        'calmar': calmar,
        'sortino': sortino,
        'trades': total_trades,
        'fitness': multi_objective_fitness
    }

def main():
    device = 'cuda' if torch.cuda.is_available() else 'cpu'
    print(f"\n==========================================================================================================", flush=True)
    print(f" 🧬 鲲 1,000,000 细胞「以变化对抗变化」动态自适应生命体：在线突触塑性与多生境盲测大考 🧬", flush=True)
    print(f"==========================================================================================================", flush=True)
    print(f"• 硬件加速: {torch.cuda.get_device_name(0) if device=='cuda' else 'CPU'} (CUDA Float16)", flush=True)
    print(f"• 架构范式: 六器官多生境 (趋势生境 25% + 震荡生境 25% + 记忆 19% + 执行 25% + 绝对免疫 5%)", flush=True)
    print(f"• 终身可塑性: 在 10.7 年盲测中引入慢速 Oja-Hebbian 在线突触塑性自适应 (风控免疫突触严禁篡改)", flush=True)
    print(f"• 制度约束: T+1 开盘成交 + 1 Tick 滑点 + 1.5 bp 佣金 + FIFO Lot 队列 + 组合保证金 + 期末强平清仓", flush=True)
    print(f"==========================================================================================================\n", flush=True)

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
    num_assets = len(data)

    print(f"[Step 1] 构建 1,000,000 细胞双生境塑性大脑模型...", flush=True)
    brain_static = PlasticAdaptiveMillionCellBrain(num_assets=num_assets, num_cells=1000000, num_synapses=2000000, device=device, seed=42)
    brain_plastic = PlasticAdaptiveMillionCellBrain(num_assets=num_assets, num_cells=1000000, num_synapses=2000000, device=device, seed=42)

    # 1. 样本内基线
    print(f"[Step 2] 评估样本内 (2005~2015) 基准表现...", flush=True)
    res_is = evaluate_adaptive_brain(brain_static, data, timeline, timeline[0], split_date, fee_mult=1.5, slip_mult=1.5, top_k=3, enable_plasticity=False)
    print(f"  ↳ 样本内适应度 = {res_is['fitness']:+.2f} (IS CAGR: {res_is['cagr']:+.2f}%, MaxDD: {res_is['max_dd']:.1f}%, 胜率: {res_is['win_rate']:.1f}%)", flush=True)

    # 2. 样本外 10.7 年消融：静态冻结大脑 vs 在线塑性自适应生命体
    print(f"\n==========================================================================================================", flush=True)
    print(f"  📊 样本外 (2016 ~ 2026, 10.7 年)「静态冻结大脑」vs「在线塑性生命体」同台终极大消融 📊", flush=True)
    print(f"==========================================================================================================", flush=True)
    print(f"{'模型机制 / 生命体形态':<32}{'期末清盘现金':<18}{'总回报率':<14}{'CAGR':<14}{'最大回撤':<14}{'胜率':<12}{'Calmar':<10}", flush=True)
    print(f"{'-'*112}", flush=True)

    # 模式 A: 静态冻结权重 (无在线塑性)
    res_static_oos = evaluate_adaptive_brain(brain_static, data, timeline, split_date, timeline[-1], fee_mult=1.0, slip_mult=1.0, top_k=3, enable_plasticity=False)
    print(f"{'1. 静态冻结大脑 (无自适应)':<30}{res_static_oos['final_cash']:,.2f} 元{'':<4}{res_static_oos['roi']:+.2f}%{'':<6}{res_static_oos['cagr']:+.2f}%{'':<6}{res_static_oos['max_dd']:.2f}%{'':<6}{res_static_oos['win_rate']:.1f}%{'':<6}{res_static_oos['calmar']:.2f}", flush=True)

    # 模式 B: 慢速在线突触塑性 (以变化对抗变化, 学习率 1e-4)
    res_plastic_oos = evaluate_adaptive_brain(brain_plastic, data, timeline, split_date, timeline[-1], fee_mult=1.0, slip_mult=1.0, top_k=3, enable_plasticity=True, lr=0.0001)
    print(f"{'2. 🔥 慢速在线塑性生命体 (η=1e-4)':<27}{res_plastic_oos['final_cash']:,.2f} 元{'':<4}{res_plastic_oos['roi']:+.2f}%{'':<6}{res_plastic_oos['cagr']:+.2f}%{'':<6}{res_plastic_oos['max_dd']:.2f}%{'':<6}{res_plastic_oos['win_rate']:.1f}%{'':<6}{res_plastic_oos['calmar']:.2f}", flush=True)

    # 模式 C: 敏捷在线突触塑性 (学习率 5e-4)
    brain_plastic_fast = PlasticAdaptiveMillionCellBrain(num_assets=num_assets, num_cells=1000000, num_synapses=2000000, device=device, seed=42)
    res_plastic_fast_oos = evaluate_adaptive_brain(brain_plastic_fast, data, timeline, split_date, timeline[-1], fee_mult=1.0, slip_mult=1.0, top_k=3, enable_plasticity=True, lr=0.0005)
    print(f"{'3. 🚀 敏捷在线塑性生命体 (η=5e-4)':<27}{res_plastic_fast_oos['final_cash']:,.2f} 元{'':<4}{res_plastic_fast_oos['roi']:+.2f}%{'':<6}{res_plastic_fast_oos['cagr']:+.2f}%{'':<6}{res_plastic_fast_oos['max_dd']:.2f}%{'':<6}{res_plastic_fast_oos['win_rate']:.1f}%{'':<6}{res_plastic_fast_oos['calmar']:.2f}", flush=True)
    print(f"==========================================================================================================\n", flush=True)

    summary = {
        'static_frozen_oos': res_static_oos,
        'plastic_adaptive_oos': res_plastic_oos,
        'plastic_adaptive_fast_oos': res_plastic_fast_oos
    }
    os.makedirs("runs", exist_ok=True)
    with open("runs/quant_plastic_adaptive_summary.json", "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, ensure_ascii=False)
    print(f"✓ 在线塑性消融审计工件已保存至 runs/quant_plastic_adaptive_summary.json\n", flush=True)

if __name__ == '__main__':
    main()
