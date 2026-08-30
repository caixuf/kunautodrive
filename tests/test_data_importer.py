import os
import gc
import shutil
import unittest
import tempfile
import sqlite3
from tools.data_importer import RealMarketDataImporter

class TestRealMarketDataImporter(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.mkdtemp()
        self.db_path = os.path.join(self.temp_dir, "test_data.db")
        self.csv_path = os.path.join(self.temp_dir, "test_bars.csv")
        self.importer = RealMarketDataImporter(self.db_path)

    def tearDown(self):
        # 显式解除对象引用并强制回收垃圾，释放 Windows 下的 SQLite 文件占用
        self.importer = None
        gc.collect()
        try:
            if os.path.exists(self.temp_dir):
                shutil.rmtree(self.temp_dir, ignore_errors=True)
        except Exception:
            pass

    def test_fetch_and_save_deterministic_bars(self):
        bars = self.importer.fetch_futures_daily("rb2405", "20240101", "20240115")
        self.assertTrue(len(bars) > 5)
        for b in bars:
            self.assertEqual(b["symbol"], "rb2405")
            self.assertTrue(b["high"] >= b["low"])
            self.assertTrue(b["volume"] > 0)

        count = self.importer.save_to_sqlite(bars)
        self.assertEqual(count, len(bars))

        conn = sqlite3.connect(self.db_path)
        try:
            cursor = conn.cursor()
            cursor.execute("SELECT COUNT(*), AVG(close) FROM bars WHERE symbol='rb2405'")
            row = cursor.fetchone()
            self.assertEqual(row[0], len(bars))
            self.assertTrue(row[1] > 3000.0)
        finally:
            conn.close()

    def test_save_to_csv(self):
        bars = self.importer.fetch_futures_daily("au2406", "20240101", "20240110")
        ok = self.importer.save_to_csv(self.csv_path, bars)
        self.assertTrue(ok)
        self.assertTrue(os.path.exists(self.csv_path))

        with open(self.csv_path, "r", encoding="utf-8") as f:
            lines = f.readlines()
            self.assertEqual(len(lines), len(bars) + 1)
            self.assertIn("date,open,high,low,close", lines[0])

if __name__ == "__main__":
    unittest.main()
