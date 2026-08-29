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

    def test_focus_tracker_add_list_remove(self):
        import tempfile
        from ai_service.focus_tracker import FocusProductTracker

        tmp = tempfile.mkdtemp()
        cfg = AIConfig(
            engine_db_path=os.path.join(tmp, "engine.db"),
            focus_state_path=os.path.join(tmp, "focus.json"),
            tasks_state_path=os.path.join(tmp, "tasks.json"),
        )
        tracker = FocusProductTracker(cfg)

        # 1. 添加专项跟踪
        res = tracker.add_focus("au2406", "黄金专项分析", "futures")
        self.assertTrue(res["ok"])
        self.assertFalse(res["snapshot"]["available"])  # 尚无数据

        # 重复添加被拒
        self.assertFalse(tracker.add_focus("au2406")["ok"])

        # 2. 写入模拟 tick 后快照可用
        import sqlite3
        with sqlite3.connect(cfg.engine_db_path) as conn:
            conn.execute("""CREATE TABLE ticks (
                id INTEGER PRIMARY KEY AUTOINCREMENT, symbol TEXT, exchange TEXT,
                last_price REAL, bid_price1 REAL, ask_price1 REAL, bid_volume1 REAL,
                ask_volume1 REAL, volume REAL, open_interest REAL, ts INTEGER NOT NULL)""")
            base_ts = 1788000000000000
            for i in range(100):
                conn.execute(
                    "INSERT INTO ticks (symbol, exchange, last_price, ts) VALUES ('au2406','SHFE',?,?)",
                    (568.0 + i * 0.05, base_ts + i * 200000))
        snap = tracker.snapshot("au2406")
        self.assertTrue(snap["available"])
        self.assertEqual(snap["tick_count"], 100)
        self.assertAlmostEqual(snap["last_price"], 568.0 + 99 * 0.05, places=4)
        self.assertGreater(snap["change_pct"], 0)

        # 3. 任务创建与执行 (price_summary)
        res = tracker.create_task("au2406", "price_summary", "黄金日线摘要")
        self.assertTrue(res["ok"])
        task_id = res["task"]["id"]
        run_res = tracker.run_task(task_id)
        self.assertTrue(run_res["ok"])
        self.assertEqual(run_res["task"]["status"], "done")
        self.assertEqual(run_res["task"]["result"]["summary"]["tick_count"], 100)

        # 4. 非法任务类型被拒
        self.assertFalse(tracker.create_task("au2406", "hack_the_planet")["ok"])

        # 5. LLM 回复中的 ```task``` 块自动解析生成任务
        reply = "好的，我来跟踪。\n```task {\"symbol\": \"au2406\", \"type\": \"volatility_report\", \"description\": \"黄金波动率\"}```"
        created = tracker.extract_and_create_tasks(reply)
        self.assertEqual(len(created), 1)
        self.assertEqual(created[0]["type"], "volatility_report")
        run2 = tracker.run_task(created[0]["id"])
        self.assertTrue(run2["ok"])
        self.assertGreater(run2["task"]["result"]["annualized_vol_pct"], 0)

        # 6. 移除跟踪
        self.assertTrue(tracker.remove_focus("au2406")["ok"])
        self.assertEqual(len(tracker.list_focus()), 0)

    def test_focus_chat_context_injection(self):
        from ai_service.focus_tracker import FocusProductTracker
        tracker = FocusProductTracker()
        ctx = tracker.focus_context_for_chat()
        self.assertIn("专项跟踪产品", ctx)

if __name__ == "__main__":
    unittest.main()
