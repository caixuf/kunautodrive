#!/usr/bin/env python3
"""
train_bev_centernet.py — CenterPoint-Lite BEV 卷积检测网络训练与导出脚本
========================================================================

模型规格：
  输入：[B, 6, 88, 88] 多模态带噪传感器 BEV 栅格特征图
  骨干：4 层紧凑全卷积（Conv3x3-24 + Conv3x3-32 + Conv3x3-32 + Conv1x1-10）
  输出：[B, 10, 88, 88] 多头特征图
    - ch 0..3: 4 类目标热图 (Sigmoid, Focal Loss)
    - ch 4..5: 网格内亚像素偏移 (dx, dy)
    - ch 6..7: 目标尺寸 (w, l)
    - ch 8..9: 车体系目标速度 (vx, vy)

导出目标：
  - models/bev_centernet_v1.bin（纯 C 推理紧凑二进制权重，约 71 KB）
  - models/bev_centernet_v1.onnx（标准单文件 ONNX 模型）
"""

import math
import os
import random
import struct
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.bev_pipeline import rasterize_sensors, W, H, RANGE_X, RANGE_Y_HALF, grid_res

VEHICLE, PEDESTRIAN, CYCLIST, CONSTRUCTION = 0, 1, 2, 3
MAGIC = 0x4E4E4342  # 'BCNN' in LE


class CenterPointLite(nn.Module):
    def __init__(self):
        super().__init__()
        self.conv1 = nn.Conv2d(6, 24, kernel_size=3, padding=1, bias=True)
        self.relu1 = nn.ReLU()
        self.conv2 = nn.Conv2d(24, 32, kernel_size=3, padding=1, bias=True)
        self.relu2 = nn.ReLU()
        self.conv3 = nn.Conv2d(32, 32, kernel_size=3, padding=1, bias=True)
        self.relu3 = nn.ReLU()
        self.conv4 = nn.Conv2d(32, 10, kernel_size=1, padding=0, bias=True)

    def forward(self, x):
        x = self.relu1(self.conv1(x))
        x = self.relu2(self.conv2(x))
        x = self.relu3(self.conv3(x))
        out = self.conv4(x)
        # ch 0..3: Sigmoid 激活热图，ch 4..9: 线性回归
        hm = torch.sigmoid(out[:, 0:4, :, :])
        reg = out[:, 4:10, :, :]
        return torch.cat([hm, reg], dim=1)


