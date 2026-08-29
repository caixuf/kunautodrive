import sqlite3
import json
import os
from typing import List, Dict, Any, Optional
from datetime import datetime, timedelta

class StrategyMemoryStore:
    """
    量化策略长期演进记忆库与规则蒸馏器 (Strategy Evolutionary Memory Store & Distillation Engine)
    
    核心特性：
    1. 经验沉淀：记录不同市场形态 (趋势/震荡/高波动) 下的成功与失败经验
    2. 规则蒸馏 (Memory Distillation)：从分散琐碎的交易记忆中自动归纳高阶交易规则
    3. 记忆衰减与修剪 (Pruning & Decay)：定期清理陈旧与低信噪比记忆，防止 Context 膨胀
    """
    def __init__(self, db_path: str = "data/strategy_memory.db"):
        self.db_path = db_path
        os.makedirs(os.path.dirname(db_path) if os.path.dirname(db_path) else ".", exist_ok=True)
        self._init_db()

    def _init_db(self):
        with sqlite3.connect(self.db_path) as conn:
            conn.execute("""
                CREATE TABLE IF NOT EXISTS strategy_memories (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    strategy_name TEXT NOT NULL,
                    symbol TEXT NOT NULL,
                    market_regime TEXT NOT NULL,  -- TRENDING, RANGING, HIGH_VOLATILITY
                    parameters_json TEXT NOT NULL,
                    sharpe_ratio REAL,
                    max_drawdown REAL,
                    win_rate REAL,
                    status TEXT NOT NULL,        -- ACCEPTED, OVERFITTED, REJECTED_CHEAT, FAILED_DRAWDOWN
                    lessons_learned TEXT,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                )
            """)
            # 高阶蒸馏规则表
            conn.execute("""
                CREATE TABLE IF NOT EXISTS distilled_rules (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    rule_category TEXT NOT NULL,  -- RISK_CONTROL, TIMING, PARAMETER_PLATEAU, REGIME_FIT
                    rule_statement TEXT NOT NULL UNIQUE,
                    confidence_score REAL DEFAULT 0.85,
                    evidence_count INTEGER DEFAULT 1,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                )
            """)
            conn.commit()

    def record_experience(
        self,
        strategy_name: str,
        symbol: str,
        market_regime: str,
        parameters: Dict[str, Any],
        sharpe_ratio: float,
        max_drawdown: float,
        win_rate: float,
        status: str,
        lessons_learned: str
    ) -> int:
        with sqlite3.connect(self.db_path) as conn:
            cursor = conn.cursor()
            cursor.execute("""
                INSERT INTO strategy_memories 
                (strategy_name, symbol, market_regime, parameters_json, sharpe_ratio, max_drawdown, win_rate, status, lessons_learned)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            """, (
                strategy_name,
                symbol,
                market_regime,
                json.dumps(parameters, ensure_ascii=False),
                sharpe_ratio,
                max_drawdown,
                win_rate,
                status,
                lessons_learned
            ))
            conn.commit()
            return cursor.lastrowid

    def query_memories_for_prompt(self, market_regime: str, limit: int = 5) -> str:
        """
        提取指定市场形态下的成功经验、失败教训以及高阶蒸馏规则
        """
        with sqlite3.connect(self.db_path) as conn:
            conn.row_factory = sqlite3.Row
            cursor = conn.cursor()

            # 1. 成功经验
            cursor.execute("""
                SELECT strategy_name, parameters_json, sharpe_ratio, max_drawdown, win_rate, lessons_learned
                FROM strategy_memories
                WHERE market_regime = ? AND status = 'ACCEPTED'
                ORDER BY sharpe_ratio DESC LIMIT ?
            """, (market_regime, limit))
            successes = cursor.fetchall()

            # 2. 失败教训
            cursor.execute("""
                SELECT strategy_name, parameters_json, status, lessons_learned
                FROM strategy_memories
                WHERE status IN ('OVERFITTED', 'REJECTED_CHEAT', 'FAILED_DRAWDOWN')
                ORDER BY id DESC LIMIT ?
            """, (limit,))
            failures = cursor.fetchall()

            # 3. 高阶蒸馏规则
            cursor.execute("""
                SELECT rule_statement, confidence_score FROM distilled_rules
                ORDER BY confidence_score DESC LIMIT 3
            """)
            rules = cursor.fetchall()

        context_lines = [f"### 【长期进化记忆库参考 (针对当前 {market_regime} 形态)】"]

        if rules:
            context_lines.append("\n**高阶蒸馏交易准则 (置信度 > 85%)：**")
            for r in rules:
                context_lines.append(f"- [准则] {r['rule_statement']} (置信度: {r['confidence_score'] * 100:.0f}%)")

        if successes:
            context_lines.append("\n**历史成功案例 (可参考借鉴)：**")
            for s in successes:
                context_lines.append(
                    f"- 策略: {s['strategy_name']} | 夏普: {s['sharpe_ratio']:.2f} | 胜率: {s['win_rate'] * 100:.1f}% | 参数: {s['parameters_json']}\n"
                    f"  成功归因: {s['lessons_learned']}"
                )
        else:
            context_lines.append("\n**历史成功案例：** 暂无完全匹配记录，基于基准均线模型冷启动。")

        if failures:
            context_lines.append("\n**历史失败踩坑记录 (严禁重蹈覆辙)：**")
            for f in failures:
                context_lines.append(
                    f"- 策略: {f['strategy_name']} | 拦截状态: {f['status']}\n"
                    f"  失败教训: {f['lessons_learned']}"
                )

        return "\n".join(context_lines)

    def distill_memories_into_rules(self) -> List[str]:
        """
        记忆蒸馏：从零散记忆中归纳出结构化高阶准则并去重入库
        """
        new_rules = []
        with sqlite3.connect(self.db_path) as conn:
            conn.row_factory = sqlite3.Row
            cursor = conn.cursor()

            # 统计失败原因
            cursor.execute("SELECT lessons_learned FROM strategy_memories WHERE status != 'ACCEPTED'")
            failures = cursor.fetchall()

            for f in failures:
                lesson = f['lessons_learned']
                if "夜盘" in lesson or "跳空" in lesson:
                    new_rules.append(("RISK_CONTROL", "夜盘开盘前 15 分钟跳空剧烈时段严禁高频追单，止损应放大至 2.5x ATR"))
                elif "震荡" in lesson or "手续费" in lesson:
                    new_rules.append(("REGIME_FIT", "震荡行情下快线周期不得小于 10，防止假突破来回摩擦产生高额手续费"))
                elif "孤峰" in lesson or "过拟合" in lesson:
                    new_rules.append(("PARAMETER_PLATEAU", "策略必须通过邻域参数平原检验，单点孤峰最优解一律判定为伪拟合拒绝上线"))

            # 写入蒸馏规则表 (去重并累加证据计数)
            for cat, stmt in new_rules:
                cursor.execute("""
                    INSERT INTO distilled_rules (rule_category, rule_statement, confidence_score, evidence_count)
                    VALUES (?, ?, 0.90, 1)
                    ON CONFLICT(rule_statement) DO UPDATE SET 
                        evidence_count = evidence_count + 1,
                        confidence_score = MIN(0.99, confidence_score + 0.02),
                        updated_at = CURRENT_TIMESTAMP
                """, (cat, stmt))
            conn.commit()

        return [stmt for _, stmt in new_rules]

    def prune_outdated_memories(self, max_records_per_regime: int = 50) -> int:
        """
        记忆修剪：淘汰冗余、过早且低评分的旧记忆，保持记忆库高信噪比
        """
        deleted_count = 0
        with sqlite3.connect(self.db_path) as conn:
            cursor = conn.cursor()
            for regime in ["TRENDING", "RANGING", "HIGH_VOLATILITY"]:
                cursor.execute("""
                    DELETE FROM strategy_memories 
                    WHERE id NOT IN (
                        SELECT id FROM strategy_memories 
                        WHERE market_regime = ? 
                        ORDER BY sharpe_ratio DESC, id DESC LIMIT ?
                    ) AND market_regime = ?
                """, (regime, max_records_per_regime, regime))
                deleted_count += cursor.rowcount
            conn.commit()
        return deleted_count

    def get_memory_stats(self) -> Dict[str, Any]:
        with sqlite3.connect(self.db_path) as conn:
            cursor = conn.cursor()
            cursor.execute("SELECT COUNT(*) FROM strategy_memories")
            total = cursor.fetchone()[0]
            cursor.execute("SELECT COUNT(*) FROM strategy_memories WHERE status = 'ACCEPTED'")
            accepted = cursor.fetchone()[0]
            cursor.execute("SELECT COUNT(*) FROM strategy_memories WHERE status != 'ACCEPTED'")
            rejected = cursor.fetchone()[0]
            cursor.execute("SELECT COUNT(*) FROM distilled_rules")
            rules_count = cursor.fetchone()[0]
            return {
                "total_memories": total,
                "accepted_count": accepted,
                "rejected_count": rejected,
                "distilled_rules_count": rules_count
            }
