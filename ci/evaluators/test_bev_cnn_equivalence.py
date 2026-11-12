#!/usr/bin/env python3
"""
test_bev_cnn_equivalence.py — BEV 卷积网络三端前向等价性与高精度检测验证

验证目标：
  1. PyTorch 模型 vs ONNXRuntime (.onnx) vs 纯 C 原生推理核 (.bin)
     三端前向逐元素最大绝对误差 < 1e-4。
  2. 在 200+ 帧真实带噪多模态传感器仿真场景上断言：
     - Recall ≥ 85%
     - Precision ≥ 85%
     - 定位 MAE ≤ 0.65m
"""

import ctypes
import math
import random
import struct
from pathlib import Path

import pytest

np = pytest.importorskip("numpy")
ort = pytest.importorskip("onnxruntime")
torch = pytest.importorskip("torch")

from tools.bev_pipeline import H, RANGE_X, RANGE_Y_HALF, W, grid_res
from tools.train_bev.train_bev_centernet import (
    CenterPointLite,
    build_dataset,
    generate_synthetic_scene,
)

ROOT = Path(__file__).resolve().parents[2]
BIN_PATH = ROOT / "models" / "bev_centernet_v1.bin"
ONNX_PATH = ROOT / "models" / "bev_centernet_v1.onnx"
LIB_SO = ROOT / "build" / "lib" / "libbev_detection_node.so"


@pytest.fixture(scope="module")
def c_model():
    """加载 C 原生模型句柄"""
    assert LIB_SO.is_file(), f"missing {LIB_SO} — build project first"
    assert BIN_PATH.is_file(), f"missing {BIN_PATH} — train model first"

    lib = ctypes.CDLL(str(LIB_SO))
    model_buf = (ctypes.c_uint8 * 1024)()
    rc = lib.bev_cnn_load(ctypes.byref(model_buf), str(BIN_PATH).encode("utf-8"))
    assert rc == 0, f"bev_cnn_load failed for {BIN_PATH}"

    yield lib, model_buf
    lib.bev_cnn_free(ctypes.byref(model_buf))


@pytest.fixture(scope="module")
def ort_session():
    """初始化 ONNXRuntime 推理会话"""
    assert ONNX_PATH.is_file(), f"missing {ONNX_PATH} — train model first"
    sess = ort.InferenceSession(str(ONNX_PATH))
    return sess


@pytest.fixture(scope="module")
def torch_model():
    """加载与 .bin 权重完全一致的 PyTorch 模型"""
    assert BIN_PATH.is_file(), f"missing {BIN_PATH}"
    model = CenterPointLite()
    with open(BIN_PATH, "rb") as f:
        f.seek(4 * 9)  # skip 9 uint32s header
        convs = [model.conv1, model.conv2, model.conv3, model.conv4]
        for conv in convs:
            in_c, out_c, k_h, pad, stride, act = struct.unpack("<6I", f.read(24))
            w = np.frombuffer(f.read(out_c * in_c * k_h * k_h * 4), dtype=np.float32).copy().reshape(out_c, in_c, k_h, k_h)
            b = np.frombuffer(f.read(out_c * 4), dtype=np.float32).copy()
            conv.weight.data.copy_(torch.from_numpy(w))
            conv.bias.data.copy_(torch.from_numpy(b))
    model.eval()
    return model


def test_three_way_numerical_equivalence(torch_model, ort_session, c_model):
    """验证 PyTorch ⇄ ONNXRuntime ⇄ C native 三端逐元素最大绝对误差 < 1e-4"""
    lib, model_buf = c_model
    ort_in_name = ort_session.get_inputs()[0].name

    np.random.seed(2026)
    # 测试多个不同分布的输入张量（均匀、正态、稀疏带噪）
    for i in range(5):
        if i == 0:
            x = np.zeros((1, 6, H, W), dtype=np.float32)
        elif i == 1:
            x = np.ones((1, 6, H, W), dtype=np.float32)
        else:
            x = np.random.randn(1, 6, H, W).astype(np.float32)

        # 1. PyTorch 前向
        with torch.no_grad():
            out_torch = torch_model(torch.from_numpy(x)).numpy()

        # 2. ONNXRuntime 前向
        out_ort = ort_session.run(None, {ort_in_name: x})[0]

        # 3. C 原生核前向
        out_c = np.zeros((1, 10, H, W), dtype=np.float32)
        rc = lib.bev_cnn_forward(ctypes.byref(model_buf),
                                 x.ctypes.data_as(ctypes.c_void_p),
                                 out_c.ctypes.data_as(ctypes.c_void_p))
        assert rc == 0, "bev_cnn_forward returned non-zero"

        diff_torch_ort = float(np.max(np.abs(out_torch - out_ort)))
        diff_torch_c   = float(np.max(np.abs(out_torch - out_c)))
        diff_ort_c     = float(np.max(np.abs(out_ort - out_c)))

        print(f"\n[Sample {i}] Max Abs Diff:")
        print(f"  PyTorch vs ORT: {diff_torch_ort:.2e}")
        print(f"  PyTorch vs C:   {diff_torch_c:.2e}")
        print(f"  ORT vs C:       {diff_ort_c:.2e}")

        assert diff_torch_ort < 1e-4, f"PyTorch vs ORT diff {diff_torch_ort} >= 1e-4"
        assert diff_torch_c < 1e-4, f"PyTorch vs C diff {diff_torch_c} >= 1e-4"
        assert diff_ort_c < 1e-4, f"ORT vs C diff {diff_ort_c} >= 1e-4"


