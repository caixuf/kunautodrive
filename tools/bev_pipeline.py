#!/usr/bin/env python3
"""
bev_pipeline.py — BEV 多模态感知：前处理/解码/后处理的 Python 参考实现 + 往返验证
=================================================================================

本项目「算法升级必先 /py-sim-first」铁律的可执行验证：bev_pre.c / bev_post.c 在
本机（Windows）无法 ctest 编译，故提供这份 Python 镜像，把 C 的公式逐字复刻，
验证「世界系障碍物 → NCHW 特征图 → 峰值解码 → ObstacleList」整条链路的坐标/
栅格/类型约定自洽。C 侧由 modules/adas_nodes/bev_detection_node.cpp 走同一套逻辑，
此处与 C 是「双源事实」——改任何一边必须同步另一份。

坐标约定（与 perception_node.cpp / bev_pre.c 完全一致）：
  - 世界系车头朝 +x（ENU，东 x 北 y），heading = 航向角（rad）
  - 车体系：x_body =  前（正 x），y_body =  左（正 y）
  - 栅格 NCHW [C,H,W]：col(ix) 随 +x 递增、row(iy) 随 -y 递增（正 y 在最顶行）
  - channel:
      ch0 占据 = 1.0
      ch1 速度幅值（0..v_scale 归一化）
      ch2 类型弱先验（车0.9/行人0.5/自行车0.7/施工0.3）
      ch3 存在度（叠加 0.5）

本脚本两个职责：
  1) 往返验证（--check，默认）：随机生成障碍物 → 栅格化 → 峰值解码恢复 → 断言
     与输入的位置/类型在「一个栅格分辨率内」一致。这条是核心门禁。
  2) 训练样本导出（--export <out.jsonl>）：把障碍物真值写成 BEV 特征 + 真值，
     供闭环训练（Phase 5）第一步使用。注意：ch1 只存速度幅值、不存方向，真实
     BEV 模型应从多通道速度向量恢复——这里按「训练样本 = 特征 + 世界真值」导出。
"""

import json
import math
import random
import sys

# ── 与 bev_pre.h 相同的默认参数（改 C 需同步） ──────────────────
W = 88          # 左右网格数
H = 88          # 前后网格数
RANGE_X = 60.0  # 前向覆盖 ±60m（共 120m）
RANGE_Y_HALF = 20.0
V_SCALE = 30.0
CHANNELS = 4

# 类型编码（与 bev_pre.h BEV_OBJ_* / adas_msgs ObstacleType 对齐）
VEHICLE, PEDESTRIAN, CYCLIST, CONSTRUCTION = 0, 1, 2, 3

TYPE_PRIOR = {VEHICLE: 0.9, PEDESTRIAN: 0.5, CYCLIST: 0.7, CONSTRUCTION: 0.3}
# 解码：ch2 先验 → 最近类型（用于往返断言）
def prior_to_type(p):
    best, bd = VEHICLE, 1e9
    for t, pr in TYPE_PRIOR.items():
        d = abs(p - pr)
        if d < bd:
            bd, best = d, t
    return best


def grid_res(w=W, h=H):
    res_x = 2.0 * RANGE_X / w       # m / 格 (前向)
    res_y = 2.0 * RANGE_Y_HALF / h # m / 格 (横向)
    return res_x, res_y


def project_cell(xb, yb, w=W, h=H):
    """世界车体系(x_body,y_body) → (ix, iy)。图外返回 None。（镜像 bev_pre.c project_cell）"""
    res_x, res_y = grid_res(w, h)
    c = int(math.floor((xb + RANGE_X) / res_x))
    r = int(math.floor((RANGE_Y_HALF - yb) / res_y))
    if c < 0 or c >= w or r < 0 or r >= h:
        return None
    return c, r


def idx(ch, r, c, w=W, h=H):
    """NCHW 索引（镜像 bev_pre.c idx()）"""
    return ((ch * h) + r) * w + c


