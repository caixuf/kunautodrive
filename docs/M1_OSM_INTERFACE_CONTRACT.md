# M1 接口契约：D2-02 输出 OSM XML Schema（v1.0）

> **目的**：让 D2-02（转换器）和 D2-09（consistency gate）并行开发不打架。本文档是
> `tools/json_to_lanelet.py` **必须**产出的 OSM XML 格式唯一权威定义。
>
> **约束**：
> - 字节级确定性（同一 map.json → 同一 .osm，属性顺序固定、双精度坐标固定 6 位小数、relation member 排序稳定）
> - 元素命名遵循 OSM 0.6 标准（`<osm>` / `<node>` / `<way>` / `<relation>` / `<nd>` / `<member>` / `<tag>`）
> - Lanelet2 兼容（能被 `lanelet2::load()` 解析）
> - 保留原 map.json lane id（用于 D2-09 双向覆盖检查）

---

## 1. 顶层结构

```xml
<?xml version="1.0" encoding="UTF-8"?>
<osm version="0.6" generator="json_to_lanelet.py">
  <!-- 节点（每个 map.json lane 的 centerline 点） -->
  <node id="N0" lat="..." lon="..." />
  <node id="N1" lat="..." lon="..." />
  ...
  <!-- way（每个 map.json lane 的左右边界） -->
  <way id="W0">
    <nd ref="N0" />
    <nd ref="N1" />
    <tag k="type" v="line_thin" />
  </way>
  ...
  <!-- relation（每个 map.json lane → 一个 lanelet） -->
  <relation id="R0">
    <member type="way" ref="W0" role="left" />
    <member type="way" ref="W1" role="right" />
    <tag k="type" v="lanelet" />
    <tag k="subtype" v="road" />
    <tag k="speed_limit" v="13.89" />
    <tag k="location" v="urban" />
    <tag k="one_way" v="yes" />
    <tag k="lanelet_id" v="road_r10260585s0.lane.1" />
  </relation>
  ...
  <!-- relation（traffic light，regulatory_element） -->
  <relation id="RT0">
    <member type="way" ref="..." role="stop_line" />
    <member type="way" ref="..." role="ref_line" />
    <tag k="type" v="regulatory_element" />
    <tag k="subtype" v="traffic_light" />
    <tag k="lanelet_id" v="..." />
  </relation>
</osm>
```

---

## 2. 元素规范

### 2.1 `<osm>` 根

- `version` = `"0.6"`
- `generator` = `"json_to_lanelet.py"`
- 不允许 `upload="false"`（让 Lanelet2 直接 load）

### 2.2 `<node>`

- `id`：`"N" + 整数`（从 0 开始递增），同一 .osm 内唯一
- `lat` / `lon`：WGS84 经纬度，**6 位小数**（米级精度，~0.1m at equator）
- 坐标来源：map.json 是 UTM ENU 系，本转换器**假设** map.json 已经是 WGS84（与 `osm2kmap.py` 互逆）；
  - **本期不写投影转换**（todo：M2 起接入 pyproj），lat/lon 与 map.json 的 x/y 字段同名复用，加 `<!-- TODO: project ENU->WGS84 -->` 注释
- 无 `version` / `timestamp` / `user` / `changeset` 属性

### 2.3 `<way>`

- `id`：`"W" + 整数`，同一 .osm 内唯一
- 子元素顺序：先所有 `<nd ref="..." />`（按 centerline 顺序），后所有 `<tag k="..." v="..." />`（按 tag key 字典序）
- `<tag>`：
  - 边界 way 必须有 `<tag k="type" v="line_thin" />`
  - 停止线 way 必须有 `<tag k="type" v="stop_line" />`

### 2.4 `<relation type="lanelet">`

- `id`：`"R" + 整数`，同一 .osm 内唯一
- `<member>` 子元素：先 `left` 后 `right`（固定顺序）
- `<tag>` 子元素顺序固定（按下面 key 顺序，缺则跳过）：
  1. `type` = `"lanelet"`
  2. `subtype` = `"road"`（本期仅 road）
  3. `speed_limit` = 字符串数字，单位 m/s，**2 位小数**
  4. `location` = `"urban"` / `"rural"` / `"highway"`（来自 map.json `roads[].type` 映射，见 §3）
  5. `one_way` = `"yes"` / `"no"`
  6. `lanelet_id` = map.json 原始 lane id（**关键**，D2-09 用此做双向覆盖检查）
- 数字 tag 值禁止前导零、禁止单位后缀

### 2.5 `<relation type="regulatory_element">`

- `id`：`"RT" + 整数`（区别于普通 lanelet）
- `<member>` 角色：
  - `stop_line`：停止线 way
  - `ref_line`：受控 lanelet（本期复用普通 lane 的 left boundary way）
