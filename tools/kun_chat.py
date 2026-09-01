#!/usr/bin/env python3
"""
kun_chat.py — 鲲 形态发生生命体终端交互式聊天窗口 (Terminal Chat TUI)
(Interactive Chat TUI for Morphogenetic Brains — Author: Li Longfei)
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from tools.kun_cellular_chat_engine import get_cellular_chat_engine


def run_chat():
    engine = get_cellular_chat_engine()
    print("=" * 70)
    print("  💬 鲲 · 形态发生计算生命体 交互式聊天窗口 (作者: 李龙飞) 💬")
    print("  • 支持数学、物理、智驾、量化、哲学与日常问答")
    print("  • 输入 'exit' 或 'quit' 退出聊天")
    print("=" * 70 + "\n")

    while True:
        try:
            prompt = input("👤 李龙飞 > ").strip()
            if not prompt:
                continue
            if prompt.lower() in ["exit", "quit", "q"]:
                print("\n👋 聊天窗口已关闭。")
                break
            
            print(f"\n🧠 鲲 (10,000,000 细胞形态发生超级大脑) >")
            reply = engine.generate_response(prompt)
            print(reply)
            print("-" * 70 + "\n")
        except (KeyboardInterrupt, EOFError):
            print("\n👋 聊天窗口已关闭。")
            break

if __name__ == "__main__":
    run_chat()