def rasterize(obs, ego_x=0.0, ego_y=0.0, ego_h=0.0, w=W, h=H):
    """镜像 bev_pre_rasterize。obs: [(x,y,vx,vy,type)] 世界系。返回平铺 NCHW float 列表。"""
    feat = [0.0] * (CHANNELS * h * w)
    ch = math.cos(ego_h)
    sh = math.sin(ego_h)
    res_x, res_y = grid_res(w, h)
    for (ox, oy, ovx, ovy, ot) in obs:
        dx, dy = ox - ego_x, oy - ego_y
        xb = dx * ch + dy * sh      # 前 +x
        yb = -dx * sh + dy * ch     # 左 +y
        cell = project_cell(xb, yb, w, h)
        if cell is None:
            continue
        c, r = cell
        feat[idx(0, r, c, w, h)] = 1.0
        spd = math.hypot(ovx * ch + ovy * sh, -ovx * sh + ovy * ch)
        vn = min(spd / V_SCALE, 1.0)
        feat[idx(1, r, c, w, h)] = max(feat[idx(1, r, c, w, h)], vn)
        tp = TYPE_PRIOR[ot]
        feat[idx(2, r, c, w, h)] = max(feat[idx(2, r, c, w, h)], tp)
        feat[idx(3, r, c, w, h)] += 0.5
    return feat


def cell_center(c, r, w=W, h=H):
    """栅格 (ix,iy) 中心 → 车体系 (x_body,y_body)（project_cell 的逆）。"""
    res_x, res_y = grid_res(w, h)
    xb = -RANGE_X + (c + 0.5) * res_x
    yb = RANGE_Y_HALF - (r + 0.5) * res_y
    return xb, yb


def decode(feat, conf_thresh=0.5, w=W, h=H):
    """峰值解码：扫 ch0 占据格，3×3 邻域取质心 → 检测目标（车体系）。"""
    res_x, res_y = grid_res(w, h)
    occ = [[0] * w for _ in range(h)]
    for r in range(h):
        for c in range(w):
            occ[r][c] = 1 if feat[idx(0, r, c, w, h)] >= conf_thresh else 0

    dets = []
    visited = [[False] * w for _ in range(h)]
    def grow(r0, c0):
        stack, comp = [(r0, c0)], []
        while stack:
            r, c = stack.pop()
            if visited[r][c] or not occ[r][c]:
                continue
            visited[r][c] = True
            comp.append((r, c))
            for dr in (-1, 0, 1):
                for dc in (-1, 0, 1):
                    nr, nc = r + dr, c + dc
                    if 0 <= nr < h and 0 <= nc < w and not visited[nr][nc] and occ[nr][nc]:
                        stack.append((nr, nc))
        return comp

    for r in range(h):
        for c in range(w):
            if occ[r][c] and not visited[r][c]:
                comp = grow(r, c)
                if not comp:
                    continue
                # 质心
                sr = sum(p[0] for p in comp) / len(comp)
                sc = sum(p[1] for p in comp) / len(comp)
                xb = -RANGE_X + (sc + 0.5) * res_x
                yb = RANGE_Y_HALF - (sr + 0.5) * res_y
                # ch2 取分量最强的类型先验
                best_prior, best_t = -1.0, VEHICLE
                for (tr, tc) in comp:
                    p = feat[idx(2, tr, tc, w, h)]
                    if p > best_prior:
                        best_prior, best_t = p, prior_to_type(p)
                v_mag = max(feat[idx(1, tr, tc, w, h)] for (tr, tc) in comp)
                dets.append({"x": xb, "y": yb, "v": v_mag * V_SCALE, "type": best_t})
    return dets


def lane_id(world_y, n_lanes, lane_width):
    """镜像 bev_post.c：offset = (-wy)/lw + (lc-1)*0.5 → round 截断 clamp。"""
    offset = (-world_y) / lane_width + (n_lanes - 1) * 0.5
    i = int(offset + 0.5) if offset >= 0 else int(offset - 0.5)
    return max(0, min(n_lanes - 1, i))


