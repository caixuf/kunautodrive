#!/usr/bin/env python3
"""Preflight checker for the OpenDRIVE subset consumed by esmini.

This is intentionally a static preflight, not a second RoadManager.  Use the
existing C++ ``test_road_network`` executable with ``--runtime-test`` when a
real esmini load and Frenet/world invariant check is also required.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple


SUPPORTED_GEOMETRIES = {"line", "arc", "spiral", "poly3"}
PARTIAL_GEOMETRIES = {"paramPoly3"}
PARTIAL_FEATURES = {
    "laneOffset": "精细 lane offset 演化可能被 esmini 部分忽略。",
    "superelevation": "超高程支持依赖 esmini 版本，当前物理层按平面道路处理。",
    "crossfall": "横坡/横断面细节未在 KunAutoDrive 物理层中建模。",
    "shape": "复杂横断面 shape 可能只被部分消费。",
}


def _local_name(tag: str) -> str:
    """Return an XML tag name without an optional namespace."""
    return tag.rsplit("}", 1)[-1]


def _children(node: ET.Element, name: str) -> List[ET.Element]:
    return [child for child in list(node) if _local_name(child.tag) == name]


def _first_child(node: ET.Element, name: str) -> Optional[ET.Element]:
    for child in list(node):
        if _local_name(child.tag) == name:
            return child
    return None


def _descendants(node: ET.Element, name: str) -> List[ET.Element]:
    return [child for child in node.iter() if _local_name(child.tag) == name]


def _finite_number(value: Optional[str]) -> Optional[float]:
    if value is None:
        return None
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) else None


def _angle_delta(first: float, second: float) -> float:
    return (first - second + math.pi) % (2.0 * math.pi) - math.pi


@dataclass
class CompatibilityReport:
    path: str
    counts: Dict[str, int] = field(default_factory=dict)
    features: Dict[str, Dict[str, Any]] = field(default_factory=dict)
    issues: List[Dict[str, Any]] = field(default_factory=list)
    runtime: Optional[Dict[str, Any]] = None

    def count(self, name: str, amount: int = 1) -> None:
        self.counts[name] = self.counts.get(name, 0) + amount

    def feature(self, name: str, status: str, count: int, message: str) -> None:
        rank = {"supported": 0, "partial": 1, "warning": 1, "unsupported": 2}
        previous = self.features.get(name)
        if previous is None or rank.get(status, 2) > rank.get(previous["status"], 2):
            selected_status = status
        else:
            selected_status = previous["status"]
        self.features[name] = {
            "status": selected_status,
            "count": max(count, previous["count"] if previous else 0),
            "message": message if selected_status == status else previous["message"],
        }

    def issue(self, severity: str, code: str, message: str, **context: Any) -> None:
        item: Dict[str, Any] = {
            "severity": severity,
            "code": code,
            "message": message,
        }
        if context:
            item["context"] = context
        self.issues.append(item)

    def as_dict(self) -> Dict[str, Any]:
        has_errors = any(item["severity"] == "error" for item in self.issues)
        has_warnings = any(item["severity"] == "warning" for item in self.issues)
        status = "FAIL" if has_errors else "WARN" if has_warnings else "PASS"
        return {
            "path": self.path,
            "status": status,
            "compatible": not has_errors,
            "strict_compatible": not has_errors and not has_warnings,
            "counts": self.counts,
            "features": self.features,
            "issues": self.issues,
            "runtime": self.runtime,
        }


def _required_number(
    report: CompatibilityReport,
    element: ET.Element,
    attribute: str,
    context: str,
    minimum: Optional[float] = None,
) -> Optional[float]:
    value = _finite_number(element.get(attribute))
    if value is None:
        report.issue(
            "error",
            "invalid_number",
            f"{context} 缺少有效数值属性 {attribute!r}。",
            attribute=attribute,
        )
        return None
    if minimum is not None and value < minimum:
        report.issue(
            "error",
            "negative_value",
            f"{context} 的 {attribute}={value:g} 小于允许下限 {minimum:g}。",
            attribute=attribute,
        )
    return value


def _check_coefficients(
    report: CompatibilityReport,
    element: ET.Element,
    names: Iterable[str],
    context: str,
) -> None:
    for name in names:
        _required_number(report, element, name, context)


def _geometry_endpoint(
    geometry: ET.Element,
    shape: ET.Element,
    length: float,
) -> Optional[Tuple[float, float, float]]:
    x = _finite_number(geometry.get("x"))
    y = _finite_number(geometry.get("y"))
    hdg = _finite_number(geometry.get("hdg"))
    if x is None or y is None or hdg is None:
        return None
    kind = _local_name(shape.tag)
    if kind == "line":
        return x + length * math.cos(hdg), y + length * math.sin(hdg), hdg
    if kind == "arc":
        curvature = _finite_number(shape.get("curvature"))
        if curvature is None:
            return None
        if abs(curvature) < 1e-12:
            return x + length * math.cos(hdg), y + length * math.sin(hdg), hdg
        radius = 1.0 / curvature
        end_hdg = hdg + curvature * length
        center_x = x - radius * math.sin(hdg)
        center_y = y + radius * math.cos(hdg)
        return (
            center_x + radius * math.sin(end_hdg),
            center_y - radius * math.cos(end_hdg),
            end_hdg,
        )
    if kind == "poly3":
        a = _finite_number(shape.get("a"))
        b = _finite_number(shape.get("b"))
        c = _finite_number(shape.get("c"))
        d = _finite_number(shape.get("d"))
        if None in (a, b, c, d):
            return None
        local_y = a + b * length + c * length**2 + d * length**3
        local_dy = b + 2.0 * c * length + 3.0 * d * length**2
        return (
            x + length * math.cos(hdg) - local_y * math.sin(hdg),
            y + length * math.sin(hdg) + local_y * math.cos(hdg),
            hdg + math.atan2(local_dy, 1.0),
        )
    # Spiral and paramPoly3 require numerical integration/parameter mapping.
    return None


def _check_geometry(
    report: CompatibilityReport,
    road: ET.Element,
    geometry: ET.Element,
    road_id: str,
    index: int,
) -> Tuple[Optional[float], Optional[Tuple[float, float, float]]]:
    context = f"road {road_id} geometry[{index}]"
    s = _required_number(report, geometry, "s", context, minimum=0.0)
    length = _required_number(report, geometry, "length", context, minimum=0.0)
    _required_number(report, geometry, "x", context)
    _required_number(report, geometry, "y", context)
    _required_number(report, geometry, "hdg", context)
    if length is None or length <= 0.0:
        return s, None

    children = list(geometry)
    if len(children) != 1:
        report.issue(
            "error",
            "geometry_shape_count",
            f"{context} 必须恰好包含一个 geometry shape，实际为 {len(children)} 个。",
        )
        return s, None

    shape = children[0]
    kind = _local_name(shape.tag)
    report.count(f"geometry.{kind}")
    if kind in SUPPORTED_GEOMETRIES:
        report.feature(
            f"geometry.{kind}",
            "supported",
            report.counts[f"geometry.{kind}"],
            "esmini 已在项目道路生成器/运行时中使用。",
        )
    elif kind in PARTIAL_GEOMETRIES:
        report.feature(
            f"geometry.{kind}",
            "partial",
            report.counts[f"geometry.{kind}"],
            "esmini 对 paramPoly3 的支持依版本而异，建议先做运行时加载验证。",
        )
        report.issue(
            "warning",
            "partial_geometry",
            f"{context} 使用 paramPoly3，属于 esmini 部分支持范围。",
        )
    else:
        report.feature(
            f"geometry.{kind}",
            "unsupported",
            report.counts[f"geometry.{kind}"],
            "未知 geometry 类型不能按当前 esmini 兼容子集保证。",
        )
        report.issue(
            "error",
            "unsupported_geometry",
            f"{context} 使用未知或未验证的 geometry 类型 {kind!r}。",
        )

    if kind == "arc":
        _required_number(report, shape, "curvature", context)
    elif kind == "spiral":
        _required_number(report, shape, "curvStart", context)
        _required_number(report, shape, "curvEnd", context)
    elif kind == "poly3":
        _check_coefficients(report, shape, ("a", "b", "c", "d"), context)
    elif kind == "paramPoly3":
        _check_coefficients(
            report,
            shape,
            ("aU", "bU", "cU", "dU", "aV", "bV", "cV", "dV"),
            context,
        )
        p_range = shape.get("pRange", "normalized")
        if p_range not in {"normalized", "arcLength"}:
            report.issue(
                "warning",
                "unknown_param_range",
                f"{context} 的 paramPoly3 pRange={p_range!r} 未验证。",
            )

    return s, _geometry_endpoint(geometry, shape, length)


def _check_plan_view(
    report: CompatibilityReport,
    road: ET.Element,
    road_id: str,
    road_length: Optional[float],
) -> None:
    plan_view = _first_child(road, "planView")
    if plan_view is None:
        report.issue("error", "missing_plan_view", f"road {road_id} 缺少 planView。")
        return
    geometries = _children(plan_view, "geometry")
    if not geometries:
        report.issue("error", "empty_plan_view", f"road {road_id} 的 planView 没有 geometry。")
        return

    previous_end_s: Optional[float] = None
    previous_endpoint: Optional[Tuple[float, float, float]] = None
    for index, geometry in enumerate(geometries):
        s, endpoint = _check_geometry(report, road, geometry, road_id, index)
        length = _finite_number(geometry.get("length"))
        if s is None or length is None or length <= 0.0:
            continue
        if previous_end_s is not None:
            gap = s - previous_end_s
            if gap < -0.05:
                report.issue(
                    "error",
                    "geometry_s_order",
                    f"road {road_id} geometry[{index}] 的 s={s:g} 早于上一段结束位置 {previous_end_s:g}。",
                )
            elif gap > 0.05:
                report.issue(
                    "warning",
                    "geometry_s_gap",
                    f"road {road_id} geometry[{index}] 与上一段存在 {gap:g}m 的 s 间隙。",
                )
            if previous_endpoint is not None:
                start_x = _finite_number(geometry.get("x"))
                start_y = _finite_number(geometry.get("y"))
                start_hdg = _finite_number(geometry.get("hdg"))
                if start_x is not None and start_y is not None:
                    position_gap = math.hypot(
                        start_x - previous_endpoint[0],
                        start_y - previous_endpoint[1],
                    )
                    if position_gap > 2.0:
                        report.issue(
                            "error",
                            "geometry_position_gap",
                            f"road {road_id} geometry[{index}] 与上一段端点相差 {position_gap:.3f}m。",
                        )
                    elif position_gap > 0.25:
                        report.issue(
                            "warning",
                            "geometry_position_gap",
                            f"road {road_id} geometry[{index}] 与上一段端点相差 {position_gap:.3f}m。",
                        )
                if start_hdg is not None and endpoint is not None:
                    heading_gap = abs(_angle_delta(start_hdg, previous_endpoint[2]))
                    if heading_gap > 0.15:
                        report.issue(
                            "warning",
                            "geometry_heading_gap",
                            f"road {road_id} geometry[{index}] 与上一段存在 {heading_gap:.3f}rad 朝向不连续。",
                        )
        previous_end_s = s + length
        previous_endpoint = endpoint

    if previous_end_s is not None and road_length is not None:
        length_gap = abs(previous_end_s - road_length)
        if length_gap > 2.0:
            report.issue(
                "error",
                "road_length_mismatch",
                f"road {road_id} 的 planView 结束 s={previous_end_s:g} 与 length={road_length:g} 相差 {length_gap:.3f}m。",
            )
        elif length_gap > 0.25:
            report.issue(
                "warning",
                "road_length_mismatch",
                f"road {road_id} 的 planView 结束 s={previous_end_s:g} 与 length={road_length:g} 相差 {length_gap:.3f}m。",
            )


def _check_profiles(report: CompatibilityReport, road: ET.Element, road_id: str) -> None:
    elevation_profile = _first_child(road, "elevationProfile")
    if elevation_profile is not None:
        elevations = _children(elevation_profile, "elevation")
        report.count("elevation", len(elevations))
        report.feature(
            "elevationProfile",
            "supported",
            len(elevations),
            "elevationProfile 在 esmini 运行时支持范围内。",
        )
        previous_s = -math.inf
        for index, elevation in enumerate(elevations):
            context = f"road {road_id} elevation[{index}]"
            s = _required_number(report, elevation, "s", context, minimum=0.0)
            _check_coefficients(report, elevation, ("a", "b", "c", "d"), context)
            if s is not None and s < previous_s:
                report.issue("error", "elevation_s_order", f"{context} 的 s 顺序不是单调递增。")
            if s is not None:
                previous_s = s

    for name, message in PARTIAL_FEATURES.items():
        elements = _descendants(road, name)
        if not elements:
            continue
        report.count(name, len(elements))
        report.feature(name, "partial", len(elements), message)
        report.issue(
            "warning",
            f"partial_{name}",
            f"road {road_id} 包含 {len(elements)} 个 {name}，{message}",
        )


def _check_lanes(report: CompatibilityReport, road: ET.Element, road_id: str) -> None:
    lanes = _first_child(road, "lanes")
    if lanes is None:
        report.issue("error", "missing_lanes", f"road {road_id} 缺少 lanes。")
        return

    sections = _children(lanes, "laneSection")
    if not sections:
        report.issue("error", "empty_lane_sections", f"road {road_id} 的 lanes 没有 laneSection。")
        return
    previous_s = -math.inf
    driving_lanes = 0
    for section_index, section in enumerate(sections):
        context = f"road {road_id} laneSection[{section_index}]"
        section_s = _required_number(report, section, "s", context, minimum=0.0)
        if section_s is not None:
            if section_s < previous_s:
                report.issue("error", "lane_section_s_order", f"{context} 的 s 顺序不是单调递增。")
            previous_s = section_s
        lane_ids = set()
        for side_name, expected in (("left", "positive"), ("right", "negative"), ("center", "center")):
            side = _first_child(section, side_name)
            if side is None:
                continue
            for lane_index, lane in enumerate(_children(side, "lane")):
                lane_context = f"{context}/{side_name}/lane[{lane_index}]"
                raw_id = lane.get("id")
                try:
                    lane_id = int(raw_id) if raw_id is not None else None
                except ValueError:
                    lane_id = None
                if lane_id is None:
                    report.issue("error", "invalid_lane_id", f"{lane_context} 缺少整数 lane id。")
                    continue
                if lane_id in lane_ids:
                    report.issue("error", "duplicate_lane_id", f"{lane_context} 重复使用 lane id={lane_id}。")
                lane_ids.add(lane_id)
                if expected == "positive" and lane_id <= 0:
                    report.issue(
                        "error",
                        "lane_side_mismatch",
                        f"{lane_context} 位于 left，但 lane id={lane_id} 不是正数。",
                    )
                elif expected == "negative" and lane_id >= 0:
                    report.issue(
                        "error",
                        "lane_side_mismatch",
                        f"{lane_context} 位于 right，但 lane id={lane_id} 不是负数。",
                    )
                elif expected == "center" and lane_id != 0:
                    report.issue(
                        "error",
                        "lane_center_id",
                        f"{lane_context} 位于 center，但 lane id={lane_id} 而非 0。",
                    )
                lane_type = lane.get("type")
                if not lane_type:
                    report.issue("error", "missing_lane_type", f"{lane_context} 缺少 type。")
                if lane_type == "driving":
                    driving_lanes += 1
                    widths = _children(lane, "width")
                    if not widths:
                        report.issue(
                            "warning",
                            "missing_lane_width",
                            f"{lane_context} 没有 width，无法保证道路宽度 invariant。",
                        )
                    for width_index, width in enumerate(widths):
                        width_context = f"{lane_context}/width[{width_index}]"
                        if width.get("s") is None:
                            # The repository's json_to_xodr.py historically
                            # omits s on constant widths; esmini treats the
                            # first record as s=0, so keep this non-blocking.
                            report.issue(
                                "warning",
                                "missing_width_s",
                                f"{width_context} 缺少 s，按常量车道宽度默认使用 0。",
                            )
                        else:
                            _required_number(report, width, "s", width_context, minimum=0.0)
                        _check_coefficients(report, width, ("a", "b", "c", "d"), width_context)
        center = _first_child(section, "center")
        if center is None or not _children(center, "lane"):
            report.issue("warning", "missing_center_lane", f"{context} 缺少 center lane。")
    report.count("laneSection", len(sections))
    report.count("driving_lane", driving_lanes)
    if driving_lanes == 0:
        report.issue("warning", "no_driving_lane", f"road {road_id} 没有 type=driving 的车道。")


def _check_road_links(
    report: CompatibilityReport,
    road: ET.Element,
    road_id: str,
    road_ids: set,
    junction_ids: set,
) -> None:
    link = _first_child(road, "link")
    if link is not None:
        for relation in ("predecessor", "successor"):
            element = _first_child(link, relation)
            if element is None:
                continue
            element_type = element.get("elementType", "road")
            element_id = element.get("elementId")
            if element_id is None:
                report.issue("error", "missing_link_id", f"road {road_id} {relation} 缺少 elementId。")
                continue
            if element_type == "road" and element_id not in road_ids:
                report.issue(
                    "error",
                    "missing_road_link_target",
                    f"road {road_id} {relation} 引用了不存在的 road {element_id!r}。",
                )
            elif element_type == "junction" and element_id not in junction_ids:
                report.issue(
                    "error",
                    "missing_junction_link_target",
                    f"road {road_id} {relation} 引用了不存在的 junction {element_id!r}。",
                )
            elif element_type not in {"road", "junction"}:
                report.issue(
                    "warning",
                    "unknown_link_type",
                    f"road {road_id} {relation} 使用未验证的 elementType={element_type!r}。",
                )

    junction_ref = road.get("junction")
    if junction_ref not in (None, "", "-1") and junction_ref not in junction_ids:
        report.issue(
            "error",
            "missing_road_junction",
            f"road {road_id} 的 junction={junction_ref!r} 不存在。",
        )


def _check_junctions(
    report: CompatibilityReport,
    root: ET.Element,
    road_ids: set,
) -> set:
    junction_ids = set()
    junctions = _children(root, "junction")
    report.count("junction", len(junctions))
    if junctions:
        report.feature(
            "junction",
            "supported",
            len(junctions),
            "junction/connection 是当前 esmini 路网运行时支持的基础结构。",
        )
    for junction_index, junction in enumerate(junctions):
        context = f"junction[{junction_index}]"
        junction_id = junction.get("id")
        if not junction_id:
            report.issue("error", "missing_junction_id", f"{context} 缺少 id。")
            continue
        if junction_id in junction_ids:
            report.issue("error", "duplicate_junction_id", f"{context} 重复使用 id={junction_id!r}。")
        junction_ids.add(junction_id)
        for connection_index, connection in enumerate(_children(junction, "connection")):
            connection_context = f"{context}/connection[{connection_index}]"
            incoming = connection.get("incomingRoad")
            connecting = connection.get("connectingRoad")
            if incoming not in road_ids:
                report.issue(
                    "error",
                    "missing_incoming_road",
                    f"{connection_context} 引用了不存在的 incomingRoad={incoming!r}。",
                )
            if connecting not in road_ids:
                report.issue(
                    "error",
                    "missing_connecting_road",
                    f"{connection_context} 引用了不存在的 connectingRoad={connecting!r}。",
                )
            contact_point = connection.get("contactPoint")
            if contact_point not in {"start", "end"}:
                report.issue(
                    "warning",
                    "invalid_contact_point",
                    f"{connection_context} 的 contactPoint={contact_point!r} 未验证。",
                )
            for lane_link in _children(connection, "laneLink"):
                for attribute in ("from", "to"):
                    if lane_link.get(attribute) is None:
                        report.issue(
                            "error",
                            "missing_lane_link_id",
                            f"{connection_context}/laneLink 缺少 {attribute}。",
                        )
    return junction_ids


def _check_signals_and_objects(report: CompatibilityReport, root: ET.Element) -> None:
    signals = _descendants(root, "signal")
    if signals:
        report.count("signal", len(signals))
        report.feature(
            "signal",
            "supported",
            len(signals),
            "signal 基础字段可由 esmini RoadManager 消费。",
        )
    objects = _descendants(root, "object")
    if objects:
        report.count("object", len(objects))
        report.feature(
            "object",
            "supported",
            len(objects),
            "静态 object 会随路网加载；具体类型仍需运行时场景验证。",
        )


def check_xodr_text(text: str, source: str = "<memory>") -> Dict[str, Any]:
    """Check an XML string and return the same JSON-ready report as a file check."""
    report = CompatibilityReport(source)
    if "<!DOCTYPE" in text.upper() or "<!ENTITY" in text.upper():
        report.issue(
            "error",
            "unsafe_xml",
            "XODR 不允许包含 DOCTYPE/ENTITY，已拒绝可能触发外部实体解析的 XML。",
        )
        return report.as_dict()
    try:
        root = ET.fromstring(text)
    except ET.ParseError as exc:
        report.issue("error", "xml_parse_error", f"XODR XML 解析失败：{exc}")
        return report.as_dict()
    if _local_name(root.tag) != "OpenDRIVE":
        report.issue("error", "invalid_root", f"根元素必须是 OpenDRIVE，实际为 {_local_name(root.tag)!r}。")
        return report.as_dict()

    header = _first_child(root, "header")
    if header is None:
        report.issue("error", "missing_header", "XODR 缺少 header。")
    else:
        report.count("header")
        for attribute in ("revMajor", "revMinor"):
            if header.get(attribute) is None:
                report.issue("warning", "missing_header_field", f"header 缺少 {attribute}。")

    roads = _children(root, "road")
    report.count("road", len(roads))
    if not roads:
        report.issue("error", "no_roads", "XODR 至少需要一个 road。")
        return report.as_dict()
    report.feature("road", "supported", len(roads), "road 是 esmini RoadManager 的基础加载单元。")

    road_ids = set()
    road_lengths: Dict[str, Optional[float]] = {}
    for road_index, road in enumerate(roads):
        road_id = road.get("id")
        context = f"road[{road_index}]"
        if not road_id:
            report.issue("error", "missing_road_id", f"{context} 缺少 id。")
            road_id = f"<missing-{road_index}>"
        elif road_id in road_ids:
            report.issue("error", "duplicate_road_id", f"{context} 重复使用 id={road_id!r}。")
        road_ids.add(road_id)
        road_length = _required_number(report, road, "length", context, minimum=0.0)
        road_lengths[road_id] = road_length
        _check_plan_view(report, road, road_id, road_length)
        _check_lanes(report, road, road_id)
        _check_profiles(report, road, road_id)

    junction_ids = _check_junctions(report, root, road_ids)
    for road in roads:
        road_id = road.get("id") or "<missing>"
        _check_road_links(report, road, road_id, road_ids, junction_ids)
    _check_signals_and_objects(report, root)

    rules = {
        road_type.get("rule")
        for road in roads
        for road_type in _children(road, "type")
        if road_type.get("rule")
    }
    if rules:
        report.count("driving_rule", len(rules))
        if not rules.issubset({"RHT", "LHT"}):
            report.issue("warning", "unknown_driving_rule", f"发现未验证的 driving rule：{sorted(rules)!r}。")
        report.feature(
            "driving_rule",
            "supported",
            len(rules),
            "RHT/LHT 规则被记录；KunAutoDrive 默认场景按中国右侧通行解释。",
        )

    return report.as_dict()


def _run_runtime_test(
    report: CompatibilityReport,
    path: Path,
    executable: Path,
    timeout: float,
) -> None:
    runtime: Dict[str, Any] = {
        "executable": str(executable),
        "passed": False,
    }
    if not executable.is_file():
        runtime["error"] = "runtime test executable not found"
        report.runtime = runtime
        report.issue("error", "runtime_test_missing", f"找不到运行时测试程序：{executable}")
        return
    try:
        completed = subprocess.run(
            [str(executable), str(path)],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
            # IDE wrappers may inject an older libstdc++ into the child
            # process. The project binaries use their own linked libraries.
            env={key: value for key, value in os.environ.items() if key != "LD_LIBRARY_PATH"},
        )
    except subprocess.TimeoutExpired:
        runtime["error"] = f"runtime test timed out after {timeout:g}s"
        report.runtime = runtime
        report.issue("error", "runtime_test_timeout", runtime["error"])
        return
    runtime.update(
        {
            "passed": completed.returncode == 0,
            "returncode": completed.returncode,
            "stdout": completed.stdout[-4000:],
            "stderr": completed.stderr[-4000:],
        }
    )
    report.runtime = runtime
    if completed.returncode != 0:
        report.issue(
            "error",
            "runtime_test_failed",
            f"esmini 运行时测试失败，退出码 {completed.returncode}。",
        )


def check_xodr(
    path: Path,
    runtime_test: Optional[Path] = None,
    runtime_timeout: float = 30.0,
) -> Dict[str, Any]:
    """Check one XODR file, optionally followed by the existing C++ runtime test."""
    path = Path(path)
    if not path.is_file():
        report = CompatibilityReport(str(path))
        report.issue("error", "file_not_found", f"找不到 XODR 文件：{path}")
        return report.as_dict()
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        report = CompatibilityReport(str(path))
        report.issue("error", "file_read_error", f"读取 XODR 失败：{exc}")
        return report.as_dict()
    result = check_xodr_text(text, str(path))
    if runtime_test is not None and result["compatible"]:
        report = CompatibilityReport(path=str(path))
        report.counts = result["counts"]
        report.features = result["features"]
        report.issues = result["issues"]
        _run_runtime_test(report, path, Path(runtime_test), runtime_timeout)
        result = report.as_dict()
    return result


def _print_human(report: Dict[str, Any]) -> None:
    print(f"[{report['status']}] {report['path']}")
    counts = report.get("counts", {})
    if counts:
        summary = ", ".join(f"{name}={value}" for name, value in sorted(counts.items()))
        print(f"  counts: {summary}")
    for name, feature in sorted(report.get("features", {}).items()):
        print(f"  {feature['status']:>11} {name} ({feature['count']}): {feature['message']}")
    for issue in report.get("issues", []):
        print(f"  {issue['severity'].upper():>7} {issue['code']}: {issue['message']}")
    runtime = report.get("runtime")
    if runtime is not None:
        print(
            f"  runtime: {'PASS' if runtime.get('passed') else 'FAIL'} "
            f"({runtime.get('executable', 'unknown')})"
        )


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="预检 OpenDRIVE 是否落在 KunAutoDrive/esmini 已验证兼容子集内。",
    )
    parser.add_argument("paths", nargs="+", type=Path, help="待检查的 .xodr 文件")
    parser.add_argument("--json", action="store_true", help="输出 JSON 报告")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="将 partial/warning 也视为失败；默认只有 error 失败。",
    )
    parser.add_argument(
        "--runtime-test",
        type=Path,
        help="可选：调用已有 test_road_network 可执行文件做 esmini 加载/invariant 验证。",
    )
    parser.add_argument(
        "--runtime-timeout",
        type=float,
        default=30.0,
        help="运行时测试超时秒数（默认 30）。",
    )
    args = parser.parse_args(argv)

    reports = [
        check_xodr(path, args.runtime_test, args.runtime_timeout)
        for path in args.paths
    ]
    if args.json:
        payload: Any = reports[0] if len(reports) == 1 else {"reports": reports}
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    else:
        for index, report in enumerate(reports):
            if index:
                print()
            _print_human(report)
    if args.strict:
        return 0 if all(report["strict_compatible"] for report in reports) else 1
    return 0 if all(report["compatible"] for report in reports) else 1


if __name__ == "__main__":
    sys.exit(main())
