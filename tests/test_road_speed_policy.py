"""posted_speed_mps 与 include/road_speed_policy.h 同一张表。"""
import unittest

from json_to_xodr import posted_speed_mps


class PostedSpeedTest(unittest.TestCase):
    def test_table(self):
        self.assertAlmostEqual(posted_speed_mps("urban", 14.0), 14.0)
        self.assertAlmostEqual(posted_speed_mps("urban", 10.0), 10.0)
        self.assertAlmostEqual(posted_speed_mps("urban", 30.0), 50.0 / 3.6)
        self.assertAlmostEqual(posted_speed_mps("viaduct_highway", 15.5), 15.5)
        self.assertAlmostEqual(posted_speed_mps("viaduct_highway", 0.0), 100.0 / 3.6)
        self.assertAlmostEqual(posted_speed_mps("ramp_curve", 14.0), 14.0)
        self.assertAlmostEqual(posted_speed_mps("ramp_curve", 0.0), 60.0 / 3.6)
        self.assertAlmostEqual(posted_speed_mps("garage_hall", 6.5), 6.5)
        self.assertAlmostEqual(posted_speed_mps("exit_merge", 10.0), 10.0)


if __name__ == "__main__":
    unittest.main()