def test_detection_metrics_on_random_benchmark(torch_model, ort_session, c_model):
    """在 200+ 帧真实带噪随机场景上断言 Recall ≥ 85%、Precision ≥ 85%、MAE ≤ 0.65m"""
    lib, model_buf = c_model
    n_frames = 200
    res_x, res_y = grid_res(W, H)

    total_truth = 0
    total_pred = 0
    total_tp = 0
    pos_errors = []

    # 定义 C 结构体 BevPostDet
    class BevPostDet(ctypes.Structure):
        _fields_ = [
            ("x", ctypes.c_float),
            ("y", ctypes.c_float),
            ("vx", ctypes.c_float),
            ("vy", ctypes.c_float),
            ("width", ctypes.c_float),
            ("length", ctypes.c_float),
            ("type", ctypes.c_int),
            ("confidence", ctypes.c_float),
            ("world_y", ctypes.c_float),
        ]

    # 定义 C 结构体 BevPreConfig
    class BevPreConfig(ctypes.Structure):
        _fields_ = [
            ("w", ctypes.c_int),
            ("h", ctypes.c_int),
            ("range_x", ctypes.c_double),
            ("range_y_half", ctypes.c_double),
            ("v_scale", ctypes.c_double),
            ("channels", ctypes.c_int),
        ]

    cfg = BevPreConfig(W, H, RANGE_X, RANGE_Y_HALF, 30.0, 6)

    random.seed(888)
    np.random.seed(888)

    dets_arr = (BevPostDet * 128)()

    for frame in range(n_frames):
        feat, _, _, truth_objs = generate_synthetic_scene(res_x, res_y)
        total_truth += len(truth_objs)

        # 执行 C 原生推理
        out_c = np.zeros((10, H, W), dtype=np.float32)
        rc = lib.bev_cnn_forward(ctypes.byref(model_buf),
                                 feat.ctypes.data_as(ctypes.c_void_p),
                                 out_c.ctypes.data_as(ctypes.c_void_p))
        assert rc == 0

        # 执行 C 原生多头解码
        n_dets = lib.bev_cnn_decode(ctypes.byref(model_buf),
                                    out_c.ctypes.data_as(ctypes.c_void_p),
                                    ctypes.byref(cfg),
                                    ctypes.c_double(0.0),
                                    ctypes.c_double(0.0),
                                    dets_arr,
                                    128)
        total_pred += n_dets

        # 匹配真实目标 (距离 < 1.5m)
        matched = set()
        for d_idx in range(n_dets):
            d = dets_arr[d_idx]
            best_idx, best_dist = -1, 1e9
            for t_idx, t in enumerate(truth_objs):
                if t_idx in matched:
                    continue
                dist = math.hypot(d.x - t["xb"], d.y - t["yb"])
                if dist < best_dist and dist < 1.5:
                    best_dist = dist
                    best_idx = t_idx
            if best_idx >= 0:
                matched.add(best_idx)
                total_tp += 1
                pos_errors.append(best_dist)

    recall = total_tp / max(1, total_truth)
    precision = total_tp / max(1, total_pred)
    mae = float(np.mean(pos_errors)) if pos_errors else 0.0

    print(f"\n200-Frame Benchmark Results ({total_truth} ground truth targets):")
    print(f"  Total TP:    {total_tp}")
    print(f"  Total Pred:  {total_pred}")
    print(f"  Recall:      {recall * 100:.2f}% (assert >= 85%)")
    print(f"  Precision:   {precision * 100:.2f}% (assert >= 85%)")
    print(f"  Pos MAE:     {mae:.3f}m (assert <= 0.65m)")

    assert recall >= 0.85, f"Recall {recall:.3f} < 0.85"
    assert precision >= 0.85, f"Precision {precision:.3f} < 0.85"
    assert mae <= 0.65, f"MAE {mae:.3f}m > 0.65m"
