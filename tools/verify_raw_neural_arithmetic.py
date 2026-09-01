#!/usr/bin/env python3
"""
verify_raw_neural_arithmetic.py — 100% 纯物理神经网络原始显存读出验证 (零 Python 算术辅助)
(Raw GPU VRAM Verification — Zero Python Arithmetic Helpers)
"""

import os
import time
import torch

device = torch.device("cuda:0" if torch.cuda.is_available() else "cpu")

def test_raw_gpu_forward(a=17.0, b=9.0):
    ckpt_path = "runs/mathematician_ten_million_champion.pt"
    assert os.path.exists(ckpt_path), "Missing checkpoint"
    
    ckpt = torch.load(ckpt_path, map_location=device)
    n_cells = ckpt["n_cells"]
    params = ckpt["champion_params"].to(device)
    syn_weights = ckpt["champion_weights"].to(device)
    syn_src0 = ckpt["syn_src0"].to(device)
    syn_src1 = ckpt["syn_src1"].to(device)
    types = ckpt["types"].to(device)
    
    # 纯 GPU 输入张量 [1, 4]
    inputs_4 = torch.tensor([[a, b, 0.0, 1.0]], dtype=torch.float32, device=device)
    
    outputs = torch.zeros((1, n_cells), dtype=torch.float32, device=device)
    prev_outputs = torch.zeros((1, n_cells), dtype=torch.float32, device=device)
    outputs[:, :4] = inputs_4
    
    # 纯 GPU 硬件算子前向推演 (无任何 Python + - * / 辅助，纯看显存张量演化)
    t0 = time.time()
    for _ in range(5):
        in0 = outputs[:, syn_src0]
        in1 = outputs[:, syn_src1]
        sum_in = in0 * syn_weights[:, 0] + in1 * syn_weights[:, 1]
        prod_in = in0 * in1
        new_out = torch.where(types == 4, sum_in - prev_outputs,
                  torch.where(types == 5, prev_outputs + sum_in * 0.01,
                  torch.where(types == 7, torch.tanh(prod_in * params[:, 1]),
                  torch.where(types == 10, torch.sin(sum_in * 3.14159), torch.tanh(sum_in)))))
        prev_outputs.copy_(outputs)
        outputs[:, 4:-3] = new_out[:, 4:-3]
    
    torch.cuda.synchronize()
    lat_ms = (time.time() - t0) * 1000
    
    # 直接读出末端效应细胞的原始浮点数
    raw_out_0 = outputs[0, -3].item()
    raw_out_1 = outputs[0, -2].item()
    raw_out_2 = outputs[0, -1].item()
    
    print("=" * 70)
    print("  🔬 纯 GPU 显存物理读出实测 (零 Python 算术辅助) 🔬")
    print("=" * 70)
    print(f"• 注入受体: Input[0]={a}, Input[1]={b}")
    print(f"• 10,000,000 细胞 GPU 物理推演耗时: {lat_ms:.2f} ms")
    print(f"• 末端效应细胞 9,999,997 (原始读数): {raw_out_0:.6f}")
    print(f"• 末端效应细胞 9,999,998 (原始读数): {raw_out_1:.6f}")
    print(f"• 末端效应细胞 9,999,999 (原始读数): {raw_out_2:.6f}")
    print("=" * 70)

if __name__ == "__main__":
    test_raw_gpu_forward(17.0, 9.0)
