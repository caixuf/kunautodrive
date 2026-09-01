#!/usr/bin/env python3
"""
Three Departments & Six Ministries Council (三省六部联席议事中枢)
Enables peer collaboration between:
- Antigravity (中书省 · 首席架构与决策)
- CodeBuddy (门下省 · 代码审议与封驳)
- MiMoCode (尚书省 · 深度研判与全链路验证)
- OpenCode (都察院 · 监察御史与坏味道排查)
- GitHub Copilot (枢密院/天策府 · 顶尖战略智库: 平时 5.6 Luna / 极难 Sol & Fable 5)
"""

import sys
import subprocess
import json
import argparse
import os
import shutil
import concurrent.futures
from typing import Dict, Optional

def resolve_workspace(explicit_workspace: Optional[str] = None) -> str:
    """
    解析并锚定目标工作区绝对路径 (防止 CWD 错位导致 AI 盲人摸象):
    1. 优先使用用户显式指定的 --workspace
    2. 其次锚定 agent_council.py 所在的主工程根目录
    3. 再次探测当前目录的 git toplevel
    """
    if explicit_workspace and os.path.exists(explicit_workspace):
        return os.path.abspath(explicit_workspace)

    script_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    if os.path.exists(os.path.join(script_root, "CMakeLists.txt")) or os.path.exists(os.path.join(script_root, ".git")):
        return script_root

    try:
        res = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, timeout=5)
        if res.returncode == 0 and res.stdout.strip():
            return os.path.abspath(res.stdout.strip())
    except Exception:
        pass

    return os.getcwd()

def query_codebuddy(prompt: str, cwd: str, timeout: int = 180) -> str:
    """Invokes CodeBuddy non-interactively with workspace anchoring and DEVNULL protection."""
    cmd = ["codebuddy", "-p", prompt]
    try:
        res = subprocess.run(
            cmd,
            cwd=cwd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout
        )
        if res.returncode == 0:
            return res.stdout.strip()
        else:
            return f"[CodeBuddy Error code {res.returncode}]: {res.stderr.strip() or res.stdout.strip()}"
    except subprocess.TimeoutExpired:
        return "[CodeBuddy Timeout]: Request exceeded timeout limit."
    except Exception as e:
        return f"[CodeBuddy Execution Exception]: {str(e)}"

def query_mimo(prompt: str, cwd: str, timeout: int = 180) -> str:
    """Invokes Xiaomi MiMo non-interactively with workspace anchoring and DEVNULL protection."""
    cmd = ["mimo", "run", prompt, "--dangerously-skip-permissions"]
    try:
        res = subprocess.run(
            cmd,
            cwd=cwd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout
        )
        if res.returncode == 0:
            return res.stdout.strip()
        else:
            return f"[MiMo Error code {res.returncode}]: {res.stderr.strip() or res.stdout.strip()}"
    except subprocess.TimeoutExpired:
        return "[MiMo Timeout]: Request exceeded timeout limit."
    except Exception as e:
        return f"[MiMo Execution Exception]: {str(e)}"

def query_opencode(prompt: str, cwd: str, timeout: int = 180) -> str:
    """Invokes OpenCode non-interactively with workspace anchoring and DEVNULL protection (都察院 · 监察御史)."""
    cmd = ["opencode", "run", prompt]
    try:
        res = subprocess.run(
            cmd,
            cwd=cwd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout
        )
        if res.returncode == 0:
            return res.stdout.strip()
        else:
            return f"[OpenCode Error code {res.returncode}]: {res.stderr.strip() or res.stdout.strip()}"
    except subprocess.TimeoutExpired:
        return "[OpenCode Timeout]: Request exceeded timeout limit."
    except Exception as e:
        return f"[OpenCode Execution Exception]: {str(e)}"

def select_copilot_model(prompt: str, model_override: Optional[str] = None, tier: str = "auto") -> str:
    """
    三省六部 Copilot 战力阶梯调度:
    - 默认/日常: gpt-5.6-luna (5.6 Luna - 极速、轻量、高响应)
    - 顶尖复杂数理/并发/死锁: gpt-5.6-sol (5.6 Sol - 极强数理逻辑与形式化推演)
    - 终极认知演化/复杂理论: claude-fable-5 (Fable 5 - 顶尖大模型思辨架构)
    """
    if model_override:
        return model_override
    if tier in ["sol", "apex"]:
        return "gpt-5.6-sol"
    if tier == "fable":
        return "claude-fable-5"
    if tier == "luna":
        return "gpt-5.6-luna"

    # 智能触发检测: 遇到顶尖高难问题自动召唤 Sol 或 Fable 5
    p_lower = prompt.lower()
    top_fable_keywords = ["心智涌现", "意识哲学", "大模型同构", "反事实心理推演", "形态发生终极收敛", "[fable]", "fable 5", "fable"]
    top_sol_keywords = ["并发死锁", "lock-free", "无锁队列", "严格形式化证明", "极端对抗", "硬核量化数学", "高维相空间", "false sharing", "[apex]", "[top_tier]", "sol"]

    for kw in top_fable_keywords:
        if kw in p_lower:
            return "claude-fable-5"
    for kw in top_sol_keywords:
        if kw in p_lower:
            return "gpt-5.6-sol"

    return "gpt-5.6-luna"

