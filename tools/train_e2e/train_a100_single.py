#!/usr/bin/env python3
"""train_a100_single.py — 单卡 A100 (40GB/80GB) 专属端到端流匹配（Flow-Matching）轨迹生成大模型训练器。

设计目标：
  1. 专为单卡 A100 优化：消除多卡 DDP 冗余开销，满血压榨 A100 单卡 312 TFLOPS BF16 算力与 1.5~2.0 TB/s HBM2e 显存带宽。
  2. 算法升级：告别 MTP（离散轨迹打分），采用连续 Flow-Matching（最优传输流匹配），11 步 Euler ODE 积分生成平滑轨迹（未来 3 秒 30 个时空点集 [x, y, v, yaw]）。
  3. A100 硬件特异加速：
     - Native BF16 混合精度 (torch.cuda.amp.autocast(dtype=torch.bfloat16))
     - PyTorch 2.x 计算图编译 (torch.compile)
     - 大 Batch Size (512~2048) + pin_memory + persistent_workers 高吞吐数据流
     - EMA (指数滑动平均) 权重平滑，消除轨迹抖动
  4. 一键交付：训练完成自动导出 policy_flow_matching.onnx，直接拷回本地 RTX 5060 接入 C++ inference_node 实时控车。

用法：
  # 在云端单卡 A100 上一键启动（自动生成 50000 帧样本测试）：
  python3 tools/train_e2e/train_a100_single.py --output models/a100_flow_policy/ --epochs 50

  # 使用真实数据集训练：
  python3 tools/train_e2e/train_a100_single.py --dataset datasets/v3_highway/ --output models/a100_flow_policy/ --batch-size 1024
"""

from __future__ import annotations

import argparse
import copy
import json
import math
import os
import random
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))


try:
    import torch
    from torch import nn
    from torch.utils.data import DataLoader, Dataset
except ModuleNotFoundError:
    torch = None
    nn = None
    DataLoader = None
    Dataset = object

def require_torch():
    if torch is None:
        print("error: PyTorch is required. Run: pip install torch numpy onnx", file=sys.stderr)
        raise SystemExit(2)
    return torch, nn, DataLoader, Dataset


# ── 1. Flow-Matching Policy 网络架构 ──────────────────────────────────

def build_flow_matching_model(nn, state_dim=59, traj_horizon=30, traj_dim=4, hidden_dim=512):
    """构建连续流匹配轨迹生成网络。"""

    class SinusoidalPosEmb(nn.Module):
        def __init__(self, dim):
            super().__init__()
            self.dim = dim

        def forward(self, t):
            if t.dim() == 1:
                t = t.unsqueeze(-1)
            half_dim = self.dim // 2
            emb = math.log(10000) / (half_dim - 1)
            emb = torch.exp(torch.arange(half_dim, device=t.device, dtype=torch.float32) * -emb)
            emb = t * emb.unsqueeze(0)
            return torch.cat((emb.sin(), emb.cos()), dim=-1)

    class FlowMatchingTrajectoryPolicy(nn.Module):
        def __init__(self):
            super().__init__()
            self.traj_horizon = traj_horizon
            self.traj_dim = traj_dim
            self.out_dim = traj_horizon * traj_dim  # 30 * 4 = 120

            # 1. 状态编码器 (自车状态 + 感知特征 + 道路几何)
            self.state_enc = nn.Sequential(
                nn.Linear(state_dim, hidden_dim),
                nn.SiLU(),
                nn.Linear(hidden_dim, hidden_dim),
                nn.LayerNorm(hidden_dim),
            )

            # 2. 噪声流时间步编码 (t in [0, 1])
            self.time_enc = nn.Sequential(
                SinusoidalPosEmb(hidden_dim),
                nn.Linear(hidden_dim, hidden_dim),
                nn.SiLU(),
                nn.Linear(hidden_dim, hidden_dim),
            )

            # 3. 带噪轨迹投影
            self.traj_proj = nn.Linear(self.out_dim, hidden_dim)

            # 4. 深度特征融合主干 (4 层 ResNet/MLP 带残差连接与 LayerNorm)
            self.block1 = nn.Sequential(
                nn.Linear(hidden_dim, hidden_dim),
                nn.SiLU(),
                nn.Linear(hidden_dim, hidden_dim),
            )
            self.ln1 = nn.LayerNorm(hidden_dim)

            self.block2 = nn.Sequential(
                nn.Linear(hidden_dim, hidden_dim),
                nn.SiLU(),
                nn.Linear(hidden_dim, hidden_dim),
            )
            self.ln2 = nn.LayerNorm(hidden_dim)

            # 5. 速度场向量预测头
            self.velocity_head = nn.Sequential(
                nn.SiLU(),
                nn.Linear(hidden_dim, self.out_dim),
            )

        def forward(self, state, x_t, t):
            """
            state: [B, state_dim]
            x_t:   [B, 120]  t 时刻插值轨迹
            t:     [B, 1]    时间步 in [0, 1]
            """
            s_emb = self.state_enc(state)
            t_emb = self.time_enc(t.squeeze(-1))
            x_emb = self.traj_proj(x_t)

            h = s_emb + x_emb + t_emb
            h = self.ln1(h + self.block1(h))
            h = self.ln2(h + self.block2(h))
            v_pred = self.velocity_head(h)
            return v_pred

        @torch.no_grad()
        def sample(self, state, steps=11):
            """11 步 Euler 积分采样，从标准高斯噪声生成平滑物理轨迹。"""
            B = state.shape[0]
            # x_0 ~ N(0, I)
            x = torch.randn(B, self.out_dim, device=state.device)
            dt = 1.0 / steps
            for step in range(steps):
                t = torch.full((B, 1), step * dt, device=state.device)
                v = self.forward(state, x, t)
                x = x + v * dt
            return x.view(B, self.traj_horizon, self.traj_dim)

    return FlowMatchingTrajectoryPolicy()


