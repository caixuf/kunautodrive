#!/usr/bin/env python3
"""
Three Departments & Six Ministries Council (三省六部联席议事中枢)
Enables peer collaboration between Antigravity (中书省) and CodeBuddy (门下省)
"""

import sys
import subprocess
import json
import argparse
import os

def query_codebuddy(prompt: str, timeout: int = 60) -> str:
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

def review_diff(git_diff_text: str) -> str:
    prompt = f"""【门下省奉敕审议令】
请作为门下省给事中/审议官，对以下中书省提交的工程代码 Diff 进行严肃独立审查：
1. 是否存在内存泄漏、未定义行为、死锁或并发竞态风险？
2. 是否存在状态机死锁或边界条件处理疏漏？
3. 给出明确的“【封驳】(指出致命问题)”或“【可/准奏】(说明通过理由与注意事项)”结论。

=== CODE DIFF START ===
{git_diff_text}
=== CODE DIFF END ===
"""
    return query_codebuddy(prompt)

def main():
    parser = argparse.ArgumentParser(description="Three Departments & Six Ministries Multi-Agent Council")
    parser.add_argument("--review-diff", action="store_true", help="Submit current git diff for CodeBuddy review")
    parser.add_argument("--task", type=str, help="Dispatch custom task to CodeBuddy")
    parser.add_argument("--file", type=str, help="Review specific file")
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
        print(">>> 门下省 (CodeBuddy) 正在审议代码 Diff...")
        opinion = review_diff(diff_text[:6000])
        print("\n================ 门下省审议意见 ================")
        print(opinion)
        print("================================================")
    elif args.task:
        print(f">>> 派发任务至 CodeBuddy: {args.task}")
        resp = query_codebuddy(args.task)
        print(resp)
    elif args.file:
        if os.path.exists(args.file):
            with open(args.file, "r", encoding="utf-8") as f:
                content = f.read()
            prompt = f"【门下省代码审查】请审查以下文件 ({args.file})：\n```\n{content[:5000]}\n```"
            print(f">>> 门下省 (CodeBuddy) 正在审查文件: {args.file}...")
            resp = query_codebuddy(prompt)
            print("\n================ 门下省审议意见 ================")
            print(resp)
            print("================================================")

if __name__ == "__main__":
    main()
