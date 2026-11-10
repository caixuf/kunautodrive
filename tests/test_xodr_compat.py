#!/usr/bin/env python3
"""Regression tests for the OpenDRIVE/esmini compatibility preflight."""

import unittest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import json_to_xodr
from tools.xodr_compat_check import check_xodr, check_xodr_text


def _road(
    road_id: str = "1",
    length: str = "100",
    plan_shape: str = "<line />",
    plan_start: str = 'x="0" y="0" hdg="0"',
    lanes: str = """
      <laneSection s="0">
        <center><lane id="0" type="none" /></center>
        <right>
          <lane id="-1" type="driving">
            <width s="0" a="3.5" b="0" c="0" d="0" />
          </lane>
        </right>
      </laneSection>
    """,
    extra: str = "",
) -> str:
    return f"""
    <road name="test-{road_id}" length="{length}" id="{road_id}" junction="-1">
      <type s="0" type="town" rule="RHT" />
      <planView>
        <geometry s="0" {plan_start} length="{length}">{plan_shape}</geometry>
      </planView>
      <lanes>{lanes}</lanes>
      {extra}
    </road>
    """


def _document(roads: str) -> str:
    return f"""<?xml version="1.0" encoding="UTF-8"?>
    <OpenDRIVE>
      <header revMajor="1" revMinor="4" name="test" version="1.00" />
      {roads}
    </OpenDRIVE>
    """