# ── 2. EMA 模型权重平滑器 ──────────────────────────────────────────

class ModelEMA:
    """指数滑动平均（EMA），提升轨迹生成平滑度，避免单 step 梯度噪点。"""
    def __init__(self, model, decay=0.999):
        self.ema_model = copy.deepcopy(model)
        self.ema_model.eval()
        self.decay = decay

    @torch.no_grad()
    def update(self, model):
        for ema_p, model_p in zip(self.ema_model.parameters(), model.parameters()):
            ema_p.data.mul_(self.decay).add_(model_p.data, alpha=1 - self.decay)


# ── 3. 数据集与合成流 ──────────────────────────────────────────────

def _wrap_angle(rad: float) -> float:
    return (rad + math.pi) % (2.0 * math.pi) - math.pi


def load_real_trajectory_dataset(
    dataset_path: Path,
    state_dim: int = 59,
    horizon: int = 30,
    dt: float = 0.1,
) -> tuple[list[list[float]], list[list[list[float]]]]:
    """从真实 samples.jsonl / dataset.jsonl 提取 59 维状态与未来 3s 自车系轨迹。"""
    jsonl_path = dataset_path / "samples.jsonl" if dataset_path.is_dir() else dataset_path
    if not jsonl_path.is_file():
        raise FileNotFoundError(f"dataset file not found: {jsonl_path}")

    records: list[dict] = []
    with jsonl_path.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            if isinstance(rec.get("ego"), dict) and isinstance(rec.get("t"), (int, float)):
                records.append(rec)

    if len(records) < 2:
        raise ValueError(f"not enough real ego records in {jsonl_path}: {len(records)}")

    # 按时间跳变或空间跳变切分独立轨迹段，避免跨场景拼接污染未来真值
    segments: list[list[dict]] = [[records[0]]]
    for rec in records[1:]:
        prev = segments[-1][-1]
        gap_s = (float(rec["t"]) - float(prev["t"])) / 1000.0
        jump_m = math.hypot(
            float(rec["ego"].get("x", 0.0)) - float(prev["ego"].get("x", 0.0)),
            float(rec["ego"].get("y", 0.0)) - float(prev["ego"].get("y", 0.0)),
        )
        if gap_s <= 0.0 or gap_s > 0.5 or jump_m > 15.0:
            segments.append([rec])
        else:
            segments[-1].append(rec)

    states: list[list[float]] = []
    trajectories: list[list[list[float]]] = []
    for seg in segments:
        t0_ms = float(seg[0]["t"])
        times = [(float(r["t"]) - t0_ms) / 1000.0 for r in seg]
        for i, rec in enumerate(seg):
            if times[-1] - times[i] < horizon * dt:
                break
            fv3 = rec.get("features_v3") or rec.get("features_v2") or rec.get("features") or []
            st = [0.0] * state_dim
            for k, val in enumerate(fv3[:state_dim]):
                st[k] = float(val)

            ego0 = rec["ego"]
            x0 = float(ego0.get("x", 0.0))
            y0 = float(ego0.get("y", 0.0))
            h0 = float(ego0.get("heading", 0.0))
            ch, sh = math.cos(h0), math.sin(h0)

            traj: list[list[float]] = []
            j = i
            for step in range(1, horizon + 1):
                target_t = times[i] + step * dt
                while j + 1 < len(seg) and times[j + 1] < target_t:
                    j += 1
                k = min(j + 1, len(seg) - 1)
                span = times[k] - times[j]
                alpha = (target_t - times[j]) / span if span > 1e-6 else 0.0
                alpha = max(0.0, min(1.0, alpha))

                ej, ek = seg[j]["ego"], seg[k]["ego"]
                wx = float(ej.get("x", 0.0)) + alpha * (float(ek.get("x", 0.0)) - float(ej.get("x", 0.0)))
                wy = float(ej.get("y", 0.0)) + alpha * (float(ek.get("y", 0.0)) - float(ej.get("y", 0.0)))
                wv = float(ej.get("v", 0.0)) + alpha * (float(ek.get("v", 0.0)) - float(ej.get("v", 0.0)))
                dh = _wrap_angle(float(ek.get("heading", 0.0)) - float(ej.get("heading", 0.0)))
                wh = _wrap_angle(float(ej.get("heading", 0.0)) + alpha * dh)

                dx, dy = wx - x0, wy - y0
                lx = ch * dx + sh * dy
                ly = -sh * dx + ch * dy
                traj.append([lx, ly, wv, _wrap_angle(wh - h0)])

            states.append(st)
            trajectories.append(traj)

    print(f"[Data] 已从 {jsonl_path} 加载真实轨迹窗口: {len(states)} 条 ({len(segments)} 段)")
    return states, trajectories


