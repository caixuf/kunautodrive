"""
专项产品跟踪与 AI 任务生成引擎 (Focus Product Tracker & AI Task Generator)

功能：
1. 专项跟踪：用户指定重点产品（如黄金 au2406），自动从 C++ 引擎 SQLite 行情库
   提取该产品的最新统计快照（最新价、涨跌、高低点、数据新鲜度）
2. AI 聊天生成任务：与 AI 对话时自动注入跟踪产品上下文；AI 回复中可输出
   ```task {"symbol": "...", "type": "...", "description": "..."} ``` 结构化任务块，
   自动解析并创建分析任务
3. 任务执行（本地确定性计算，不依赖 LLM Key）：
   - price_summary    价格统计摘要（笔数/最新价/涨跌/高低）
   - volatility_report 波动率报告（分钟级收益波动 + 年化估算 + 平均振幅）
   - llm_analysis     LLM 深度分析（喂入数据摘要，无 Key 时降级为 stub）
"""

import json
import os
import re
import sqlite3
from datetime import datetime, timezone
from typing import Dict, Any, List, Optional

from ai_service.config import AIConfig
from ai_service.provider import LLMProvider

SUPPORTED_TASK_TYPES = {"price_summary", "volatility_report", "llm_analysis"}

_TASK_BLOCK_RE = re.compile(r"```task\s*(\{.*?\})\s*```", re.DOTALL)


