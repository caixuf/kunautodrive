#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import unittest
import datetime
from ai_service.session_scheduler import AutoQuantSessionScheduler, MarketSessionState
from ai_service.ctp_bridge import CtpConfig, CtpExecutionGateway

class TestAutoQuantSessionScheduler(unittest.TestCase):
    def setUp(self):
        self.cfg = CtpConfig(broker_id="9999", user_id="test_simnow", account_id="acc_test_simnow")
        self.gw = CtpExecutionGateway(self.cfg)
        self.scheduler = AutoQuantSessionScheduler(ctp_gateway=self.gw)

    def test_session_state_evaluation(self):
        # 1. 测试早盘准备时段 (周二 08:55)
        dt_pre = datetime.datetime(2026, 9, 1, 8, 55, 0)
        st_pre = self.scheduler.evaluate_session_state(dt_pre)
        self.assertEqual(st_pre.session_type, "PRE_MARKET")
        self.assertTrue(st_pre.is_trading_day)

        # 2. 测试日盘连续交易时段 (周二 10:00)
        dt_trade = datetime.datetime(2026, 9, 1, 10, 0, 0)
        st_trade = self.scheduler.evaluate_session_state(dt_trade)
        self.assertEqual(st_trade.session_type, "TRADING")

        # 3. 测试日盘收盘清算时段 (周二 15:05)
        dt_post = datetime.datetime(2026, 9, 1, 15, 5, 0)
        st_post = self.scheduler.evaluate_session_state(dt_post)
        self.assertEqual(st_post.session_type, "POST_MARKET")

        # 4. 测试夜盘交易时段 (周二 21:30)
        dt_night = datetime.datetime(2026, 9, 1, 21, 30, 0)
        st_night = self.scheduler.evaluate_session_state(dt_night)
        self.assertEqual(st_night.session_type, "TRADING")

        # 5. 测试周末完全休市 (周日 12:00)
        dt_weekend = datetime.datetime(2026, 9, 6, 12, 0, 0)
        st_weekend = self.scheduler.evaluate_session_state(dt_weekend)
        self.assertEqual(st_weekend.session_type, "IDLE")
        self.assertFalse(st_weekend.is_trading_day)

    def test_pre_and_post_market_execution(self):
        today = "2026-09-01"
        res_pre = self.scheduler.execute_pre_market_workflow(today)
        self.assertEqual(res_pre["steps"]["ctp_connect"], "SUCCESS")
        self.assertEqual(res_pre["steps"]["settlement_confirm"], "CONFIRMED")

        res_post = self.scheduler.execute_post_market_workflow(today)
        self.assertIn("steps", res_post)
        self.assertEqual(self.scheduler.last_daily_report_date, today)

if __name__ == "__main__":
    unittest.main()