class XodrCompatibilityTest(unittest.TestCase):
    def test_supported_line_and_arc_shapes_pass(self):
        roads = _road() + _road(
            road_id="2",
            length="50",
            plan_shape='<arc curvature="0.01" />',
            plan_start='x="100" y="0" hdg="0"',
        )
        report = check_xodr_text(_document(roads))

        self.assertEqual(report["status"], "PASS")
        self.assertTrue(report["compatible"])
        self.assertEqual(report["counts"]["geometry.line"], 1)
        self.assertEqual(report["counts"]["geometry.arc"], 1)
        self.assertEqual(report["features"]["geometry.arc"]["status"], "supported")

    def test_partial_features_are_warnings_and_strict_fails(self):
        lanes = """
          <laneOffset s="0" a="0" b="0" c="0" d="0" />
          <laneSection s="0">
            <center><lane id="0" type="none" /></center>
            <right>
              <lane id="-1" type="driving">
                <width s="0" a="3.5" b="0" c="0" d="0" />
              </lane>
            </right>
          </laneSection>
        """
        extra = """
          <lateralProfile>
            <superelevation s="0" a="0" b="0" c="0" d="0" />
          </lateralProfile>
        """
        report = check_xodr_text(
            _document(
                _road(
                    length="20",
                    plan_shape="""
                      <paramPoly3 aU="0" bU="1" cU="0" dU="0"
                                  aV="0" bV="0" cV="0" dV="0"
                                  pRange="normalized" />
                    """,
                    lanes=lanes,
                    extra=extra,
                )
            )
        )

        self.assertEqual(report["status"], "WARN")
        self.assertTrue(report["compatible"])
        self.assertFalse(report["strict_compatible"])
        self.assertEqual(report["features"]["geometry.paramPoly3"]["status"], "partial")
        self.assertEqual(report["features"]["laneOffset"]["status"], "partial")
        self.assertEqual(report["features"]["superelevation"]["status"], "partial")

    def test_unknown_geometry_is_blocking(self):
        report = check_xodr_text(
            _document(_road(length="20", plan_shape="<cubicSpline />"))
        )

        self.assertEqual(report["status"], "FAIL")
        self.assertFalse(report["compatible"])
        self.assertTrue(any(issue["code"] == "unsupported_geometry" for issue in report["issues"]))

    def test_lane_direction_and_references_are_validated(self):
        malformed = """
          <road name="bad" length="20" id="1" junction="99">
            <planView>
              <geometry s="0" x="0" y="0" hdg="0" length="20"><line /></geometry>
            </planView>
            <link>
              <successor elementType="road" elementId="404" contactPoint="start" />
            </link>
            <lanes>
              <laneSection s="0">
                <center><lane id="1" type="none" /></center>
                <right><lane id="1" type="driving" /></right>
              </laneSection>
            </lanes>
          </road>
        """
        report = check_xodr_text(_document(malformed))
        codes = {issue["code"] for issue in report["issues"]}

        self.assertFalse(report["compatible"])
        self.assertIn("missing_road_link_target", codes)
        self.assertIn("missing_road_junction", codes)
        self.assertIn("lane_center_id", codes)
        self.assertIn("lane_side_mismatch", codes)

    def test_unsafe_xml_and_missing_file_fail_cleanly(self):
        unsafe = check_xodr_text("<!DOCTYPE OpenDRIVE><OpenDRIVE />")
        self.assertEqual(unsafe["status"], "FAIL")
        self.assertEqual(unsafe["issues"][0]["code"], "unsafe_xml")

        missing = check_xodr(ROOT / "does-not-exist.xodr")
        self.assertEqual(missing["status"], "FAIL")
        self.assertEqual(missing["issues"][0]["code"], "file_not_found")

    def test_json_to_xodr_3d_nodes_and_friction_zones_generation(self):
        # 1. 3D 共线斜坡道路：验证 elevationProfile 与 friction_zones material 导出
        scenario_collinear = {
            "name": "collinear_slope_and_friction",
            "road_network": {
                "edges": [
                    {
                        "id": 0,
                        "type": "urban",
                        "lanes": 2,
                        "lane_width": 3.5,
                        "nodes": [[0.0, 0.0, 0.0], [100.0, 0.0, 8.0]],
                    }
                ]
            },
            "friction_zones": [
                {
                    "x_min": 20.0,
                    "x_max": 80.0,
                    "y_min": -5.0,
                    "y_max": 5.0,
                    "mu_scale": 0.35,
                    "surface": "wet",
                }
            ],
        }
        xml_collinear = json_to_xodr.convert(scenario_collinear)
        self.assertIn("<elevationProfile>", xml_collinear)
        self.assertIn('<material sOffset="20.000000"', xml_collinear)
        self.assertIn('friction="0.3500"', xml_collinear)

        report_collinear = check_xodr_text(xml_collinear)
        self.assertEqual(report_collinear["status"], "PASS")
        self.assertTrue(report_collinear["compatible"])
        self.assertEqual(report_collinear["counts"].get("elevation", 0), 2)
        self.assertEqual(report_collinear["features"].get("lane.material", {}).get("status"), "supported")

        # 2. 3D 非共线高架道路：验证曲线密采样 elevationProfile 导出
        scenario_elevated = {
            "name": "elevated_curve",
            "road_network": {
                "edges": [
                    {
                        "id": 1,
                        "type": "highway",
                        "lanes": 4,
                        "lane_width": 3.75,
                        "nodes": [[0.0, 0.0, 0.0], [50.0, 15.0, 6.0], [100.0, 0.0, 12.0]],
                    }
                ]
            },
        }
        xml_elevated = json_to_xodr.convert(scenario_elevated)
        self.assertIn("<elevationProfile>", xml_elevated)
        report_elevated = check_xodr_text(xml_elevated)
        self.assertEqual(report_elevated["status"], "PASS")
        self.assertTrue(report_elevated["compatible"])
        self.assertGreater(report_elevated["counts"].get("elevation", 0), 2)

    def test_steep_grade_warning_and_invalid_friction_error(self):
        # 1. 陡坡预检 (|b| > 0.25) 触发 steep_elevation_grade warning
        steep_elevation = """
          <elevationProfile>
            <elevation s="0" a="0" b="0.30" c="0" d="0" />
            <elevation s="100" a="30" b="0" c="0" d="0" />
          </elevationProfile>
        """
        report_steep = check_xodr_text(_document(_road(extra=steep_elevation)))
        self.assertEqual(report_steep["status"], "WARN")
        self.assertTrue(any(issue["code"] == "steep_elevation_grade" for issue in report_steep["issues"]))

        # 2. 非法负摩擦系数 (<material sOffset="0" friction="-0.2"/>) 触发 invalid_lane_friction error
        lanes_invalid_friction = """
          <laneSection s="0">
            <center><lane id="0" type="none" /></center>
            <right>
              <lane id="-1" type="driving">
                <width s="0" a="3.5" b="0" c="0" d="0" />
                <material sOffset="0" surface="ice" friction="-0.2" roughness="0.0" />
              </lane>
            </right>
          </laneSection>
        """
        report_bad_friction = check_xodr_text(_document(_road(lanes=lanes_invalid_friction)))
        self.assertEqual(report_bad_friction["status"], "FAIL")
        self.assertTrue(any(issue["code"] == "invalid_lane_friction" for issue in report_bad_friction["issues"]))


if __name__ == "__main__":
    unittest.main()