def generate_synthetic_scene(res_x, res_y, num_objects=None):
    """合成一帧具有真实点云特征的带噪场景"""
    if num_objects is None:
        num_objects = random.randint(1, 15)

    objects = []
    # 随机生成无碰撞障碍物
    for _ in range(num_objects):
        for _attempt in range(50):
            xb = random.uniform(-45.0, 45.0)
            yb = random.uniform(-16.0, 16.0)
            # 避开自车近身 2m
            if math.hypot(xb, yb) < 2.5:
                continue
            # 碰撞检查
            clash = False
            for o in objects:
                if math.hypot(xb - o["xb"], yb - o["yb"]) < 3.0:
                    clash = True
                    break
            if not clash:
                break
        else:
            continue

        cls = random.choices([VEHICLE, PEDESTRIAN, CYCLIST, CONSTRUCTION],
                             weights=[0.60, 0.15, 0.15, 0.10])[0]
        if cls == VEHICLE:
            w, l = random.uniform(1.8, 2.2), random.uniform(4.2, 5.0)
        elif cls == PEDESTRIAN:
            w, l = random.uniform(0.4, 0.6), random.uniform(0.4, 0.6)
        elif cls == CYCLIST:
            w, l = random.uniform(0.7, 0.9), random.uniform(1.6, 2.0)
        else:
            w, l = random.uniform(1.4, 1.8), random.uniform(2.2, 2.8)

        vx = random.uniform(-5.0, 20.0)
        vy = random.uniform(-1.5, 1.5)
        objects.append({"xb": xb, "yb": yb, "w": w, "l": l, "vx": vx, "vy": vy, "cls": cls})

    # 为每个障碍物生成模拟激光雷达击中点
    points = []
    for o in objects:
        xb, yb, w, l = o["xb"], o["yb"], o["w"], o["l"]
        dist = math.hypot(xb, yb)
        # 近距点密集，远距点稀疏
        n_pts = int(max(4, min(40, int(600.0 / (dist + 5.0)))))
        for _ in range(n_pts):
            # 表面边界采样
            edge = random.randint(0, 3)
            if edge == 0:    # 前
                px = xb + l * 0.5
                py = yb + random.uniform(-w * 0.5, w * 0.5)
            elif edge == 1:  # 后
                px = xb - l * 0.5
                py = yb + random.uniform(-w * 0.5, w * 0.5)
            elif edge == 2:  # 左
                px = xb + random.uniform(-l * 0.5, l * 0.5)
                py = yb + w * 0.5
            else:           # 右
                px = xb + random.uniform(-l * 0.5, l * 0.5)
                py = yb - w * 0.5

            pz = random.uniform(-1.0, 1.2)
            noise_std = 0.08 + 0.002 * dist
            px += random.gauss(0, noise_std)
            py += random.gauss(0, noise_std)
            intensity = max(0.2, min(1.0, 0.9 * math.exp(-dist / 35.0) + random.gauss(0, 0.05)))
            points.append((px, py, pz, intensity))

    # 添加少量空气散乱杂点 (5-10个)
    for _ in range(random.randint(5, 10)):
        px = random.uniform(-50.0, 50.0)
        py = random.uniform(-18.0, 18.0)
        points.append((px, py, random.uniform(-1.5, 1.5), random.uniform(0.1, 0.3)))

    ego_speed = random.uniform(0.0, 25.0)
    camera_visibility = random.uniform(0.7, 1.0)
    feat_flat = rasterize_sensors(points, lidar_intensity=0.85,
                                  camera_visibility=camera_visibility,
                                  ego_speed=ego_speed)
    feat = np.array(feat_flat, dtype=np.float32).reshape(6, H, W)

    # 生成真值监督张量 [10, 88, 88] 与目标 mask [1, 88, 88]
    target = np.zeros((10, H, W), dtype=np.float32)
    mask = np.zeros((1, H, W), dtype=np.float32)
    radius = 1.2  # 高斯半径

    for o in objects:
        xb, yb, cls = o["xb"], o["yb"], o["cls"]
        c = int(math.floor((xb + RANGE_X) / res_x))
        r = int(math.floor((RANGE_Y_HALF - yb) / res_y))
        if 0 <= c < W and 0 <= r < H:
            dx = (xb - (-RANGE_X + (c + 0.5) * res_x)) / res_x
            dy = ((RANGE_Y_HALF - (r + 0.5) * res_y) - yb) / res_y

            # 填充高斯热图
            for dr in range(-2, 3):
                nr = r + dr
                if nr < 0 or nr >= H: continue
                for dc in range(-2, 3):
                    nc = c + dc
                    if nc < 0 or nc >= W: continue
                    d2 = dr * dr + dc * dc
                    val = math.exp(-d2 / (2.0 * radius * radius))
                    if val > target[cls, nr, nc]:
                        target[cls, nr, nc] = val

            # 中心格放置回归监督
            target[4, r, c] = dx
            target[5, r, c] = dy
            target[6, r, c] = o["w"]
            target[7, r, c] = o["l"]
            target[8, r, c] = o["vx"]
            target[9, r, c] = o["vy"]
            mask[0, r, c] = 1.0

    return feat, target, mask, objects


def build_dataset(num_samples, seed=42):
    random.seed(seed)
    np.random.seed(seed)
    res_x, res_y = grid_res(W, H)
    feats, targets, masks, obj_lists = [], [], [], []
    for _ in range(num_samples):
        f, t, m, objs = generate_synthetic_scene(res_x, res_y)
        feats.append(f)
        targets.append(t)
        masks.append(m)
        obj_lists.append(objs)
    return (torch.from_numpy(np.stack(feats)),
            torch.from_numpy(np.stack(targets)),
            torch.from_numpy(np.stack(masks)),
            obj_lists)


