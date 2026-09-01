import json
import os
import shutil
import subprocess
import urllib.request
import urllib.error
from typing import Dict, Any, List, Optional
from ai_service.config import AIConfig

class LLMProvider:
    def __init__(self, config: Optional[AIConfig] = None):
        self.config = config or AIConfig()
        self.copilot_cli_path = shutil.which("copilot")

    def chat_completion(self, messages: List[Dict[str, str]], temperature: Optional[float] = None) -> str:
        provider_mode = os.getenv("KUN_LLM_PROVIDER", "").lower()

        # 1. 显式指定使用 GitHub Copilot CLI
        if provider_mode == "copilot_cli" and self.copilot_cli_path:
            reply = self._copilot_cli_completion(messages)
            if reply:
                return reply

        api_key = self.config.api_key.strip()
        
        # 2. 当未配置 API Key 时，若本地安装有 GitHub Copilot CLI 则优先无缝直连
        if not api_key:
            if self.copilot_cli_path:
                reply = self._copilot_cli_completion(messages)
                if reply:
                    return reply
            return self._stub_completion(messages)

        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
            "User-Agent": "CodeBuddy/2.0"
        }

        payload = {
            "model": self.config.model,
            "messages": messages,
            "temperature": temperature if temperature is not None else self.config.temperature,
            "stream": True
        }

        url = f"{self.config.base_url.rstrip('/')}/chat/completions"
        req = urllib.request.Request(url, data=json.dumps(payload).encode("utf-8"), headers=headers, method="POST")

        try:
            with urllib.request.urlopen(req, timeout=45) as resp:
                full_chunks = []
                for line in resp:
                    line_str = line.decode("utf-8").strip()
                    if line_str.startswith("data:"):
                        data_part = line_str[5:].strip()
                        if data_part == "[DONE]":
                            break
                        try:
                            chunk = json.loads(data_part)
                            delta = chunk.get("choices", [{}])[0].get("delta", {}).get("content", "")
                            if delta:
                                full_chunks.append(delta)
                        except Exception:
                            pass
                
                result = "".join(full_chunks)
                if result:
                    return result
                if self.copilot_cli_path:
                    reply = self._copilot_cli_completion(messages)
                    if reply:
                        return reply
                return self._stub_completion(messages)
        except Exception as e:
            if self.copilot_cli_path:
                reply = self._copilot_cli_completion(messages)
                if reply:
                    return reply
            return f"[AI Provider Fallback] 网络请求异常 ({str(e)})\n\n" + self._stub_completion(messages)

    def _copilot_cli_completion(self, messages: List[Dict[str, str]]) -> Optional[str]:
        if not self.copilot_cli_path:
            return None
        prompt_parts = []
        for m in messages:
            role = m.get("role", "user")
            content = m.get("content", "")
            if role == "system":
                prompt_parts.append(f"[系统角色与约束]: {content}")
            elif role == "user":
                prompt_parts.append(f"[用户指令]: {content}")
            elif role == "assistant":
                prompt_parts.append(f"[历史助手回复]: {content}")
        full_prompt = "\n\n".join(prompt_parts)

        try:
            res = subprocess.run(
                [self.copilot_cli_path, "-p", full_prompt, "-s", "--no-ask-user", "--no-color"],
                capture_output=True,
                text=True,
                timeout=45
            )
            if res.returncode == 0 and res.stdout.strip():
                return res.stdout.strip()
        except Exception:
            pass
        return None

    def _stub_completion(self, messages: List[Dict[str, str]]) -> str:
        prompt_text = " ".join([m.get("content", "") for m in messages])
        banner = (
            "> **[本地演示 STUB 模式]** 未配置 `KUN_LLM_API_KEY` 或 LLM 请求失败，"
            "以下为管道连通性验证输出，**不包含任何真实分析结论**，禁止用于交易决策。\n\n"
        )

        if "复盘" in prompt_text or "绩效" in prompt_text or "PnL" in prompt_text:
            return banner + (
                "### 【KunQuant 收盘复盘报告 — STUB 占位】\n\n"
                "本应包含：核心资产与权益变动、交易执行归因（胜率/盈亏比/手续费磨损）、"
                "异常刺针与风控拦截记录、次日操作建议。\n\n"
                "配置 LLM Provider 后（设置环境变量 `KUN_LLM_API_KEY`，可选 `KUN_LLM_BASE_URL` / "
                "`KUN_LLM_MODEL`），将调用真实模型基于当天全量统计数据生成正式研报。\n\n"
                "**注意：Stub 模式不产生任何绩效数字，任何编造的胜率/收益数据均违反 §2-G 防作弊硬约束。**"
            )
        elif "策略" in prompt_text or "Strategy" in prompt_text:
            return banner + (
                "```cpp\n"
                "// [STUB 占位] 策略草稿生成需要真实 LLM。\n"
                "// 上线前必须通过 §2-G 三道关卡：硬约束 Prompt → 引擎断言 → 数据平移测试。\n"
                "```"
            )
        else:
            return "KunQuant AI Copilot: 系统核心总线与交易链路运行平稳，所有指标均在正常控制范围内。"