def create_synthetic_dataset(n_samples=50000, state_dim=59, horizon=30, traj_dim=4):
    """当无外部数据集时，自动合成物理自洽的高速巡航与避障轨迹数据集。"""
    print(f"[Data] 正在生成单卡 A100 压力测试合成数据集 ({n_samples} 帧样本)...")
    states = []
    trajectories = []
    for _ in range(n_samples):
        v0 = random.uniform(8.0, 25.0)       # 初始速度 30~90 km/h
        d0 = random.uniform(-1.0, 1.0)       # 初始偏航
        head0 = random.uniform(-0.05, 0.05)
        curve = random.uniform(-0.005, 0.005)

        # 59 维特征向量 (模拟 V3 特征)
        st = [0.0] * state_dim
        st[0] = v0
        st[1] = d0
        st[2] = head0
        st[3] = curve * v0
        st[4] = random.uniform(30.0, 120.0)  # 前车距
        st[5] = random.uniform(8.0, 25.0)    # 前车速
        states.append(st)

        # 未来 30 步 (dt=0.1s, 共 3.0s) 物理轨迹
        traj = []
        cx, cy, cv, ch = 0.0, d0, v0, head0
        for step in range(horizon):
            dt = 0.1
            cx += cv * math.cos(ch) * dt
            cy += cv * math.sin(ch) * dt - 0.2 * cy * dt  # 趋向道路中心
            ch += curve * cv * dt
            cv = max(0.0, cv + random.uniform(-0.1, 0.1))
            traj.append([cx, cy, cv, ch])
        trajectories.append(traj)

    return states, trajectories


class TrajectoryDataset(Dataset):
    def __init__(self, states, trajectories, torch):
        self.states = torch.tensor(states, dtype=torch.float32)
        # flatten: [N, 30, 4] -> [N, 120]
        self.trajectories = torch.tensor(trajectories, dtype=torch.float32).view(len(trajectories), -1)

    def __len__(self):
        return len(self.states)

    def __getitem__(self, idx):
        return self.states[idx], self.trajectories[idx]