- `<tag>`：
  1. `type` = `"regulatory_element"`
  2. `subtype` = `"traffic_light"`（本期唯一）
  3. `lanelet_id` = map.json 红绿灯对应的 lane id

---

## 3. map.json → lanelet 字段映射

| map.json 字段 | .osm 字段 | 备注 |
|---|---|---|
| `roads[].id` | `relation.lanelet_id` (去 lane 后缀) | |
| `roads[].lanes[].id` | `relation.lanelet_id` | **唯一 key** |
| `roads[].lanes[].centerline[][0]` (x) | `node.lon` | **本期不投影，直接复用** |
| `roads[].lanes[].centerline[][1]` (y) | `node.lat` | 同上 |
| `roads[].lanes[].centerline[][2]` (z) | （丢弃） | 2D 平面投影 |
| `roads[].lanes[].width` | （计算左右边界偏移） | 左/右 way 各偏移 width/2 |
| `roads[].lanes[].direction` | `relation.one_way` | `1` → `"yes"`，`-1` → `"no"` |
| `roads[].speed_limit` | `relation.speed_limit` | 单位 m/s，2 位小数 |
| `roads[].type` | `relation.location` | `"urban"`/`"rural"`/`"motorway"`/`"residential"` 直传；其它 → `"urban"` |
| `traffic_lights[].lane` | `relation.lanelet_id` + `stop_line` member | 本期红绿灯结构粗略，按 lane 挂即可 |

---

## 4. 字节级确定性要求

D2-02 转换器输出必须**字节稳定**（同 map.json 跑两次 mmap 比对通过）：

1. **node id 严格递增**：按 lane 的 centerline 在 map.json 中的出现顺序遍历
2. **way id 严格递增**：每个 lane 的 left way 先于 right way
3. **relation id 严格递增**：按 lane id 字典序
4. **属性顺序固定**：见 §2.3 / §2.4
5. **双精度固定 6 位小数**：`f"{x:.6f}"`，**不**用 `str(x)`
6. **空字段跳过**：缺值的 tag 整个 `<tag>` 不写（不写空字符串 `<tag k="x" v="" />`）
7. **traffic_light 必须在所有 lane relation 之后**（单独分组）

---

## 5. D2-09 consistency gate 期望的解析规则

D2-09 实现需满足以下接口（agent C 实现时严格遵守）：

```python
# 伪代码
def parse_lanelet_osm(osm_path: Path) -> dict[str, dict]:
    """
    返回 {lanelet_id: {left_node_ids, right_node_ids, lat, lon, attrs, ...}}
    """
    ...

def check_consistency(map_json_path: Path, osm_path: Path) -> list[str]:
    """
    返回错误列表（空 = 通过）。
    检查：
    1. 每个 map.json lane 在 .osm 里有对应 relation（按 lanelet_id）
    2. 每个 .osm lanelet relation 在 map.json 里有对应 lane
    3. centerline 起点坐标偏差 ≤ 1.0m（map.json x/y 直比 .osm lat/lon 当作同一坐标）
    """
    ...
```

D2-09 gate CLI 接口：

```bash
python3 ci/gates/lanelet_consistency_check.py [--map maps/<name>/map.json --osm maps/<name>/lanelet.osm]
# 默认扫所有 maps/*/{map.json,lanelet.osm}
```

退出码：0 = 全绿，1 = 有 mismatch，2 = map.json 缺对应 .osm 或反过来。

---

## 6. 测试用例（D2-02 单测覆盖）

`tests/test_json_to_lanelet.py` 必须覆盖：

| 用例 | 输入 | 断言 |
|---|---|---|
| 直道 | 手工造 1 条 road 2 lane | 2 个 relation，left/right way 节点数 = centerline 节点数 |
| 弯道 | 手工造 1 条 road 含 curvature | 同上，坐标不是直线 |
| 单向 vs 双向 | 两条 road：oneway=true/false | `one_way` tag 分别为 yes/no |
| 含 traffic_light | 1 road + 1 traffic_light | 1 个 regulatory_element relation，subtype=traffic_light |
| 字节稳定性 | 同 map.json 跑两次 | 输出 bytes 相等 |
| 缺字段 | speed_limit 缺失 | 该 tag 不写 |
| 大地图 | `maps/osm_lujiazui_v2/map.json` | 2738 roads → 转换成功 |

---

*文档版本：v1.0（与 REQ_L3_DIR2_HDMAP.md v1.0 对齐，agy 四轮 10/10 通过）*
*下一步：agent B / C 据此实现，agent A 据此文档化 submodule*
