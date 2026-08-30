import os
import unittest
import time
from ai_service.ctp_bridge import CtpConfig, CtpExecutionGateway, _RealCtpTraderSpi

class TestCtpExecutionGateway(unittest.TestCase):
    def setUp(self):
        self.cfg = CtpConfig(broker_id="9999", user_id="test_user_01", account_id="acc_test_simnow")
        self.orders = []
        self.trades = []
        self.gw = CtpExecutionGateway(
            config=self.cfg,
            on_order_cb=lambda o: self.orders.append(o),
            on_trade_cb=lambda t: self.trades.append(t)
        )

    def test_ctp_connection_and_handshake(self):
        ok = self.gw.connect()
        self.assertTrue(ok)
        self.assertTrue(self.gw.is_connected)
        self.assertTrue(self.gw.is_authenticated)
        self.assertTrue(self.gw.is_logged_in)
        self.assertTrue(self.gw.settlement_confirmed)

    def test_order_submission_and_fill(self):
        self.gw.connect()
        ref = self.gw.send_order("rb2405", "LONG", "OPEN", 3620.0, 10.0)
        self.assertTrue(len(ref) > 0)
        self.assertEqual(len(self.orders), 1)
        self.assertEqual(self.orders[0]["status"], "ACCEPTED")

        # 模拟部分成交 4 手
        self.gw.on_market_fill_simulated(ref, 3620.0, 4.0)
        self.assertEqual(len(self.trades), 1)
        self.assertEqual(self.trades[0]["volume"], 4.0)
        self.assertEqual(self.orders[-1]["status"], "PARTIALLY_FILLED")

        # 模拟全量成交余下 6 手
        self.gw.on_market_fill_simulated(ref, 3620.0, 6.0)
        self.assertEqual(len(self.trades), 2)
        self.assertEqual(self.orders[-1]["status"], "FILLED")

    def test_query_rate_limiting(self):
        self.gw.connect()
        # 第一次查询应成功
        acc1 = self.gw.query_account_throttled()
        self.assertIsNotNone(acc1)

        # 立即二次查询应被流控拦截 (返回 None)
        acc2 = self.gw.query_account_throttled()
        self.assertIsNone(acc2)

        # 等待 1 秒后应恢复
        time.sleep(1.05)
        acc3 = self.gw.query_account_throttled()
        self.assertIsNotNone(acc3)

    def test_real_spi_callbacks(self):
        spi = _RealCtpTraderSpi(self.gw)
        spi.OnFrontConnected()
        self.assertTrue(self.gw.is_connected)

        spi.OnRspAuthenticate(None, type('RspInfo', (), {'ErrorID': 0, 'ErrorMsg': 'Success'})(), 1, True)
        self.assertTrue(self.gw.is_authenticated)

        spi.OnRspUserLogin(None, type('RspInfo', (), {'ErrorID': 0, 'ErrorMsg': 'Success'})(), 2, True)
        self.assertTrue(self.gw.is_logged_in)

        spi.OnRspSettlementInfoConfirm(None, type('RspInfo', (), {'ErrorID': 0, 'ErrorMsg': 'Success'})(), 3, True)
        self.assertTrue(self.gw.settlement_confirmed)

if __name__ == "__main__":
    unittest.main()

    def test_auto_reconnect_watchdog(self):
        spi = _RealCtpTraderSpi(self.gw)
        self.gw.connect()
        self.assertTrue(self.gw.is_connected)

        # 模拟断线
        spi.OnFrontDisconnected(4097)
        self.assertFalse(self.gw.is_connected)
        self.assertFalse(self.gw.is_logged_in)

        # 验证重连看门狗在后台成功拉起并重新完成连接
        time.sleep(1.2)
        self.assertTrue(self.gw.is_connected)
        self.assertTrue(self.gw.is_logged_in)
        self.assertTrue(self.gw.settlement_confirmed)
