#!/usr/bin/env python3
"""
train_cellular_language_model_10m.py — 千万级 (10,000,000 细胞/参数) 原生形态发生语言生成模型
(10-Million Cell Causal Language Model on Chinese Text Corpus — Author: Li Longfei)

实验目的：
通过真实的“预测下一个字符（Next-Token Prediction）”自回归目标，
将 10,000,000 个细胞参数在中文真实语料上进行端到端优化，
直观观察网络如何从最初的“随机乱码”逐步学会中文语法与词汇连贯性！
"""

import os
import sys
import time
import math
import torch
import torch.nn as nn
import torch.nn.functional as F

device = torch.device("cuda:0" if torch.cuda.is_available() else "cpu")
gpu_name = torch.cuda.get_device_name(0) if torch.cuda.is_available() else "CPU"

# 1. 准备训练语料 (包含日常对话、哲学问答、科学论述的综合中文文本)
TRAIN_CORPUS = """
李龙飞先生创立了形态发生计算生命系统。形态发生是指生物组织通过局部细胞相互作用自发形成复杂拓扑结构的过程。
在自然界中，生命的演化经历了漫长的时间，从单细胞生物到多细胞生物，再到具有高度认知能力的智慧生命。
人类语言是一种高阶符号系统，包含了丰富的语法结构、上下文语境和常识逻辑。
通过自回归序列概率建模，神经网络能够学习字符之间的转移概率，从而实现连贯的文本生成与交流。
科学研究需要严谨的实验验证与理论推演。无论是物理控制、3D动力学仿真，还是语言理解，都需要扎实的数据与算法支撑。
在日常生活中，保持良好的作息与饮食习惯对于身心健康至关重要。合理的营养搭配能够为大脑提供充足的葡萄糖与能量。
人工智能的发展经历了符号主义、连接主义与行为主义的融合。现代大语言模型通过海量语料预训练掌握了广泛的世界知识。
形态发生图谱在硬实时控制中具备极高确定性，而语言模型则擅长抽象语义的理解与表达，两者结合构成了完整的智能闭环。
""".strip() * 30 # 扩增为训练文本流

# 构建字符词表
chars = sorted(list(set(TRAIN_CORPUS)))
vocab_size = len(chars)
char_to_ix = {ch: i for i, ch in enumerate(chars)}
ix_to_char = {i: ch for i, ch in enumerate(chars)}

# 2. 构建 10,000,000 细胞规模的自回归因果语言模型架构
class CellularCausalLanguageModel(nn.Module):
    def __init__(self, vocab_size, d_model=384, n_layers=6, n_heads=6, max_len=128):
        super().__init__()
        self.d_model = d_model
        self.tok_emb = nn.Embedding(vocab_size, d_model)
        self.pos_emb = nn.Parameter(torch.zeros(1, max_len, d_model))
        
        encoder_layer = nn.TransformerEncoderLayer(
            d_model=d_model, nhead=n_heads, dim_feedforward=d_model * 4,
            dropout=0.0, activation="gelu", batch_first=True
        )
        self.blocks = nn.TransformerEncoder(encoder_layer, num_layers=n_layers)
        self.head = nn.Linear(d_model, vocab_size, bias=False)
        self.max_len = max_len

    def forward(self, idx, targets=None):
        B, T = idx.size()
        x = self.tok_emb(idx) + self.pos_emb[:, :T, :]
        mask = torch.triu(torch.full((T, T), float("-inf"), device=idx.device), diagonal=1)
        x = self.blocks(x, mask=mask)
        logits = self.head(x)
        
        loss = None
        if targets is not None:
            loss = F.cross_entropy(logits.view(-1, logits.size(-1)), targets.view(-1))
        return logits, loss

    @torch.no_grad()
    def generate(self, start_text, max_new_tokens=30):
        self.eval()
        idx = torch.tensor([[char_to_ix.get(c, 0) for c in start_text]], dtype=torch.long, device=device)
        for _ in range(max_new_tokens):
            idx_cond = idx[:, -self.max_len:]
            logits, _ = self(idx_cond)
            next_logit = logits[:, -1, :]
            probs = F.softmax(next_logit, dim=-1)
            next_ix = torch.multinomial(probs, num_samples=1)
            idx = torch.cat((idx, next_ix), dim=1)
        res = "".join([ix_to_char[i] for i in idx[0].cpu().tolist()])
        self.train()
        return res

def run_language_evolution():
    print("\n" + "=" * 80)
    print("  🧬 千万级 (10,000,000 细胞/参数) 原生形态发生语言生成模型训练  🧬")
    print("=" * 80)
    
    model = CellularCausalLanguageModel(vocab_size=vocab_size).to(device)
    total_params = sum(p.numel() for p in model.parameters())
    print(f"• 词表大小 (Vocab Size):    {vocab_size} 个中文独立字符")
    print(f"• 神经元/参数总规模:        {total_params:,} (千万级语言模型！)")
    print(f"• 硬件加速设备:            {gpu_name}")
    print(f"• 训练优化目标:            Next-Token Prediction 交叉熵损失 (自回归序列演化)")
    print("=" * 80 + "\n")

    # 训练前测试：未训练状态下的语言生成效果
    print("🔍 [训练前阶段 0] 观察千万细胞在随机初始化状态下的生成文本：")
    raw_sample = model.generate("形态发生", max_new_tokens=25)
    print(f"  👉 随机状态生成: \"{raw_sample}\" (完全是杂乱无序的随机字符)\n")

    # 准备训练张量
    data_tensor = torch.tensor([char_to_ix[c] for c in TRAIN_CORPUS], dtype=torch.long, device=device)
    seq_len = 64
    batch_size = 32
    optimizer = torch.optim.AdamW(model.parameters(), lr=1e-3, weight_decay=1e-2)

    print("⚡ 正在向千万细胞注入真实中文语料与自回归梯度选择压...")
    steps = 150
    t0 = time.time()

    for step in range(1, steps + 1):
        # 随机采样训练批次
        ix = torch.randint(len(data_tensor) - seq_len - 1, (batch_size,))
        x = torch.stack([data_tensor[i:i+seq_len] for i in ix])
        y = torch.stack([data_tensor[i+1:i+seq_len+1] for i in ix])

        optimizer.zero_grad()
        logits, loss = model(x, y)
        loss.backward()
        optimizer.step()

        if step % 30 == 0 or step == steps:
            sample_text = model.generate("人类语言", max_new_tokens=20)
            elapsed = time.time() - t0
            print(f"Step [{step:3d}/{steps:3d}] | 语言交叉熵损失: {loss.item():.4f} | 耗时: {elapsed:.2f}s")
            print(f"  📝 当前学习生成的句子: \"{sample_text}\"")

    total_time = time.time() - t0
    print("\n" + "=" * 80)
    print(f"  🎉 千万级语言模型训练演化完成！总耗时: {total_time:.2f}s")
    print("=" * 80 + "\n")

    # 最终交互测试
    test_prompts = ["李龙飞先生", "形态发生", "在日常生活中", "人工智能"]
    print("📚 [最终生成效果测试] 千万级语言模型针对不同提示词的补全结果：")
    for p in test_prompts:
        ans = model.generate(p, max_new_tokens=25)
        print(f"• 输入: 「{p}」 -> 模型自发生成: 「{ans}」")

    # 保存模型
    os.makedirs("runs", exist_ok=True)
    save_path = "runs/cellular_language_model_10m.pt"
    torch.save(model.state_dict(), save_path)
    print(f"\n✓ 千万级原生语言模型已保存至: {save_path}")

if __name__ == "__main__":
    run_language_evolution()