class FocusProductTracker:
    def __init__(self, config: Optional[AIConfig] = None, provider: Optional[LLMProvider] = None):
        self.config = config or AIConfig()
        self.provider = provider or LLMProvider(self.config)
        self._focus = self._load_json(self.config.focus_state_path, [])
        self._tasks = self._load_json(self.config.tasks_state_path, [])
        self._next_task_id = max([t["id"] for t in self._tasks], default=0) + 1

    # ────────────────────────── 状态持久化 ──────────────────────────

    @staticmethod
    def _load_json(path: str, default):
        try:
            if os.path.exists(path):
                with open(path, "r", encoding="utf-8") as f:
                    return json.load(f)
        except Exception:
            pass
        return default

    @staticmethod
    def _save_json(path: str, data):
        os.makedirs(os.path.dirname(path) if os.path.dirname(path) else ".", exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)

    def _save_focus(self):
        self._save_json(self.config.focus_state_path, self._focus)

    def _save_tasks(self):
        self._save_json(self.config.tasks_state_path, self._tasks)

    # ────────────────────────── 专项跟踪 ──────────────────────────

    def add_focus(self, symbol: str, note: str = "", category: str = "futures") -> Dict[str, Any]:
        symbol = symbol.strip()
        if not symbol:
            return {"ok": False, "error": "symbol 不能为空"}
        if any(f["symbol"] == symbol for f in self._focus):
            return {"ok": False, "error": f"{symbol} 已在跟踪列表中"}
        entry = {
            "symbol": symbol,
            "note": note,
            "category": category,
            "added_at": datetime.now(timezone.utc).isoformat(),
        }
        self._focus.append(entry)
        self._save_focus()
        return {"ok": True, "focus": entry, "snapshot": self.snapshot(symbol)}

    def remove_focus(self, symbol: str) -> Dict[str, Any]:
        before = len(self._focus)
        self._focus = [f for f in self._focus if f["symbol"] != symbol]
        self._save_focus()
        return {"ok": len(self._focus) < before, "remaining": len(self._focus)}

    def list_focus(self) -> List[Dict[str, Any]]:
        result = []
        for f in self._focus:
            item = dict(f)
            item["snapshot"] = self.snapshot(f["symbol"])
            result.append(item)
        return result

    # ────────────────────────── 行情快照 (读引擎 SQLite) ──────────────────────────

    def snapshot(self, symbol: str) -> Dict[str, Any]:
        """从引擎 ticks 表提取该产品统计快照。无数据时返回 stale 提示。"""
        db_path = self.config.engine_db_path
        if not os.path.exists(db_path):
            return {"symbol": symbol, "available": False, "reason": f"行情库不存在: {db_path}"}
        try:
            with sqlite3.connect(f"file:{db_path}?mode=ro", uri=True) as conn:
                row = conn.execute(
                    """SELECT COUNT(*), MIN(ts), MAX(ts), MIN(last_price), MAX(last_price),
                              (SELECT last_price FROM ticks WHERE symbol = ? ORDER BY id DESC LIMIT 1)
                       FROM ticks WHERE symbol = ?""",
                    (symbol, symbol),
                ).fetchone()
                first_price_row = conn.execute(
                    "SELECT last_price FROM ticks WHERE symbol = ? ORDER BY id ASC LIMIT 1",
                    (symbol,),
                ).fetchone()
        except sqlite3.Error as e:
            return {"symbol": symbol, "available": False, "reason": f"行情库读取失败: {e}"}

        count, first_ts, last_ts, lo, hi, last_price = row
        if not count:
            return {
                "symbol": symbol,
                "available": False,
                "reason": "暂无落盘 tick（确认引擎 recorder 已启动且该合约有行情源）",
            }

        first_price = first_price_row[0] if first_price_row else last_price

        chg_pct = ((last_price - first_price) / first_price * 100.0) if first_price else 0.0
        stale_min = (datetime.now(timezone.utc).timestamp() * 1e6 - last_ts) / 60e6
        return {
            "symbol": symbol,
            "available": True,
            "tick_count": count,
            "first_ts": first_ts,
            "last_ts": last_ts,
            "last_price": round(last_price, 4),
            "change_pct": round(chg_pct, 3),
            "low": round(lo, 4),
            "high": round(hi, 4),
            "stale_minutes": round(stale_min, 1),
        }

    def focus_context_for_chat(self) -> str:
        """生成注入聊天 system prompt 的跟踪产品上下文摘要"""
        if not self._focus:
            return "（当前没有专项跟踪产品）"
        lines = ["当前专项跟踪产品列表："]
        for f in self.list_focus():
            s = f["snapshot"]
            if s.get("available"):
                lines.append(
                    f"- {s['symbol']}（{f['category']}）：最新 {s['last_price']}，"
                    f"区间涨跌 {s['change_pct']}%，高/低 {s['high']}/{s['low']}，"
                    f"落盘 {s['tick_count']} 条，数据新鲜度 {s['stale_minutes']} 分钟。备注: {f['note'] or '无'}"
                )
            else:
                lines.append(f"- {s['symbol']}（{f['category']}）：无可用行情（{s.get('reason')}）。备注: {f['note'] or '无'}")
        lines.append(
            "如果用户表达了希望跟踪/分析某产品或创建分析任务的意图，"
            "请在回复末尾输出形如 ```task {\"symbol\": \"...\", \"type\": \"price_summary|volatility_report|llm_analysis\", \"description\": \"...\"}``` 的任务块。"
        )
        return "\n".join(lines)

    # ────────────────────────── AI 任务 ──────────────────────────

    def create_task(self, symbol: str, task_type: str, description: str = "") -> Dict[str, Any]:
        symbol = symbol.strip()
        if not symbol:
            return {"ok": False, "error": "symbol 不能为空"}
        if task_type not in SUPPORTED_TASK_TYPES:
            return {"ok": False, "error": f"不支持的任务类型: {task_type}，可选 {sorted(SUPPORTED_TASK_TYPES)}"}
        task = {
            "id": self._next_task_id,
            "symbol": symbol,
            "type": task_type,
            "description": description,
            "status": "pending",
            "created_at": datetime.now(timezone.utc).isoformat(),
            "result": None,
        }
        self._next_task_id += 1
        self._tasks.append(task)
        self._save_tasks()
        return {"ok": True, "task": task}

    def list_tasks(self, status: Optional[str] = None) -> List[Dict[str, Any]]:
        if status:
            return [t for t in self._tasks if t["status"] == status]
        return list(self._tasks)

    def run_task(self, task_id: int) -> Dict[str, Any]:
        task = next((t for t in self._tasks if t["id"] == task_id), None)
        if not task:
            return {"ok": False, "error": f"任务不存在: {task_id}"}
        try:
            if task["type"] == "price_summary":
                result = self._run_price_summary(task["symbol"])
            elif task["type"] == "volatility_report":
                result = self._run_volatility_report(task["symbol"])
            else:
                result = self._run_llm_analysis(task["symbol"], task.get("description", ""))
            task["status"] = "done"
        except Exception as e:
            task["status"] = "failed"
            result = {"error": str(e)}
        task["finished_at"] = datetime.now(timezone.utc).isoformat()
        task["result"] = result
        self._save_tasks()
        return {"ok": task["status"] == "done", "task": task}

    def run_pending(self) -> List[Dict[str, Any]]:
        return [self.run_task(t["id"]) for t in self._tasks if t["status"] == "pending"]

    # ── 任务执行器 ──

    def _run_price_summary(self, symbol: str) -> Dict[str, Any]:
        snap = self.snapshot(symbol)
        if not snap.get("available"):
            raise RuntimeError(snap.get("reason", "无行情数据"))
        return {"type": "price_summary", "summary": snap}

    def _run_volatility_report(self, symbol: str) -> Dict[str, Any]:
        db_path = self.config.engine_db_path
        with sqlite3.connect(f"file:{db_path}?mode=ro", uri=True) as conn:
            rows = conn.execute(
                "SELECT ts, last_price FROM ticks WHERE symbol = ? ORDER BY ts ASC",
                (symbol,),
            ).fetchall()
        if len(rows) < 10:
            raise RuntimeError(f"tick 数不足 (只有 {len(rows)} 条)，无法计算波动率")
        prices = [r[1] for r in rows]
        rets = [
            (prices[i] - prices[i - 1]) / prices[i - 1]
            for i in range(1, len(prices))
            if prices[i - 1] > 0
        ]
        mean = sum(rets) / len(rets)
        var = sum((r - mean) ** 2 for r in rets) / len(rets)
        per_tick_std = var ** 0.5
        # 按平均采样间隔折算年化 (期货年化因子 ~242 交易日)
        span_us = rows[-1][0] - rows[0][0]
        avg_interval_s = max(span_us / 1e6 / (len(rows) - 1), 1e-6)
        samples_per_year = 242 * 8 * 3600 / avg_interval_s
        annualized_vol = per_tick_std * (samples_per_year ** 0.5) * 100
        ranges = [abs(prices[i] - prices[i - 1]) for i in range(1, len(prices))]
        return {
            "type": "volatility_report",
            "symbol": symbol,
            "samples": len(rets),
            "per_tick_std_pct": round(per_tick_std * 100, 5),
            "annualized_vol_pct": round(annualized_vol, 2),
            "avg_move": round(sum(ranges) / len(ranges), 4),
            "max_move": round(max(ranges), 4),
            "note": "基于已落盘 tick 的粗估，样本越短可信度越低",
        }

    def _run_llm_analysis(self, symbol: str, question: str) -> Dict[str, Any]:
        snap = self.snapshot(symbol)
        data_ctx = json.dumps(snap, ensure_ascii=False)
        try:
            vol = self._run_volatility_report(symbol)
            data_ctx += "\n波动率摘要: " + json.dumps(vol, ensure_ascii=False)
        except Exception:
            pass
        messages = [
            {"role": "system", "content": "你是严谨的量化投研分析师。基于给定数据做客观分析，禁止编造数据中没有的数字，如实说明数据局限性。"},
            {"role": "user", "content": f"专项跟踪产品 {symbol} 的行情数据:\n{data_ctx}\n\n分析问题: {question or '给出该产品近期走势要点与风险提示'}"},
        ]
        return {"type": "llm_analysis", "symbol": symbol, "analysis": self.provider.chat_completion(messages)}

    # ────────────────────────── 聊天任务块解析 ──────────────────────────

    def extract_and_create_tasks(self, llm_reply: str) -> List[Dict[str, Any]]:
        """从 LLM 回复中解析 ```task {...}``` 块并创建任务"""
        created = []
        for m in _TASK_BLOCK_RE.finditer(llm_reply or ""):
            try:
                spec = json.loads(m.group(1))
                res = self.create_task(
                    spec.get("symbol", ""),
                    spec.get("type", ""),
                    spec.get("description", ""),
                )
                if res.get("ok"):
                    created.append(res["task"])
            except Exception:
                continue
        return created
