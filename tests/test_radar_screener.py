import unittest
from ai_service.radar_screener import screen_radar_candidates, get_ai_radar_payload, calculate_atr, calculate_ma

class TestRadarScreener(unittest.TestCase):
    def test_calculate_atr(self):
        highs = [10.0, 12.0, 11.5, 13.0]
        lows = [9.0, 10.0, 10.5, 11.0]
        closes = [9.5, 11.0, 11.0, 12.5]
        atr = calculate_atr(highs, lows, closes, 3)
        self.assertGreater(atr, 0.0)

    def test_calculate_ma(self):
        series = [1.0, 2.0, 3.0, 4.0, 5.0]
        self.assertEqual(calculate_ma(series, 3), 4.0)

    def test_screen_candidates(self):
        recs = screen_radar_candidates()
        self.assertIsInstance(recs, list)
        if len(recs) > 0:
            top = recs[0]
            self.assertIn("symbol", top)
            self.assertIn("score", top)
            self.assertIn("entry", top)
            self.assertIn("sl", top)
            self.assertIn("tp", top)
            self.assertIn("reason", top)
            self.assertGreater(top["tp"], top["entry"])
            self.assertLess(top["sl"], top["entry"])

    def test_get_payload(self):
        payload = get_ai_radar_payload()
        self.assertEqual(payload["status"], "OK")
        self.assertIn("recommendations", payload)
        self.assertIn("timestamp", payload)

if __name__ == "__main__":
    unittest.main()
