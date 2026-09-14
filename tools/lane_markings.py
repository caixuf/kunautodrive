"""lane_markings.py — 车道标线生产规则（单一事实源）

map.json 的 lanes[].markings 只准从这里长出来：
  extract_city_map / map_compiler 自动派生、osm2kmap 从 SUMO 车道写出，
  都走 markings_for_lane。前端 RoadView 只做 type→mesh，禁止再发明一套。

GB 5768 语义（与历史 _markings 零回归）：
  最左/最右外侧 = 白实线；对向分隔 = 双黄；同向车道间 = 白虚线。
  匝道 / highway.*_link：最外侧再加 deceleration（车行道纵向减速标线）。
"""

RAMP_TYPE_HINTS = ("ramp", "_link")


def is_ramp_type(road_type):
    t = str(road_type or "").lower()
    return any(h in t for h in RAMP_TYPE_HINTS)


def markings_for_lane(index, n_lanes, *, oneway=False, is_opp=False,
                      has_opposing=None, road_type=""):
    """按车道位置推断 markings[]。

    index: 1-based，1=贴中心（最内），n_lanes=贴路肩（最外）。
    is_opp: 双向路对向半幅（左右侧对调）。
    has_opposing: 是否存在对向车流。None 时 = not oneway。
    """
    n = max(1, int(n_lanes))
    idx = int(index)
    if has_opposing is None:
        has_opposing = not oneway
    inner = "right" if is_opp else "left"
    outer = "left" if is_opp else "right"
    mk = []
    if idx == 1:
        mk.append({"type": "double_yellow" if has_opposing else "solid_white",
                   "side": inner})
    if idx == n:
        mk.append({"type": "solid_white", "side": outer})
    else:
        mk.append({"type": "dashed_white", "side": outer})
    if is_ramp_type(road_type) and idx == n:
        mk.append({"type": "deceleration", "side": outer})
    return mk