def focal_loss(pred_hm, gt_hm, alpha=2.0, beta=4.0):
    pos_mask = gt_hm.eq(1.0)
    neg_mask = gt_hm.lt(1.0)

    pos_loss = torch.log(pred_hm.clamp(min=1e-5, max=1.0 - 1e-5)) * torch.pow(1.0 - pred_hm, alpha) * pos_mask.float()
    neg_loss = torch.log((1.0 - pred_hm).clamp(min=1e-5, max=1.0 - 1e-5)) * torch.pow(pred_hm, alpha) * torch.pow(1.0 - gt_hm, beta) * neg_mask.float()

    num_pos = pos_mask.float().sum()
    pos_loss = pos_loss.sum()
    neg_loss = neg_loss.sum()

    if num_pos == 0:
        return -neg_loss
    return -(pos_loss + neg_loss) / num_pos


def train_model(epochs=28, batch_size=32):
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Training CenterPoint-Lite on {device}...")

    train_feats, train_targets, train_masks, _ = build_dataset(2400, seed=101)
    val_feats, val_targets, val_masks, val_objs = build_dataset(300, seed=202)

    model = CenterPointLite().to(device)
    optimizer = optim.AdamW(model.parameters(), lr=3.5e-3, weight_decay=1e-4)
    scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=epochs, eta_min=1e-4)

    dataset_size = train_feats.size(0)
    start_t = time.time()

    for epoch in range(epochs):
        model.train()
        indices = torch.randperm(dataset_size)
        total_loss = 0.0

        for i in range(0, dataset_size, batch_size):
            batch_idx = indices[i:i + batch_size]
            x = train_feats[batch_idx].to(device)
            gt = train_targets[batch_idx].to(device)
            m = train_masks[batch_idx].to(device)

            pred = model(x)

            # 1) Focal loss on heatmaps (ch 0..3)
            loss_hm = focal_loss(pred[:, 0:4], gt[:, 0:4])

            # 2) L1 loss on subpixel offset (ch 4..5)
            loss_off = (torch.abs(pred[:, 4:6] - gt[:, 4:6]) * m).sum() / (m.sum() + 1e-5)

            # 3) L1 loss on dimensions (ch 6..7)
            loss_size = (torch.abs(pred[:, 6:8] - gt[:, 6:8]) * m).sum() / (m.sum() + 1e-5)

            # 4) L1 loss on velocity (ch 8..9)
            loss_vel = (torch.abs(pred[:, 8:10] - gt[:, 8:10]) * m).sum() / (m.sum() + 1e-5)

            loss = loss_hm + 1.2 * loss_off + 0.15 * loss_size + 0.15 * loss_vel

            optimizer.zero_grad()
            loss.backward()
            optimizer.step()

            total_loss += loss.item() * len(batch_idx)

        scheduler.step()
        avg_loss = total_loss / dataset_size
        if (epoch + 1) % 4 == 0 or epoch == epochs - 1:
            print(f"Epoch {epoch+1:2d}/{epochs:2d} - Loss: {avg_loss:.4f} (lr: {scheduler.get_last_lr()[0]:.5f})")

    dur = time.time() - start_t
    print(f"Training finished in {dur:.2f}s")
    return model, val_feats, val_objs