def query_copilot(prompt: str, cwd: str, model: Optional[str] = None, tier: str = "auto", timeout: int = 180) -> str:
    """Invokes GitHub Copilot CLI (枢密院/天策府 · 顶尖战略智库)."""
    copilot_path = shutil.which("copilot") or "/home/caixuf/.npm-global/bin/copilot"
    target_model = select_copilot_model(prompt, model_override=model, tier=tier)
    cmd = [
        copilot_path,
        "--model", target_model,
        "--add-dir", cwd,
        "--allow-all-paths",
        "-p", prompt,
        "-s",
        "--no-ask-user",
        "--no-color"
    ]
    try:
        res = subprocess.run(
            cmd,
            cwd=cwd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout
        )
        if res.returncode == 0 and res.stdout.strip():
            return f"【战力配置: {target_model} | 锚定战场: {cwd}】\n{res.stdout.strip()}"
        else:
            return f"[Copilot ({target_model}) Error code {res.returncode}]: {res.stderr.strip() or res.stdout.strip()}"
    except subprocess.TimeoutExpired:
        return f"[Copilot ({target_model}) Timeout]: Request exceeded timeout limit."
    except Exception as e:
        return f"[Copilot ({target_model}) Execution Exception]: {str(e)}"

def truncate_diff_cleanly(diff_text: str, max_chars: int = 6000) -> str:
    """Truncates diff cleanly at line boundaries with clear indication."""
    if len(diff_text) <= max_chars:
        return diff_text
    lines = diff_text[:max_chars].splitlines()
    truncated = "\n".join(lines[:-1])
    return f"{truncated}\n\n[... Diff truncated at line boundary for review context ...]"

def review_diff(git_diff_text: str, cwd: str, agent: str = "all", model: Optional[str] = None, tier: str = "auto") -> Dict[str, str]:
    clean_diff = truncate_diff_cleanly(git_diff_text)
    prompt = f"""【三省六部联席审议令 · 战场目录: {cwd}】
请作为审议官，对以下中书省提交的工程代码 Diff 进行独立审查：
1. 是否存在内存泄漏、未定义行为、死锁或并发竞态风险？
2. 是否存在状态机死锁、边界条件疏漏或虚假共享？
3. 给出明确的“【封驳】(指出致命问题)”或“【可/准奏】(说明通过理由与注意事项)”结论。

=== CODE DIFF START ===
{clean_diff}
=== CODE DIFF END ===
"""
    tasks = {}
    if agent in ["codebuddy", "all"]:
        tasks["CodeBuddy (门下省)"] = lambda p=prompt: query_codebuddy(p, cwd=cwd)
    if agent in ["mimo", "all"]:
        tasks["MiMo (尚书省)"] = lambda p=prompt: query_mimo(p, cwd=cwd)
    if agent in ["opencode", "all"]:
        tasks["OpenCode (都察院)"] = lambda p=prompt: query_opencode(p, cwd=cwd)
    if agent in ["copilot", "all"]:
        tasks["GitHub Copilot (枢密院/天策府)"] = lambda p=prompt: query_copilot(p, cwd=cwd, model=model, tier=tier)

    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(tasks) or 1) as executor:
        future_map = {executor.submit(func): name for name, func in tasks.items()}
        for future in concurrent.futures.as_completed(future_map):
            name = future_map[future]
            try:
                results[name] = future.result()
            except Exception as e:
                results[name] = f"[Execution Error]: {e}"

    return results

