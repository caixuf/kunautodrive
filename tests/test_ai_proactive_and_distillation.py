import os
import unittest
from ai_service.config import AIConfig
from ai_service.memory import StrategyMemoryStore
from ai_service.proactive_daemon import ProactiveAutonomousScheduler

class TestAIProactiveAndDistillation(unittest.TestCase):
    def setUp(self):
        self.test_db = "data/test_proactive_memory.db"
        if os.path.exists(self.test_db):
            os.remove(self.test_db)
        self.memory = StrategyMemoryStore(self.test_db)
        self.scheduler = ProactiveAutonomousScheduler(memory_store=self.memory)

    def tearDown(self):
        if os.path.exists(self.test_db):
            os.remove(self.test_db)

    def test_memory_distillation_and_rules(self):
        # 写入几条包含典型失败模式的经验
        self.memory.record_experience(
            strategy_name="Night_Scalper_v1",
            symbol="rb2405",
            market_regime="HIGH_VOLATILITY",
            parameters={"fast": 3, "slow": 10},
            sharpe_ratio=-0.5,
            max_drawdown=0.08,
            win_rate=0.35,
            status="FAILED_DRAWDOWN",
            lessons_learned="夜盘开盘跳空过大触发连续止损磨损"
        )
        self.memory.record_experience(
            strategy_name="Choppy_DualMA_v2",
            symbol="rb2405",
            market_regime="RANGING",
            parameters={"fast": 3, "slow": 8},
            sharpe_ratio=0.2,
            max_drawdown=0.06,
            win_rate=0.42,
            status="OVERFITTED",
            lessons_learned="震荡市中短均线频繁来回摩擦产生高额手续费"
        )

        # 触发规则蒸馏
        rules = self.memory.distill_memories_into_rules()
        self.assertTrue(len(rules) >= 2)

        # 验证提示词中是否已自动注入提炼后的高阶准则
        prompt_ctx = self.memory.query_memories_for_prompt("RANGING")
        self.assertIn("高阶蒸馏交易准则", prompt_ctx)

    def test_proactive_drawdown_event_trigger(self):
        # 模拟触发日内回撤 2.5% 预警
        advice = self.scheduler.on_drawdown_breach_event(
            account_id="acc_master_simnow",
            current_mdd_pct=2.5,
            context_info={"symbol": "rb2405", "pos_volume": 20.0, "reason": "连续3笔止损"}
        )
        self.assertIsInstance(advice, str)
        self.assertTrue(len(advice) > 20)

        # 验证风控处置已被沉淀到记忆库
        stats = self.memory.get_memory_stats()
        self.assertEqual(stats["rejected_count"], 1)

    def test_proactive_sensor_outlier_trigger(self):
        msg = self.scheduler.on_sensor_outlier_spike_event("rb2405", 8, 5.2)
        self.assertIn("传感器融合告警", msg)
        self.assertIn("已自动剔除异常尖刺源", msg)

    def test_proactive_shadow_promotion_trigger(self):
        msg = self.scheduler.on_shadow_promotion_ready_event("Candidate_Opt_v3", 2.45, 1.30)
        self.assertIn("AI 策略热切换升级申请", msg)
        self.assertIn("超越基准 1.2x", msg)

if __name__ == "__main__":
    unittest.main()