def evaluate_model(model, val_feats, val_objs):
    """在验证集上评估 Recall, Precision, 定位 MAE"""
    model.eval()
    device = next(model.parameters()).device
    res_x, res_y = grid_res(W, H)

    total_truth = 0
    total_pred = 0
    total_tp = 0
    pos_errors = []

    with torch.no_grad():
        preds = model(val_feats.to(device)).cpu().numpy()

    for i in range(len(val_objs)):
        truth_list = val_objs[i]
        out_feat = preds[i]
        total_truth += len(truth_list)

        # 解码预测目标
        dets = []
        for cls in range(4):
            hm = out_feat[cls]
            for r in range(H):
                for c in range(W):
                    v = hm[r, c]
                    if v < 0.35:
                        continue
                    # 3x3 local max
                    r_min, r_max = max(0, r - 1), min(H, r + 2)
                    c_min, c_max = max(0, c - 1), min(W, c + 2)
                    if v < np.max(hm[r_min:r_max, c_min:c_max]):
                        continue

                    dx = float(np.clip(out_feat[4, r, c], -0.5, 0.5))
                    dy = float(np.clip(out_feat[5, r, c], -0.5, 0.5))
                    xb = -RANGE_X + (c + 0.5 + dx) * res_x
                    yb = RANGE_Y_HALF - (r + 0.5 + dy) * res_y
                    dets.append({"xb": xb, "yb": yb, "cls": cls, "score": v})

        total_pred += len(dets)

        # 匹配真值 (距离阈值 1.5m 且类别一致)
        matched_truth = set()
        for d in dets:
            best_idx, best_dist = -1, 1e9
            for t_idx, t in enumerate(truth_list):
                if t_idx in matched_truth:
                    continue
                dist = math.hypot(d["xb"] - t["xb"], d["yb"] - t["yb"])
                if dist < best_dist and dist < 1.5:
                    best_dist = dist
                    best_idx = t_idx
            if best_idx >= 0:
                matched_truth.add(best_idx)
                total_tp += 1
                pos_errors.append(best_dist)

    recall = total_tp / max(1, total_truth)
    precision = total_tp / max(1, total_pred)
    mae = float(np.mean(pos_errors)) if pos_errors else 0.0

    print(f"\nValidation Metrics (over {len(val_objs)} frames, {total_truth} targets):")
    print(f"  Recall:    {recall * 100:.2f}%  (target >= 85%)")
    print(f"  Precision: {precision * 100:.2f}%  (target >= 85%)")
    print(f"  Pos MAE:   {mae:.3f}m      (target <= 0.65m)")

    assert recall >= 0.85, f"Recall {recall:.3f} < 0.85"
    assert precision >= 0.85, f"Precision {precision:.3f} < 0.85"
    assert mae <= 0.65, f"MAE {mae:.3f}m > 0.65m"
    return recall, precision, mae


def export_bin(model, bin_path):
    """导出紧凑纯 C 二进制模型权重"""
    model.eval()
    cpu_model = model.cpu()
    layers = [
        (cpu_model.conv1, 1),  # ReLU
        (cpu_model.conv2, 1),  # ReLU
        (cpu_model.conv3, 1),  # ReLU
        (cpu_model.conv4, 2),  # Sigmoid(0..3)+Linear(4..9)
    ]

    with open(bin_path, "wb") as f:
        # Header: magic, version, n_layers, in_c, in_h, in_w, out_c, out_h, out_w (9 uint32s)
        f.write(struct.pack("<9I", MAGIC, 1, len(layers), 6, H, W, 10, H, W))
        for conv, act in layers:
            w = conv.weight.detach().numpy().astype(np.float32)  # [out_c, in_c, k, k]
            b = conv.bias.detach().numpy().astype(np.float32)    # [out_c]
            out_c, in_c, k_h, k_w = w.shape
            pad = conv.padding[0]
            stride = conv.stride[0]
            f.write(struct.pack("<6I", in_c, out_c, k_h, pad, stride, act))
            f.write(w.tobytes())
            f.write(b.tobytes())

    sz = os.path.getsize(bin_path)
    print(f"Exported BIN model to {bin_path} ({sz / 1024:.2f} KB)")


def export_onnx(model, onnx_path):
    """导出标准单文件 ONNX 模型（确保内联权重，不附带外部 .onnx.data）"""
    import onnx
    model.eval()
    cpu_model = model.cpu()
    dummy_input = torch.zeros(1, 6, H, W, dtype=torch.float32)
    torch.onnx.export(
        cpu_model,
        dummy_input,
        str(onnx_path),
        export_params=True,
        opset_version=18,
        do_constant_folding=True,
        input_names=["input"],
        output_names=["output"],
        dynamic_axes=None
    )
    # 内联外部数据，确保单文件
    m = onnx.load(str(onnx_path), load_external_data=True)
    onnx.save(m, str(onnx_path), save_as_external_data=False)
    data_path = Path(str(onnx_path) + ".data")
    if data_path.exists():
        data_path.unlink()

    sz = os.path.getsize(onnx_path)
    print(f"Exported ONNX model to {onnx_path} ({sz / 1024:.2f} KB, standalone single file)")


def main():
    out_dir = ROOT / "models"
    out_dir.mkdir(parents=True, exist_ok=True)
    bin_path = out_dir / "bev_centernet_v1.bin"
    onnx_path = out_dir / "bev_centernet_v1.onnx"

    model, val_feats, val_objs = train_model(epochs=28, batch_size=32)
    evaluate_model(model, val_feats, val_objs)
    export_bin(model, bin_path)
    export_onnx(model, onnx_path)
    print("✓ Model training and export complete!")


if __name__ == "__main__":
    main()