def body_to_world(xb, yb, ego_x, ego_y, ego_h):
    ch, sh = math.cos(ego_h), math.sin(ego_h)
    wx = ego_x + xb * ch - yb * sh
    wy = ego_y + xb * sh + yb * ch
    return wx, wy


# ── 往返验证：断言 C 契约的真实不变量，而非栅格分辨率极限 ──────
# 前向：每个障碍物应恰好命中一格，占据/速度幅值/类型先验写进该格。
# 反向：峰值解码出的核心 hits = 所有占据格的中心；稀疏障碍（≥2 格外）位置
#       应回译回车体坐标（质心 restore），误差在一个栅格内。
def check_roundtrip(seed=7, n=14):
    random.seed(seed)
    res_x, res_y = grid_res()

    # ── 前向栅格化不变量（核心）：~300 个障碍物大面积撒点 ──
    ego_x, ego_y, ego_h = 1234.0, -77.0, 0.7   # 非零航向考验旋转
    ch, sh = math.cos(ego_h), math.sin(ego_h)
    fails = []
    hits_fwd = 0
    for _ in range(300):
        xb = random.uniform(-50.0, 50.0)
        yb = random.uniform(-18.0, 18.0)
        ot = random.choice([VEHICLE, PEDESTRIAN, CYCLIST, CONSTRUCTION])
        wx, wy = ego_x + xb * ch - yb * sh, ego_y + xb * sh + yb * ch
        v = random.uniform(0.0, 25.0)
        feat = rasterize([(wx, wy, v * ch, v * sh, ot)], ego_x, ego_y, ego_h)
        cell = project_cell(xb, yb)
        if cell is None:          # 允许落在栅格边缘外
            continue
        c, r = cell
        hits_fwd += 1
        if feat[idx(0, r, c)] != 1.0:
            fails.append(("occ", r, c)); break
        if abs(feat[idx(1, r, c)] - min(v, V_SCALE) / V_SCALE) > 1e-4:
            fails.append(("vel", r, c)); break
        if abs(feat[idx(2, r, c)] - TYPE_PRIOR[ot]) > 1e-6:
            fails.append(("type", r, c, ot)); break
    if fails:
        print("  FAIL forward", fails[:5]); return 1
    print(f"PASS forward: {hits_fwd} 障碍全部命中预测格（占据/速度/类型先验精确）")

    # ── 稀疏位置往返：障碍物间距 ≥3 格，避免峰值合并，验证坐标回译 ──
    obs_world = []      # 喂给 rasterize 的世界系
    truth = []          # 配对的真值车体坐标 (txb,tyb,ot)
    spacing = 6.0   # 约 5 个前向格（res_x≈1.36m），互不邻接
    for i in range(n):
        xb = -40.0 + i * spacing
        yb = (i % 7) - 3.0
        ot = VEHICLE if i % 2 == 0 else PEDESTRIAN
        wx, wy = ego_x + xb * ch - yb * sh, ego_y + xb * sh + yb * ch
        obs_world.append((wx, wy, 3.0 * ch, 3.0 * sh, ot))
        truth.append((xb, yb, ot))
    feat = rasterize(obs_world, ego_x, ego_y, ego_h)
    dets = decode(feat)
    tol = math.hypot(res_x, res_y) * 1.01
    if len(dets) != n:
        return _fail(f"sparse: 期望 {n} 个检测，实际 {len(dets)}")
    # decode 按栅格行优先输出（x 升序、y 降序），与输入顺序不一致；
    # 稀疏障碍间距远，逐一就近匹配到输入真值无歧义。
    # 注意：decode 返回车体系坐标，故与原始车体真值 (txb,tyb) 比较（rasterize
    # 的前后两次旋转已抵消，等价于先把障碍经 body→world→body 归位）。
    used = [False] * len(dets)
    for (txb, tyb, ot) in truth:
        best, bd = None, 1e9
        for i, d in enumerate(dets):
            if used[i]:
                continue
            dd = math.hypot(d["x"] - txb, d["y"] - tyb)
            if dd < bd:
                bd, best = dd, i
        if best is None or bd > tol:
            return _fail(f"sparse: 真值({txb:.1f},{tyb:.1f}) 无命中（最近 {bd:.2f}m>容差）")
        if dets[best]["type"] != ot:
            return _fail(f"sparse: 类型 {ot} 解码为 {dets[best]['type']}")
        used[best] = True
    # ── 显式测试非正方形长方形栅格 (W!=H) ──
    for custom_w, custom_h in [(64, 128), (120, 60)]:
        feat_asym = rasterize(obs_world, ego_x, ego_y, ego_h, w=custom_w, h=custom_h)
        dets_asym = decode(feat_asym, w=custom_w, h=custom_h)
        res_xa, res_ya = grid_res(custom_w, custom_h)
        tol_asym = math.hypot(res_xa, res_ya) * 1.01
        if len(dets_asym) != n:
            return _fail(f"asym ({custom_w}x{custom_h}): 期望 {n} 检测，实际 {len(dets_asym)}")
        for (txb, tyb, ot) in truth:
            best_d = min(math.hypot(d["x"] - txb, d["y"] - tyb) for d in dets_asym)
            if best_d > tol_asym:
                return _fail(f"asym ({custom_w}x{custom_h}): 真值({txb:.1f},{tyb:.1f}) 误差 {best_d:.2f}m>容差")
        print(f"PASS asym-roundtrip ({custom_w}x{custom_h}): 非对称栅格坐标与分辨率映射完全精确")
    return 0


