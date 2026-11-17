#!/usr/bin/env python3
"""frame_unit_contract_check.py — 四层原始消息隐式约定（坐标系 + 时间戳三件套 + 单位）门禁。

校验：
  1. msg/common.msg 的 Header 必须包含 sensor_time_us / host_time_us / sync_time_us / frame_id；
  2. 所有 v2 领域 .msg 中的几何/状态/输出关键消息（Localization/PointCloud/ImageFrame/ChassisState）
     必须声明 Header 或显式 frame_id/timestamp_us；
  3. 单位命名一致性：禁止新增无单位后缀的模糊时间字段（time/stamp 未带 _us/_ms/_s）。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from msg_codegen import IDLParser  # noqa: E402

REQUIRED_HEADER_FIELDS = {
    "sensor_time_us",
    "host_time_us",
    "sync_time_us",
    "sync_status",
    "frame_id",
}

REQUIRED_V2_STRUCTS = {
    "Header",
    "ImageFrame",
    "ChassisState",
    "HmiDecisionState",
    "Localization",
}


def main() -> int:
    schema = IDLParser.from_file(ROOT / "msg" / "adas_msgs.msg")
    by_name = schema.structs
    errors: list[str] = []

    for req in REQUIRED_V2_STRUCTS:
        if req not in by_name:
            errors.append(f"missing required v2 struct `{req}`")

    hdr = by_name.get("Header")
    if hdr:
        hdr_fields = {f.name for f in hdr.fields}
        missing = sorted(REQUIRED_HEADER_FIELDS - hdr_fields)
        if missing:
            errors.append(f"Header missing timestamp-trio/frame fields: {missing}")

    # Key messages must embed Header or declare frame_id/timestamp_us explicitly.
    for name in ("ImageFrame", "ChassisState", "HmiDecisionState"):
        st = by_name.get(name)
        if st and not any(f.idl_type == "Header" for f in st.fields):
            errors.append(f"`{name}` must embed `Header` for frame_id & timestamp-trio")
    for name in ("Localization", "LidarPointCloud"):
        st = by_name.get(name)
        if not st:
            continue
        fields = {f.name for f in st.fields}
        has_header = any(f.idl_type == "Header" for f in st.fields)
        if not has_header and not {"frame_id", "timestamp_us"} <= fields:
            errors.append(
                f"`{name}` must embed `Header` or declare frame_id+timestamp_us"
            )

    # Unit suffix contract: time/stamp fields must state their unit.
    for name, st in by_name.items():
        for f in st.fields:
            if re.search(r"(time|stamp)", f.name) and not re.search(r"_(us|ms|s)$", f.name):
                errors.append(
                    f"`{name}.{f.name}` has an ambiguous time unit suffix "
                    "(expected _us/_ms/_s)"
                )

    if errors:
        print("✗ frame_unit_contract_check FAILED:")
        for e in errors:
            print(f"  {e}")
        return 1

    print(
        f"✓ frame_unit_contract_check OK "
        f"({len(schema.structs)} structs, Header timestamp-trio + frame_id verified)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
