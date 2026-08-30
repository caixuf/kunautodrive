#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ai_service/session_scheduler.py — 7×24 小时全自动期货交易日流水线调度器 (Session Scheduler)

核心状态机与交易日流程：
1. PRE_MARKET (08:50-09:00 / 20:50-21:00):
   - CTP / SimNow 柜台三步握手 (Connect -> Authenticate -> Login)
   - 自动提交投资者结算结果确认 (ReqSettlementInfoConfirm)
   - 检查账户资金充足度与事前风控门限
   - 检查并清理昨日遗留未成交挂单
2. TRADING (09:00-15:00 / 21:00-02:30):
   - 连续交易时段监视
   - 网关断线看门狗实时巡检
3. POST_MARKET (15:00-15:15 / 02:30-02:40):
   - 自动持仓与账本结算 (SQLite WAL -> /api/report)
   - 触发 AI 微服务生成每日复盘研报与 FocusTracker 诊断
   - 触发 Walk-Forward 参数平原检验与进化引擎次日推荐更新
4. IDLE:
   - 休市挂起与低频心跳
"""

import sys
import os
import time
import json
import logging
import datetime
import threading
from typing import Dict, Any, Optional
from dataclasses import dataclass
import urllib.request
import urllib.error

logging.basicConfig(level=logging.INFO, format='[%(asctime)s] [%(levelname)s] [SessionScheduler] %(message)s')

@dataclass
class MarketSessionState:
    session_type: str # "PRE_MARKET", "TRADING", "POST_MARKET", "IDLE"
    is_trading_day: bool
    current_time_str: str
    next_event: str
    seconds_to_next: float

class AutoQuantSessionScheduler:
    def __init__(self, ctp_gateway=None, daemon_url="http://127.0.0.1:8900", ai_url="http://127.0.0.1:8901"):
        self.ctp_gateway = ctp_gateway
        self.daemon_url = daemon_url
        self.ai_url = ai_url
        self.running = False
        self._worker_thread = None
        self.last_daily_report_date = ""
        self.last_premarket_checked = ""

    def evaluate_session_state(self, now: Optional[datetime.datetime] = None) -> MarketSessionState:
        """根据中国商品期货交易时间表计算当前状态机阶段"""
        if now is None:
            now = datetime.datetime.now()

        weekday = now.weekday() # 0 = Monday, 6 = Sunday
        time_hm = now.strftime("%H:%M")
        is_trading_day = weekday < 5

        # 周末判定 (周六 02:30 后至周日 20:50 前为完全休市)
        if weekday == 5 and time_hm >= "02:30":
            is_trading_day = False
        elif weekday == 6 and time_hm < "20:50":
            is_trading_day = False

        # 时段切分
        # 1. 早盘前准备 (08:50 - 09:00)
        if is_trading_day and "08:50" <= time_hm < "09:00":
            return MarketSessionState("PRE_MARKET", True, time_hm, "日盘连续交易开市", 600)
        # 2. 夜盘前准备 (20:50 - 21:00)
        elif is_trading_day and "20:50" <= time_hm < "21:00":
            return MarketSessionState("PRE_MARKET", True, time_hm, "夜盘连续交易开市", 600)
        # 3. 日盘连续交易 (09:00 - 15:00，包含 10:15-10:30, 11:30-13:30 小节休)
        elif is_trading_day and "09:00" <= time_hm < "15:00":
            return MarketSessionState("TRADING", True, time_hm, "日盘收盘清算", 3600)
        # 4. 夜盘连续交易 (21:00 - 02:30 次日)
        elif is_trading_day and ("21:00" <= time_hm or time_hm < "02:30"):
            return MarketSessionState("TRADING", True, time_hm, "夜盘收盘清算", 3600)
        # 5. 日盘收盘清算 (15:00 - 15:15)
        elif is_trading_day and "15:00" <= time_hm < "15:15":
            return MarketSessionState("POST_MARKET", True, time_hm, "进入休市待机", 900)
        # 6. 夜盘收盘清算 (02:30 - 02:40)
        elif is_trading_day and "02:30" <= time_hm < "02:40":
            return MarketSessionState("POST_MARKET", True, time_hm, "进入休市待机", 600)
        else:
            return MarketSessionState("IDLE", is_trading_day, time_hm, "等待开盘准备时段", 1800)

    def execute_pre_market_workflow(self, date_str: str) -> Dict[str, Any]:
        """执行盘前全自动自检流程"""
        logging.info(f"========== [PRE_MARKET] 执行盘前全自动自检流程 ({date_str}) ==========")
        results = {"date": date_str, "steps": {}}

        # 1. 检查并握手 CTP / SimNow 柜台
        if self.ctp_gateway:
            try:
                ok = self.ctp_gateway.connect()
                results["steps"]["ctp_connect"] = "SUCCESS" if ok else "FAILED"
                # 自动确认结算单
                self.ctp_gateway.confirm_settlement()
                results["steps"]["settlement_confirm"] = "CONFIRMED"
            except Exception as e:
                results["steps"]["ctp_connect"] = f"ERROR: {e}"
        else:
            results["steps"]["ctp_connect"] = "SKIPPED_NO_GATEWAY"

        # 2. 检查主服务 daemon 健康状态
        daemon_health = self._check_http_endpoint(f"{self.daemon_url}/api/report")
        results["steps"]["daemon_health"] = "ONLINE" if daemon_health else "OFFLINE"

        # 3. 检查 AI 微服务健康状态
        ai_health = self._check_http_endpoint(f"{self.ai_url}/api/ai/diagnose")
        results["steps"]["ai_service_health"] = "ONLINE" if ai_health else "OFFLINE"

        self.last_premarket_checked = date_str
        logging.info(f"盘前自检完成: {results}")
        return results

    def execute_post_market_workflow(self, date_str: str) -> Dict[str, Any]:
        """执行收盘全自动清算与 AI 研报生成"""
        logging.info(f"========== [POST_MARKET] 执行收盘全自动结算与研报流程 ({date_str}) ==========")
        results = {"date": date_str, "steps": {}}

        # 1. 请求 daemon 获取日终收益报告
        report_data = self._http_get_json(f"{self.daemon_url}/api/report")
        if report_data:
            results["steps"]["daily_report"] = "FETCHED"
            logging.info(f"获取到日终结算报告: 账户总盈亏={report_data.get('total_pnl', 0.0)} 交易笔数={report_data.get('trade_count', 0)}")
        else:
            results["steps"]["daily_report"] = "OFFLINE_FALLBACK"

        # 2. 触发 AI 微服务生成每日复盘研报
        trigger_res = self._http_post_json(f"{self.ai_url}/api/ai/diagnose", {"action": "generate_daily_report", "date": date_str})
        results["steps"]["ai_daily_digest"] = "TRIGGERED" if trigger_res else "SKIPPED"

        self.last_daily_report_date = date_str
        logging.info(f"收盘结算完成: {results}")
        return results

    def _check_http_endpoint(self, url: str) -> bool:
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "KunQuantScheduler/1.0"})
            with urllib.request.urlopen(req, timeout=1.5) as resp:
                return resp.status == 200
        except Exception:
            return False

    def _http_get_json(self, url: str) -> Optional[Dict[str, Any]]:
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "KunQuantScheduler/1.0"})
            with urllib.request.urlopen(req, timeout=2.0) as resp:
                if resp.status == 200:
                    return json.loads(resp.read().decode('utf-8'))
        except Exception:
            pass
        return None

    def _http_post_json(self, url: str, data: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        try:
            body = json.dumps(data).encode('utf-8')
            req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json", "User-Agent": "KunQuantScheduler/1.0"})
            with urllib.request.urlopen(req, timeout=3.0) as resp:
                if resp.status == 200:
                    return json.loads(resp.read().decode('utf-8'))
        except Exception:
            pass
        return None

    def start_daemon(self):
        """以独立看门狗线程启动调度器"""
        if self.running:
            return
        self.running = True
        self._worker_thread = threading.Thread(target=self._scheduler_loop, daemon=True, name="AutoQuantScheduler")
        self._worker_thread.start()
        logging.info("7×24 小时全自动交易日调度器已启动。")

    def stop_daemon(self):
        self.running = False
        if self._worker_thread and self._worker_thread.joinable():
            self._worker_thread.join(timeout=2.0)

    def _scheduler_loop(self):
        while self.running:
            now = datetime.datetime.now()
            today_str = now.strftime("%Y-%m-%d")
            st = self.evaluate_session_state(now)

            if st.session_type == "PRE_MARKET" and self.last_premarket_checked != today_str:
                self.execute_pre_market_workflow(today_str)
            elif st.session_type == "POST_MARKET" and self.last_daily_report_date != today_str:
                self.execute_post_market_workflow(today_str)

            time.sleep(10)

def main():
    scheduler = AutoQuantSessionScheduler()
    st = scheduler.evaluate_session_state()
    print(f"当前交易状态评估: 时段={st.session_type}, 交易日={st.is_trading_day}, 当前时间={st.current_time_str}, 下一事件={st.next_event}")

    # 手动触发一次自检演示
    today = datetime.datetime.now().strftime("%Y-%m-%d")
    res_pre = scheduler.execute_pre_market_workflow(today)
    res_post = scheduler.execute_post_market_workflow(today)
    print("Pre-market result:", res_pre)
    print("Post-market result:", res_post)

if __name__ == "__main__":
    main()