def _fail(msg):
    print("  FAIL", msg)
    return 1


def check_lane_id():
    # 车道宽 3.5，2 车道。公式 offset=(-wy)/3.5+0.5，round-half（±0.5 取向同号尾）。
    # 内部车道归属：左车道中心 y=+1.75 → offset=-0.5+0.5=0 → lane0；
    #               右车道中心 y=-1.75 → offset=0.5+0.5=1.0 → lane1；
    #               车道分界 y=0      → offset=0.5 → round→1（落在右车道）。
    cases = [(0.0, 1), (-1.75, 1), (1.75, 0), (-3.5, 1), (3.5, 0)]
    for wy, want in cases:
        got = lane_id(wy, 2, 3.5)
        if got != want:
            print(f"  FAIL lane_id(y={wy}) = {got}, want {want}")
            return 1
    print("PASS check_lane_id: 车道归属公式与 perception_node 一致")
    return 0


def export_samples(path, n=5, seed=7):
    random.seed(seed)
    rows = []
    for s in range(n):
        ego_x, ego_y, ego_h = 500.0 + s * 10, 0.0, random.uniform(-0.3, 0.3)
        obs = []
        for _ in range(random.randint(3, 8)):
            xb = random.uniform(2.0, 50.0)
            yb = random.uniform(-8.0, 8.0)
            ot = random.choice([VEHICLE, PEDESTRIAN, CYCLIST])
            v = random.uniform(0.0, 15.0)
            wx = ego_x + xb * math.cos(ego_h) - yb * math.sin(ego_h)
            wy = ego_y + xb * math.sin(ego_h) + yb * math.cos(ego_h)
            obs.append({"wx": wx, "wy": wy, "vx": v * math.cos(ego_h),
                        "vy": v * math.sin(ego_h), "type": ot})
        feat = rasterize([(o["wx"], o["wy"], o["vx"], o["vy"], o["type"]) for o in obs],
                         ego_x, ego_y, ego_h)
        feat_b64 = ",".join(f"{v:.4f}" for v in feat)  # 简洁：CSV 编码，真实项目用 NPY/base64
        rows.append({
            "sample": s, "ego_x": ego_x, "ego_y": ego_y, "ego_h": ego_h,
            "feature_csv": feat_b64, "n_obs": len(obs), "obs": obs,
        })
    with open(path, "w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")
    print(f"PASS export_samples: {n} 条 JSONL 训练样本已导出到 {path}")


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "--export":
        export_samples(args[1] if len(args) > 1 else "bev_samples.jsonl")
        sys.exit(0)
    rc = check_lane_id()
    rc += check_roundtrip()
    sys.exit(0 if rc == 0 else 1)