def main():
    parser = argparse.ArgumentParser(description="Three Departments & Six Ministries Multi-Agent Council (三省六部联席议事中枢)")
    parser.add_argument("--review-diff", action="store_true", help="Submit current git diff for Council review")
    parser.add_argument("--task", type=str, help="Dispatch custom task to agents")
    parser.add_argument("--file", type=str, help="Review specific file")
    parser.add_argument("--agent", choices=["codebuddy", "mimo", "opencode", "copilot", "all", "both"], default="all", help="Target agent")
    parser.add_argument("--workspace", "-w", type=str, default=None, help="Target workspace directory (default: auto-detected repository root)")
    parser.add_argument("--tier", choices=["auto", "luna", "sol", "fable", "apex"], default="auto", help="Copilot combat tier (default: auto -> luna, apex problems -> sol/fable)")
    parser.add_argument("--model", type=str, help="Explicit model override for Copilot (e.g. gpt-5.6-luna, gpt-5.6-sol, claude-fable-5)")
    args = parser.parse_args()

    workspace = resolve_workspace(args.workspace)
    print(f"🏛️  【三省六部联席中枢】锚定战场目录 (Workspace): {workspace}")

    if args.review_diff:
        diff_res = subprocess.run(["git", "diff", "HEAD~1"], cwd=workspace, stdout=subprocess.PIPE, text=True)
        diff_text = diff_res.stdout
        if not diff_text:
            diff_res = subprocess.run(["git", "diff"], cwd=workspace, stdout=subprocess.PIPE, text=True)
            diff_text = diff_res.stdout
        if not diff_text:
            print("No diff detected to review in workspace:", workspace)
            sys.exit(0)
        print(f">>> 联席议事中枢正在向 [{args.agent}] (Copilot阶梯: {args.tier}) 呈递代码 Diff 审议...")
        opinions = review_diff(diff_text, cwd=workspace, agent=args.agent, model=args.model, tier=args.tier)
        for name, op in opinions.items():
            print(f"\n================ {name} 审议意见 ================")
            print(op)
            print("=" * 50)
    elif args.task:
        print(f">>> 派发任务至 [{args.agent}] (Copilot阶梯: {args.tier}): {args.task}")
        tasks = {}
        if args.agent in ["codebuddy", "all", "both"]:
            tasks["CodeBuddy (门下省)"] = lambda p=args.task: query_codebuddy(p, cwd=workspace)
        if args.agent in ["mimo", "all", "both"]:
            tasks["MiMo (尚书省)"] = lambda p=args.task: query_mimo(p, cwd=workspace)
        if args.agent in ["opencode", "all"]:
            tasks["OpenCode (都察院)"] = lambda p=args.task: query_opencode(p, cwd=workspace)
        if args.agent in ["copilot", "all"]:
            tasks["GitHub Copilot (枢密院/天策府)"] = lambda p=args.task: query_copilot(p, cwd=workspace, model=args.model, tier=args.tier)

        with concurrent.futures.ThreadPoolExecutor(max_workers=len(tasks) or 1) as executor:
            future_map = {executor.submit(func): name for name, func in tasks.items()}
            for future in concurrent.futures.as_completed(future_map):
                name = future_map[future]
                try:
                    res = future.result()
                except Exception as e:
                    res = f"[Execution Error]: {e}"
                print(f"\n================ {name} 响应 ================")
                print(res)
                print("=" * 50)
    elif args.file:
        file_path = args.file if os.path.isabs(args.file) else os.path.join(workspace, args.file)
        if os.path.exists(file_path):
            with open(file_path, "r", encoding="utf-8") as f:
                content = f.read()
            prompt = f"【代码审查 · 战场: {workspace}】请审查以下文件 ({file_path})：\n```\n{content[:5000]}\n```"
            print(f">>> 正在向 [{args.agent}] 提交文件审查: {file_path}...")
            tasks = {}
            if args.agent in ["codebuddy", "all", "both"]:
                tasks["CodeBuddy (门下省)"] = lambda p=prompt: query_codebuddy(p, cwd=workspace)
            if args.agent in ["mimo", "all", "both"]:
                tasks["MiMo (尚书省)"] = lambda p=prompt: query_mimo(p, cwd=workspace)
            if args.agent in ["opencode", "all"]:
                tasks["OpenCode (都察院)"] = lambda p=prompt: query_opencode(p, cwd=workspace)
            if args.agent in ["copilot", "all"]:
                tasks["GitHub Copilot (枢密院/天策府)"] = lambda p=prompt: query_copilot(p, cwd=workspace, model=args.model, tier=args.tier)

            with concurrent.futures.ThreadPoolExecutor(max_workers=len(tasks) or 1) as executor:
                future_map = {executor.submit(func): name for name, func in tasks.items()}
                for future in concurrent.futures.as_completed(future_map):
                    name = future_map[future]
                    try:
                        res = future.result()
                    except Exception as e:
                        res = f"[Execution Error]: {e}"
                    print(f"\n================ {name} 审议意见 ================")
                    print(res)
                    print("=" * 50)

if __name__ == "__main__":
    main()

