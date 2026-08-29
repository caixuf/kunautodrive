import os
import unittest
from ai_service.config import AIConfig
from ai_service.provider import LLMProvider
from ai_service.daily_reporter import DailyReporter
from ai_service.background_agent_client import BackgroundAgentClient

class TestAIService(unittest.TestCase):
    def test_config_defaults(self):
        cfg = AIConfig()
        self.assertEqual(cfg.port, 8901)
        self.assertTrue(cfg.use_stub_when_no_key)
        self.assertEqual(cfg.bg_agent_sandbox_type, "cvm")

    def test_provider_stub_fallback(self):
        cfg = AIConfig(api_key="")
        provider = LLMProvider(cfg)
        reply = provider.chat_completion([{"role": "user", "content": "生成今日交易复盘报告"}])
        self.assertIn("KunQuant 收盘复盘报告", reply)

    def test_daily_reporter_generation(self):
        reporter = DailyReporter()
        mock_data = {
            "account_id": "acc_master_simnow",
            "starting_equity": 1000000.0,
            "ending_equity": 1024580.0,
            "net_pnl": 24580.0,
            "win_rate": 0.765,
            "profit_factor": 2.84,
            "max_drawdown": 0.0015,
            "trades_count": 34
        }
        report = reporter.generate_report(mock_data, "2026-08-30")
        self.assertIsInstance(report, str)
        self.assertTrue(len(report) > 50)
        
        # 验证报告文件是否自动落盘
        saved_file = "runs/reports/daily_report_2026-08-30_acc_master_simnow.md"
        self.assertTrue(os.path.exists(saved_file))

    def test_strategy_gen_constraints_exist(self):
        prompt_file = "kun_quant/prompts/strategy_gen_constraints.md"
        self.assertTrue(os.path.exists(prompt_file))
        with open(prompt_file, "r", encoding="utf-8") as f:
            content = f.read()
        self.assertIn("严禁未来函数", content)
        self.assertIn("禁止过拟合魔法参数", content)
        self.assertIn("禁止忽略交易成本", content)

    def test_background_agent_client_dispatch(self):
        client = BackgroundAgentClient()
        res = client.create_agent(
            prompt="帮我重构这个项目的代码结构",
            session_id="session_xyz789",
            agent_title="代码重构助手",
            sandbox_type="cvm",
            repo_config={
                "repo": "https://github.com/user/KunAutoDrive",
                "branch": "main",
                "token": "test_token",
                "provider": "github"
            }
        )
        self.assertTrue(res.get("success"))
        self.assertEqual(res.get("session_id"), "session_xyz789")

    def test_background_agent_strategy_task(self):
        client = BackgroundAgentClient()
        res = client.dispatch_strategy_generator_task(
            strategy_requirement="编写螺纹钢 5 分钟均线突破策略",
            session_id="session_strat_001"
        )
        self.assertTrue(res.get("success"))
        self.assertEqual(res.get("session_id"), "session_strat_001")
        payload = res.get("request_payload", {})
        self.assertIn("严禁未来函数", payload.get("prompt", ""))

if __name__ == "__main__":
    unittest.main()
