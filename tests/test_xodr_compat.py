#!/usr/bin/env python3
"""Regression tests for the OpenDRIVE/esmini compatibility preflight."""

import unittest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

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


if __name__ == "__main__":
    unittest.main()
