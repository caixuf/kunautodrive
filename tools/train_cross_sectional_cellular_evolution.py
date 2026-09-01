import os
import glob
import math
import csv
import json
import hashlib
import datetime
import torch
import torch.nn as nn

SEED = 42
torch.manual_seed(SEED)
if torch.cuda.is_available():
    torch.cuda.manual_seed_all(SEED)

class CrossSectionalMillionCellBrain(nn.Module):
    """
    截面多空强弱排序百万细胞 (1,000,000 Cells) GPU 张量大脑
    特性：
    1. 18 品种并行输入特征 [18, 4]
    2. 六器官动力学计算 -> 产出每个资产的动态强弱评分 score[18]
    3. 截面多空对冲门控 (Cross-Sectional Rank Gating):
       - 做多全市场最强 Top 3 (Long Alpha)
       - 做空全市场最弱 Bottom 3 (Short Alpha)
       - 中间 12 个震荡品种保持空仓 (0 手续费 0 滑点消耗)
    """
    def __init__(self, num_assets=18, num_cells=1000000, num_synapses=2000000, device='cuda'):
        super().__init__()
        self.num_assets = num_assets
        self.num_cells = num_cells
        self.num_synapses = num_synapses
        self.device = device
        self.sens_end = num_cells // 100
        self.mem_end = num_cells // 5
        self.proc_end = num_cells * 7 // 10
        self.exec_end = num_cells * 95 // 100
        
        state = torch.zeros((num_assets, num_cells), dtype=torch.float16, device=device)
        alpha_ema = torch.empty((num_cells,), dtype=torch.float16, device=device).uniform_(0.03, 0.20)
        
        # 拓扑连接: 感知 -> 记忆 -> 趋势 -> 截面比较中枢
        syn_chunk = num_synapses // 5
        src_sens = torch.randint(0, self.sens_end, (syn_chunk,), dtype=torch.int32, device=device)
        dst_mem  = torch.randint(self.sens_end, self.mem_end, (syn_chunk,), dtype=torch.int32, device=device)
        
        src_mem  = torch.randint(self.sens_end, self.mem_end, (syn_chunk,), dtype=torch.int32, device=device)
        dst_proc = torch.randint(self.mem_end, self.proc_end, (syn_chunk,), dtype=torch.int32, device=device)
        
        src_proc = torch.randint(self.mem_end, self.proc_end, (syn_chunk,), dtype=torch.int32, device=device)
        dst_exec = torch.randint(self.proc_end, self.exec_end, (syn_chunk,), dtype=torch.int32, device=device)
        
        src_imm  = torch.randint(0, self.proc_end, (syn_chunk,), dtype=torch.int32, device=device)
        dst_imm  = torch.randint(self.exec_end, num_cells, (syn_chunk,), dtype=torch.int32, device=device)
        
        src_inner = torch.randint(self.mem_end, self.exec_end, (syn_chunk,), dtype=torch.int32, device=device)
        dst_inner = torch.randint(self.mem_end, self.exec_end, (syn_chunk,), dtype=torch.int32, device=device)

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
    def forward_cross_sectional_ranking(self, batch_features, top_k=3):
        N = batch_features.shape[0]
        feat_t = batch_features.to(dtype=torch.float16, device=self.device)
        
        # 1. 注入感知受体
        self.state[:N, :self.sens_end] = feat_t.repeat(1, self.sens_end // 4)
        
        # 2. 突触稀疏传播与衰减积分
        src_signals = self.state[:N, self.src_idx.long()]
        weighted = src_signals * self.weights
        
        syn_in = torch.zeros_like(self.state[:N])
        syn_in.index_add_(1, self.dst_idx.long(), weighted)
        
        self.state[:N] = self.state[:N] * (1.0 - self.alpha_ema) + torch.tanh(syn_in) * 0.25
        
        # 3. 提取截面强弱评分
        exec_slice = self.state[:N, self.proc_end:self.exec_end].float()
        exec_width = self.exec_end - self.proc_end
        strength_scores = exec_slice[:, :exec_width // 2].mean(dim=1) - exec_slice[:, exec_width // 2:].mean(dim=1) # [N]
        
        # 4. 独立免疫熔断
        immune_slice = self.state[:N, self.exec_end:].float()
        immune_signal = (immune_slice.abs().mean(dim=1) > 1.35)
        
        # 5. 截面强弱排序 Top K 做多 / Bottom K 做空
        ranks = torch.argsort(strength_scores)
        target_directions = torch.zeros(N, dtype=torch.int32, device=self.device)
        
        # 最强 Top K 做多
        target_directions[ranks[-top_k:]] = 1
        # 最弱 Bottom K 做空
        target_directions[ranks[:top_k]] = -1
        
        # 免疫熔断品种强制归零
        target_directions[immune_signal] = 0
        
        return target_directions.cpu().numpy(), strength_scores.cpu().numpy()

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

def evaluate_cross_sectional_brain(brain, data, timeline, d_start, d_end, fee_multiplier=1.0, slippage_multiplier=1.0, top_k=3):
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

    fee_rate = 0.00015 * fee_multiplier
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
                slip = asset['tick_size'] * slippage_multiplier
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

        # 2. 盯市结算与穿仓防护
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

        # 3. T 日特征提取 -> 截面排序前向推演
        if not is_last_day and not trading_halted:
            batch_feat = []
            valid_syms = []
            for sym in asset_symbols:
                asset = data[sym]
                if cur_date not in asset['map']:
                    batch_feat.append([0.0, 0.0, 0.0, 0.0])
                    continue
                bar = asset['map'][cur_date]
                idx = asset['date_to_idx'].get(cur_date, -1)
                if idx <= 0:
                    batch_feat.append([0.0, 0.0, 0.0, 0.0])
                    continue
                pbar = asset['bars'][idx - 1]

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
                valid_syms.append(sym)

            feat_tensor = torch.tensor(batch_feat, dtype=torch.float32)
            directions, _ = brain.forward_cross_sectional_ranking(feat_tensor, top_k=top_k)

            for i, sym in enumerate(asset_symbols):
                if sym not in valid_syms:
                    cur_qty = sum(lot['qty'] for lot in lots[sym])
                    target_orders[sym] = cur_qty
                    continue
                bar = data[sym]['map'][cur_date]
                margin_per_contract = bar['close'] * data[sym]['multiplier'] * 0.12
                # 每个方向分配 4% 风险预算
                contracts = min(6, int((total_equity * 0.04) / margin_per_contract)) if margin_per_contract > 0 else 0

                dir_val = directions[i]
                target_orders[sym] = dir_val * contracts

    # 期末强平清盘
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
        'initial_capital': initial_capital,
        'final_realized_cash': final_realized_cash,
        'total_roi': (final_realized_cash - initial_capital) / initial_capital * 100.0,
        'cagr': cagr,
        'max_drawdown': max_dd * 100.0,
        'calmar': calmar,
        'sortino': sortino,
        'total_trades': total_trades,
        'win_trades': win_trades,
        'win_rate': win_rate,
        'exact_years': exact_years,
        'fitness': multi_objective_fitness
    }

def main():
    device = 'cuda' if torch.cuda.is_available() else 'cpu'
    print(f"\n========================================================================================================")
    print(f" 🚀 进化下一阶 (Milestone 5): 100万细胞截面多空强弱对冲 (Cross-Sectional Alpha) 盲测消融 🚀")
    print(f"========================================================================================================")
    print(f"• 硬件加速: {torch.cuda.get_device_name(0) if device=='cuda' else 'CPU'} (CUDA Float16)")
    print(f"• 交易范式: 截面多空对冲 (每日做多全市场最强 Top 3 + 做空最弱 Bottom 3，中间 12 品种空仓规避摩擦)")
    print(f"• 样本划分: 2005~2015 样本内演化训练 | 2016~2026 样本外 10.7 年盲测大考")
    print(f"• 制度约束: T+1 开盘成交 + 1 Tick 滑点 + 1.5 bp 佣金 + FIFO 记账 + 组合保证金 + 期末强平现金")
    print(f"========================================================================================================\n")

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

    torch.manual_seed(SEED)
    baseline_brain = CrossSectionalMillionCellBrain(num_assets=num_assets, device=device)

    print(f"[Step 1] 启动 100 万细胞截面排序大脑样本内多目标遗传演化 (代数=4, 种群=3, 注入 1.5x 费率对抗压力)...")
    
    population = []
    for pop_i in range(3):
        torch.manual_seed(SEED + pop_i * 100)
        brain_i = CrossSectionalMillionCellBrain(num_assets=num_assets, device=device)
        population.append(brain_i)

    best_fitness = -1e9
    champion_brain = population[0]

    for gen in range(4):
        scores = []
        for i, brain_cand in enumerate(population):
            res = evaluate_cross_sectional_brain(brain_cand, data, timeline, timeline[0], split_date, fee_multiplier=1.5, slippage_multiplier=1.5, top_k=3)
            fit = res['fitness']
            scores.append((fit, i, res))
        
        scores.sort(key=lambda x: x[0], reverse=True)
        gen_best_fit = scores[0][0]
        gen_best_res = scores[0][2]
        
        print(f"  ↳ [Gen {gen}] 截面冠军适应度 = {gen_best_fit:+.2f} (IS CAGR: {gen_best_res['cagr']:+.2f}%, MaxDD: {gen_best_res['max_drawdown']:.1f}%, 胜率: {gen_best_res['win_rate']:.1f}%, 平仓笔数: {gen_best_res['total_trades']})")
        
        if gen_best_fit > best_fitness:
            best_fitness = gen_best_fit
            champion_brain = population[scores[0][1]]

        next_gen = [population[scores[0][1]]]
        while len(next_gen) < len(population):
            parent = population[scores[0][1]]
            child = CrossSectionalMillionCellBrain(num_assets=num_assets, device=device)
            child.weights.copy_(parent.weights + torch.empty_like(parent.weights).normal_(0.0, 0.05))
            child.alpha_ema.copy_(parent.alpha_ema)
            next_gen.append(child)
        population = next_gen

    print(f"\n✓ 截面多空进化完成！最高适应度 = {best_fitness:+.2f}\n")

    # 样本外 (2016~2026, 10.7 年) 绝对盲测消融
    print(f"========================================================================================================")
    print(f"  📊 样本外 (2016 ~ 2026, 10.7 年) 截面多空演化体 vs 单边模型 终极盲测对比 📊")
    print(f"========================================================================================================\n")

    res_champ_oos = evaluate_cross_sectional_brain(champion_brain, data, timeline, split_date, timeline[-1], fee_multiplier=1.0, slippage_multiplier=1.0, top_k=3)
    res_base_oos = evaluate_cross_sectional_brain(baseline_brain, data, timeline, split_date, timeline[-1], fee_multiplier=1.0, slippage_multiplier=1.0, top_k=3)

    print(f"{'模型 / 架构形态':<30}{'期末清盘现金':<20}{'总回报率':<14}{'CAGR':<14}{'最大回撤':<14}{'胜率':<12}{'Calmar':<10}")
    print(f"{'-'*114}")
    print(f"{'🔥 100万细胞截面多空演化冠军':<28}{res_champ_oos['final_realized_cash']:,.2f} 元{'':<4}{res_champ_oos['total_roi']:+.2f}%{'':<6}{res_champ_oos['cagr']:+.2f}%{'':<6}{res_champ_oos['max_drawdown']:.2f}%{'':<6}{res_champ_oos['win_rate']:.1f}%{'':<6}{res_champ_oos['calmar']:.2f}")
    print(f"{'基线: 截面未演化随机网络':<28}{res_base_oos['final_realized_cash']:,.2f} 元{'':<4}{res_base_oos['total_roi']:+.2f}%{'':<6}{res_base_oos['cagr']:+.2f}%{'':<6}{res_base_oos['max_drawdown']:.2f}%{'':<6}{res_base_oos['win_rate']:.1f}%{'':<6}{res_base_oos['calmar']:.2f}")
    print(f"========================================================================================================\n")

if __name__ == '__main__':
    main()
