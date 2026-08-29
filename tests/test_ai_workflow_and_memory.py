import os
import unittest
from ai_service.memory import StrategyMemoryStore
from ai_service.workflow_pipeline import EvolutionWorkflowPipeline

class TestAIWorkflowAndMemory(unittest.TestCase):
    def setUp(self):
        self.test_db = "data/test_strategy_memory.db"
        if os.path.exists(self.test_db):
            os.remove(self.test_db)
        self.memory = StrategyMemoryStore(self.test_db)

    def tearDown(self):
        if os.path.exists(self.test_db):
            os.remove(self.test_db)

    def test_strategy_memory_record_and_query(self):
        # 记录一条成功经验
        self.memory.record_experience(
            strategy_name="DualMA_Trend_v1",
            symbol="rb2405",
            market_regime="TRENDING",
            parameters={"fast": 5, "slow": 20},
            sharpe_ratio=2.15,
            max_drawdown=0.025,
            win_rate=0.72,
            status="ACCEPTED",
            lessons_learned="顺大趋势开仓，结合 ATR 动态止损有效保护利润"
        )

        # 记录一条失败教训 (负向记忆)
        self.memory.record_experience(
            strategy_name="Scalper_Overfit_v0",
            symbol="rb2405",
            market_regime="TRENDING",
            parameters={"fast": 2, "slow": 5},
            sharpe_ratio=0.45,
            max_drawdown=0.12,
            win_rate=0.41,
            status="FAILED_DRAWDOWN",
            lessons_learned="超短均线在夜盘跳空时触发连续止损磨损，参数严重过拟合"
        )

        prompt_ctx = self.memory.query_memories_for_prompt("TRENDING")
        self.assertIn("历史成功案例", prompt_ctx)
        self.assertIn("DualMA_Trend_v1", prompt_ctx)
        self.assertIn("历史失败踩坑记录", prompt_ctx)
        self.assertIn("Scalper_Overfit_v0", prompt_ctx)

    def test_workflow_pipeline_execution(self):
        pipeline = EvolutionWorkflowPipeline(memory_store=self.memory)

        # 运行一次完整进化流水线
        res = pipeline.run_evolution_cycle(
            symbol="rb2405",
            market_features={"atr": 12.0, "trend_strength": 0.85},
            strategy_base_type="DualMA"
        )

        self.assertEqual(res["regime"], "TRENDING")
        self.assertEqual(res["verdict"], "ACCEPTED")
        self.assertTrue(res["memory_id"] > 0)
        self.assertIn("lessons_learned", res)

        # 验证记忆已被持久化到库中
        stats = self.memory.get_memory_stats()
        self.assertEqual(stats["total_memories"], 1)
        self.assertEqual(stats["accepted_count"], 1)

if __name__ == "__main__":
    unittest.main()
