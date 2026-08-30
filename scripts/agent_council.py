#!/usr/bin/env python3
"""
Three Departments & Six Ministries Council (三省六部联席议事中枢)
Enables peer collaboration between:
- Antigravity (中书省 · 首席架构与决策)
- CodeBuddy (门下省 · 代码审议与封驳)
- MiMoCode (尚书省 · 深度研判与全链路验证)
"""

import sys
import subprocess
import json
import argparse
import os
import concurrent.futures
from typing import Dict, Optional

def query_codebuddy(prompt: str, timeout: int = 90) -> str:
    """Invokes CodeBuddy non-interactively to perform peer review or task execution."""
    cmd = ["codebuddy", "-p", prompt]
    try:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout)
        if res.returncode == 0:
            return res.stdout.strip()
        else:
            return f"[CodeBuddy Error code {res.returncode}]: {res.stderr.strip()}"
    except subprocess.TimeoutExpired:
        return "[CodeBuddy Timeout]: Request exceeded timeout limit."
    except Exception as e:
        return f"[CodeBuddy Execution Exception]: {str(e)}"

def query_mimo(prompt: str, timeout: int = 90) -> str:
    """Invokes Xiaomi MiMo non-interactively."""
    cmd = ["mimo", "run", prompt, "--dangerously-skip-permissions"]
    try:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout)
        if res.returncode == 0:
            return res.stdout.strip()
        else:
            return f"[MiMo Error code {res.returncode}]: {res.stderr.strip()}"
    except subprocess.TimeoutExpired:
        return "[MiMo Timeout]: Request exceeded timeout limit."
    except Exception as e:
        return f"[MiMo Execution Exception]: {str(e)}"

def truncate_diff_cleanly(diff_text: str, max_chars: int = 5000) -> str:
    """Truncates diff cleanly at line boundaries with clear indication."""
    if len(diff_text) <= max_chars:
        return diff_text
    lines = diff_text[:max_chars].splitlines()
    truncated = "\n".join(lines[:-1])
    return f"{truncated}\n\n[... Diff truncated at line boundary for review context ...]"

def review_diff(git_diff_text: str, agent: str = "both") -> Dict[str, str]:
    clean_diff = truncate_diff_cleanly(git_diff_text)
    prompt = f"""【门下省/尚书省审议令】
请作为审议官，对以下中书省提交的工程代码 Diff 进行独立审查：
1. 是否存在内存泄漏、未定义行为、死锁或并发竞态风险？
2. 是否存在状态机死锁或边界条件处理疏漏？
3. 给出明确的“【封驳】(指出致命问题)”或“【可/准奏】(说明通过理由与注意事项)”结论。

=== CODE DIFF START ===
{clean_diff}
=== CODE DIFF END ===
"""
    tasks = {}
    if agent in ["codebuddy", "both"]:
        tasks["CodeBuddy (门下省)"] = query_codebuddy
    if agent in ["mimo", "both"]:
        tasks["MiMo (尚书省)"] = query_mimo

    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(tasks) or 1) as executor:
        future_map = {executor.submit(func, prompt): name for name, func in tasks.items()}
        for future in concurrent.futures.as_completed(future_map):
            name = future_map[future]
            try:
                results[name] = future.result()
            except Exception as e:
                results[name] = f"[Execution Error]: {e}"

    return results

def main():
    parser = argparse.ArgumentParser(description="Three Departments & Six Ministries Multi-Agent Council")
    parser.add_argument("--review-diff", action="store_true", help="Submit current git diff for Council review")
    parser.add_argument("--task", type=str, help="Dispatch custom task to agents")
    parser.add_argument("--file", type=str, help="Review specific file")
    parser.add_argument("--agent", choices=["codebuddy", "mimo", "both"], default="both", help="Target agent")
    args = parser.parse_args()

    if args.review_diff:
        diff_res = subprocess.run(["git", "diff", "HEAD~1"], stdout=subprocess.PIPE, text=True)
        diff_text = diff_res.stdout
        if not diff_text:
            diff_res = subprocess.run(["git", "diff"], stdout=subprocess.PIPE, text=True)
            diff_text = diff_res.stdout
        if not diff_text:
            print("No diff detected to review.")
            sys.exit(0)
        print(f">>> 联席议事中枢正在向 [{args.agent}] 呈递代码 Diff 审议...")
        opinions = review_diff(diff_text, agent=args.agent)
        for name, op in opinions.items():
            print(f"\n================ {name} 审议意见 ================")
            print(op)
            print("=" * 50)
    elif args.task:
        print(f">>> 派发任务至 [{args.agent}]: {args.task}")
        if args.agent in ["codebuddy", "both"]:
            print("\n--- CodeBuddy 响应 ---")
            print(query_codebuddy(args.task))
        if args.agent in ["mimo", "both"]:
            print("\n--- MiMo 响应 ---")
            print(query_mimo(args.task))
    elif args.file:
        if os.path.exists(args.file):
            with open(args.file, "r", encoding="utf-8") as f:
                content = f.read()
            prompt = f"【代码审查】请审查以下文件 ({args.file})：\n```\n{content[:4000]}\n```"
            print(f">>> 正在向 [{args.agent}] 提交文件审查: {args.file}...")
            if args.agent in ["codebuddy", "both"]:
                print("\n--- CodeBuddy 审议 ---")
                print(query_codebuddy(prompt))
            if args.agent in ["mimo", "both"]:
                print("\n--- MiMo 审议 ---")
                print(query_mimo(prompt))

if __name__ == "__main__":
    main()