# ── 4. 主训练流程 (单卡 A100 优化) ──────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="FlowEngine Single-A100 Flow-Matching Policy Trainer")
    parser.add_argument("--dataset", default=None, help="Path to exported dataset directory (optional)")
    parser.add_argument("--output", default="models/a100_flow_policy/", help="Artifact output directory")
    parser.add_argument("--epochs", type=int, default=50, help="Training epochs")
    parser.add_argument("--batch-size", type=int, default=1024, help="Batch size (A100 建议 1024~2048)")
    parser.add_argument("--lr", type=float, default=3e-4, help="Learning rate")
    parser.add_argument("--hidden", type=int, default=512, help="Hidden dimension (A100 512~768)")
    parser.add_argument("--num-workers", type=int, default=8, help="DataLoader workers")
    parser.add_argument("--no-compile", action="store_true", help="禁用 torch.compile 计算图编译")
    parser.add_argument("--export-onnx", action="store_true", default=True, help="训练后自动导出 ONNX")
    args = parser.parse_args()

    torch, nn, DataLoader, Dataset = require_torch()

    # 1. 硬件检测与环境报告
    if not torch.cuda.is_available():
        device = torch.device("cpu")
        print("⚠️ 未检测到 CUDA GPU，将在 CPU 上降级运行 (警告：仅供冒烟测试)")
    else:
        device = torch.device("cuda:0")
        gpu_name = torch.cuda.get_device_name(0)
        vram_gb = torch.cuda.get_device_properties(0).total_memory / (1024 ** 3)
        print("=" * 65)
        print(f"🚀 FlowEngine 单卡专属加速训练器启动")
        print(f"   检测到 GPU: {gpu_name}")
        print(f"   显存容量:   {vram_gb:.1f} GB")
        print(f"   计算特性:   Native BF16 + Tensor Cores + Flash Flow-Matching")
        print("=" * 65)

        # 针对 Ampere/A100 开启 TF32 与 cuDNN Benchmark
        torch.backends.cuda.matmul.allow_tf32 = True
        torch.backends.cudnn.allow_tf32 = True
        torch.backends.cudnn.benchmark = True

    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)

    # 2. 准备数据
    if args.dataset and Path(args.dataset).exists():
        states, trajectories = load_real_trajectory_dataset(Path(args.dataset))
    else:
        states, trajectories = create_synthetic_dataset(50000)

    eff_batch = min(args.batch_size, max(1, len(states)))
    dataset = TrajectoryDataset(states, trajectories, torch)
    loader = DataLoader(
        dataset,
        batch_size=eff_batch,
        shuffle=True,
        num_workers=args.num_workers if torch.cuda.is_available() else 0,
        pin_memory=torch.cuda.is_available(),
        persistent_workers=(args.num_workers > 0 and torch.cuda.is_available()),
        drop_last=len(states) >= eff_batch and eff_batch > 1,
    )

    # 3. 初始化模型与优化器
    model = build_flow_matching_model(nn, state_dim=59, traj_horizon=30, traj_dim=4, hidden_dim=args.hidden).to(device)
    ema = ModelEMA(model, decay=0.999)

    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=args.epochs * len(loader))

    # A100 PyTorch 2.x 计算图编译
    train_fn = model
    if torch.cuda.is_available() and not args.no_compile and hasattr(torch, "compile"):
        try:
            print("[Compile] 正在使用 PyTorch 2.x Inductor 编译计算图 (优化单卡算子融合)...")
            train_fn = torch.compile(model)
        except Exception as e:
            print(f"[Compile] 编译跳过 ({e})，使用标准 Eager 模式")
            train_fn = model

    # 4. 训练主循环 (Optimal Transport Flow-Matching)
    print(f"\n[Train] 开始训练 (总 Epochs: {args.epochs}, Batch Size: {args.batch_size}, 步数/Epoch: {len(loader)})...")
    start_time = time.time()

    for epoch in range(1, args.epochs + 1):
        epoch_loss = 0.0
        t0 = time.time()
        for batch_idx, (st, x_1) in enumerate(loader):
            st = st.to(device, non_blocking=True)
            x_1 = x_1.to(device, non_blocking=True)  # 真值未来轨迹目标 (Target Flow Endpoint)
            x_0 = torch.randn_like(x_1)             # 初始高斯噪声 (Noise Flow Source)

            # 最优传输路径 (Optimal Transport Probability Path): x_t = (1-t) x_0 + t x_1
            t = torch.rand(st.size(0), 1, device=device)
            x_t = (1.0 - t) * x_0 + t * x_1
            target_velocity = x_1 - x_0             # 目标流场速度矢量 u_t = x_1 - x_0

            optimizer.zero_grad(set_to_none=True)

            # 单卡 A100 专属 BF16 混合精度
            if torch.cuda.is_available():
                with torch.amp.autocast('cuda', dtype=torch.bfloat16):
                    v_pred = train_fn(st, x_t, t)
                    loss = nn.functional.mse_loss(v_pred, target_velocity)
            else:
                v_pred = train_fn(st, x_t, t)
                loss = nn.functional.mse_loss(v_pred, target_velocity)

            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            scheduler.step()
            ema.update(model)

            epoch_loss += loss.item()

        elapsed = time.time() - t0
        avg_loss = epoch_loss / len(loader)
        fps = (len(dataset)) / elapsed
        if epoch % 5 == 0 or epoch == 1 or epoch == args.epochs:
            print(f"  Epoch {epoch:02d}/{args.epochs:02d} | Loss: {avg_loss:.5f} | 吞吐: {fps:.0f} 帧/秒 | 耗时: {elapsed:.2f}s")

    total_time = time.time() - start_time
    print(f"\n✅ 训练完成! 总耗时: {total_time:.1f} 秒, 平均每 Epoch: {total_time/args.epochs:.2f}s")

    # 5. 保存权重与 Manifest
    pt_path = out_dir / "model_flow_a100.pt"
    torch.save({
        "epoch": args.epochs,
        "model_state": ema.ema_model.state_dict(),
        "hidden_dim": args.hidden,
        "state_dim": 59,
        "traj_horizon": 30,
        "traj_dim": 4,
    }, pt_path)
    print(f"[Save] 权重已保存至: {pt_path}")

    # 6. 一键导出 ONNX (供本地 RTX 5060 C++ 推理消费)
    if args.export_onnx:
        onnx_path = out_dir / "policy_flow_matching.onnx"
        print(f"[ONNX] 正在导出车端高效 ONNX 模型: {onnx_path} ...")
        ema.ema_model.eval()

        class ONNXExportWrapper(nn.Module):
            def __init__(self, policy):
                super().__init__()
                self.policy = policy

            def forward(self, state):
                # 固定 11 步采样导出为纯前向计算图
                return self.policy.sample(state, steps=11)

        export_model = ONNXExportWrapper(ema.ema_model).cpu()
        dummy_state = torch.randn(1, 59, dtype=torch.float32)

        try:
            torch.onnx.export(
                export_model,
                dummy_state,
                str(onnx_path),
                input_names=["state_feat"],
                output_names=["predicted_trajectory"],
                dynamic_axes={"state_feat": {0: "batch"}, "predicted_trajectory": {0: "batch"}},
                opset_version=14,
                dynamo=False,
            )
            print(f"🎉 导出成功！生成的 ONNX 模型尺寸: {onnx_path.stat().st_size / 1024 / 1024:.2f} MB")
            print("=" * 65)
            print("下一步使用说明：")
            print(f"  1. 将生成的 {onnx_path} 拷贝回本地 RTX 5060 环境：")
            print(f"     scp user@a100-cloud:{onnx_path.resolve()} ./models/")
            print("  2. 在本地执行 demo.sh 开启 20Hz 实时推理验证！")
            print("=" * 65)
        except Exception as err:
            print(f"⚠️ ONNX 导出跳过: {err}")

    # 写入元数据契约
    manifest = {
        "architecture": "Flow-Matching Trajectory Generator",
        "steps": 11,
        "state_dim": 59,
        "horizon_steps": 30,
        "horizon_seconds": 3.0,
        "target_hardware": "NVIDIA A100 (Train) -> RTX 5060 / Orin (Inference)",
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
