import os
from dataclasses import dataclass

def _load_env_file():
    """从本地 .env 文件加载环境变量 (支持注释与双引号，安全隔离不入库)"""
    env_paths = [".env", ".env.local"]
    for path in env_paths:
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#") or "=" not in line:
                        continue
                    k, v = line.split("=", 1)
                    k = k.strip()
                    v = v.strip().strip("'\"")
                    if k and k not in os.environ:
                        os.environ[k] = v

_load_env_file()

@dataclass
class AIConfig:
    api_key: str = os.getenv("KUN_LLM_API_KEY", "")
    base_url: str = os.getenv("KUN_LLM_BASE_URL", "https://copilot.tencent.com/v2")
    model: str = os.getenv("KUN_LLM_MODEL", "deepseek-v3")
    temperature: float = 0.2
    port: int = int(os.getenv("KUN_AI_PORT", "8901"))
    use_stub_when_no_key: bool = True
    schedule_interval_sec: int = int(os.getenv("KUN_AI_SCHEDULE_INTERVAL_SEC", "1200"))

    # 专项产品跟踪 (Focus Tracker) 数据源与状态持久化
    engine_db_path: str = os.getenv("KUN_ENGINE_DB_PATH", "data/kun_quant.db")
    focus_state_path: str = os.getenv("KUN_FOCUS_STATE_PATH", "data/ai_focus.json")
    tasks_state_path: str = os.getenv("KUN_TASKS_STATE_PATH", "data/ai_tasks.json")

    # 云端 Background Agent 沙箱服务配置
    bg_agent_base_url: str = os.getenv("KUN_BG_AGENT_BASE_URL", "http://localhost:8080")
    bg_agent_sandbox_type: str = os.getenv("KUN_BG_SANDBOX_TYPE", "cvm")
    bg_agent_repo: str = os.getenv("KUN_BG_REPO", "https://github.com/user/KunAutoDrive")
    bg_agent_branch: str = os.getenv("KUN_BG_BRANCH", "main")
    bg_agent_token: str = os.getenv("KUN_BG_TOKEN", "")
