# 卷五 · 第五课：地图引擎——路从哪来

## 本章问题

你的车学会超车了（卷三），你在一条像样的世界里练过了（卷四）。但这个「世界」里的路，
是**手画的**——一条直路，两条车道。

现在问题来了：**真实的路，从哪来？**

你要的不是手画的直路，而是**一条真实的街道**——有弯道、有路口、有高架、有隧道、有
建筑的那种。你不能手画一座城。这一章讲：**怎么把真实世界的路，变成车能开、能渲染的
数据。**

## 你自己的答案：手写坐标

最朴素的想法：把路画成一系列坐标点，写进文件。

```json
{
  "roads": [
    { "id": "main", "nodes": [[0,0], [100,0], [200,50]], "lanes": 2 }
  ]
}
```

看，一条带弯的路出来了。但画到第十条路，你会撞墙：

1. **手写不了城市**：北京国贸的每条路、每个路口、每栋楼——手写？不可能。
2. **高度全 0**：你画的每条路 `z=0`，但真实世界有高架、有隧道。没有高度，立体路网
   全被压成平面——卷五开头那个「桥隧塌平」问题。
3. **不连通**：你画的路和路之间，谁接谁？手工维护 `successors`（后继关系）？连错一条，
   车就开不出去。

**结论：地图必须「生成」，不能「手写」。**

## 真正的方案：一条从真实世界到数据的流水线

项目的地图生成是一条单向流水线，每一步只做一件事：

```
OSM（真实街道数据）
   |  osm2kmap（适配器）
   v
.kmap（声明式地图语言：我想要的"路长这样"）
   |  map_compiler（唯一的编译器）
   v
map.json（运行时契约：车能读、渲染器能画）
```

### 第一步：OSM——真实世界的「原料」

OpenStreetMap（OSM）是免费的全球地图数据：每一条路、每一栋楼、每一个路口，都有人
标好了。你想要的「北京国贸」就在里面。OSM 是**原料**——它原始、庞杂、充满不一致，
不能直接给车用，但它是「真实」的来源。

OSM 数据格式是 PBF（Protocol Buffer Binary Format），包含：
- **way**：一条路的几何折点 + 标签（highway 类型、名称、车道数、限速……）
- **node**：路的端点（经纬度坐标）
- **relation**：路之间的拓扑关系（同向车道、分隔带……）

项目用 `osm_bbox_clip.py` 按 bbox 裁剪区域，再用 SUMO `netconvert` 把 OSM 转成
车道级几何（`net.xml`），最后 `osm2kmap` 翻译成 .kmap。

### 第二步：.kmap——「我想要的」和「数据里的」的中间语言

这里有一个关键设计：**为什么不直接从 OSM 生成 map.json？**

因为 OSM 是「脏数据」：一条路可能被切成 50 段、一个路口可能有 8 条内部连接、高度
信息散落各处。如果你直接 OSM -> map.json，那么「数据长什么样」就直接决定了「代码长
什么样」——OSM 一变，你的逻辑就乱。

所以项目引入中间层 **.kmap**：它描述的是**你想要的**路（「一条 4 车道的城市主干道」），
而不是 OSM 里那条被切碎的线。`osm2kmap` 负责把脏的 OSM 翻译成干净的 .kmap；你想要的
任何路（手写的也行、程序生成的也行）都先变成 .kmap。

```
OSM data ---osm2kmap---> .kmap <--- hand-written / procedural
                           |
                     map_compiler (single compiler)
                           v
                       map.json
```

**这就是「DSL 单一枢纽」**：任何来源想进运行时，都必须先变成 .kmap，再经**唯一的**
`map_compiler.py` 编译。好处：你永远只有一个地方决定「.kmap 长什么样」，改一个编译器，
所有来源同步升级。**多入口直写 map.json 的时代（四个脚本各写各的），被判定为架构错误，
废弃了。**

#### .kmap DSL 语法

```
Map {
    id: osm_zhengdong
    name: "OSM Zhengdong"

    Road main_road {
        type: urban
        lanes: 4
        laneWidth: 3.5
        speedLimit: 15.0
        oneWay: false
        Point { x: 0; y: 0; z: 0 }
        Point { x: 180; y: 0; z: 0 }
        Elevation { s: 0; z: 0 }
    }

    Connection { from: main_road; to: side_street; type: continue }
}
```

Road 块声明一条路：几何点（Point）、车道数、类型、限速。Connection 块声明路之间的
拓扑连接。Lane 块可选，覆盖自动派生的车道属性。

### 第三步：map.json——运行时唯一认识的东西

`map_compiler` 把 .kmap 编译成 `map.json`：每条路的中心线、每条车道、车道之间的
后继关系、红绿灯、建筑。它是**下游唯一认识的东西**——flowsim 用它跑车，flowboard
用它渲染，A* 用它规划路径。它不关心 map.json 来自 OSM 还是手写。

## 一个关键几何约定：centerline 不是路中线

**map.json 里最值得注意的一个几何约定**：`road.centerline` 是**最左车道的左边线**，
不是道路中线；每条 `lane.centerline` 是它**自己**的中心线。

为什么这么别扭？因为这是从 SUMO（交通仿真软件）继承来的约定。在 SUMO 里，每个
direction 的 edge 的 centerline 是该方向最左车道的左边界。两个方向的 edge 拼在一起时，
它们的 centerline 分别是各自方向的左边界，中间是双黄线。

```
              centerline_A (A方向最左车道左边界)
              |
   <--- A方向行驶 ---|--- B方向行驶 --->
              |
              centerline_B (B方向最左车道左边界)
```

前端 `RoadAxis.js` 用 `laneGroupEnvelope()` 从 lane centerline 反推真实的车道组中心：
测量各 lane centerline 相对 road centerline 的横向偏移，取中位数作为修正量。

**记住：地图数据里的「中心线」可能不是你直觉里的中心线——读契约前先看文档。**

## 车道标线：GB 5768 标准

每条 lane 的 `markings` 字段描述车道边界的标线类型：

| 标线类型 | 含义 | 出现位置 |
|---------|------|---------|
| `double_yellow` | 双黄实线（对向分隔） | 双向道路的中央分隔线 |
| `solid_white` | 白色实线（不可变道） | 道路外缘、公交专用道边界 |
| `dashed_white` | 白色虚线（可变道） | 同向车道分隔线 |

标线生成规则（`osm2kmap.py emit_road`）：
- lane.1（最左车道）左侧：双向路 = `double_yellow`，单向路 = `solid_white`
- lane.N（最右车道）右侧：`solid_white`
- 其他车道分隔：`dashed_white`

前端 `RoadView.js` 消费这些标线：`buildLaneMarkingsInto()` 从 lane centerline 偏移
出标线位置，虚线按 3m 实线 + 6m 间隔铺设。

## 路口处理：fork + internal connector

### SUMO 的路口模型

SUMO 在路口内部生成**内部连接器**（`internal` edge），连接进入方向和离开方向。
每个 connector 携带：
- 转向类型（straight / left / right / uturn）
- 车道级几何（精确的转弯曲线）
- lane successors（从进入车道 → connector 车道 → 离开车道的链路）

### map.json 的 fork 契约

osm2kmap 把路口拓扑输出为 `junctions[]`：

```json
{
  "id": 100,
  "type": "fork",
  "incoming_road": "road_r81212673s1",
  "connecting_roads": [
    { "id": "road_j10299667962_2", "turn": "straight" },
    { "id": "road_j10299667962_3", "turn": "uturn" }
  ]
}
```

`incoming_road` 是进入路口的真实道路。`connecting_roads` 是路口内部 connector（road_j*），
每个带 `turn` 标签。

### 前端如何消费 fork 数据

`ConnectorView.js` 的 `connectingRoads()` 需要把 fork 记录里的 connector 匹配到
渲染用的 arm（真实道路端点）。问题是：connector 的 id（road_j*）不在 edges 列表里
（internal roads 被过滤掉了），直接 `armIdx.get(conn.id)` 永远是 null。

**修复方案**：通过 connector 的 `lane.successors` 反向查找目标真实道路：

```
connector road_jXXX
  -> lane.successors[0] = "road_r81212673s0.lane.1"
  -> base road = "road_r81212673s0"
  -> armIdx.get("road_r81212673s0") = 目标 arm 索引
```

这样 fork 的转向导流线（左转虚线引导）就能正确渲染了。

## 高度处理：高架与隧道

还记得你自己的答案里「高度全 0」吗？真实世界有高架和隧道，这必须体现在数据里。

```
桥梁：z = +6.0 x max(1, layer) 米，标记 road.bridge = true
隧道：z = -4.0 米，标记 road.tunnel = true
```

高度从哪来？OSM 里直接查。`osm2kmap` 通过 Overpass API 按路的 way ID 查桥/隧/层数
元数据，写进 .kmap，编译进 map.json。每个桥隧路的所有中心线点和 lane centerline 点的
z 分量都统一设为该高度值（整条路抬升/下沉，不做逐点渐变）。

**为什么高度这么重要？** 回想卷四的 invariant——「每辆车离路面高度 < epsilon」。如果地图
没有高度，车永远在地面上，invariant 测不出「浮空」。但真实世界有高架，没有高度信息，
你的车就会**从高架底下穿过去**——看起来像穿模，实际是数据没告诉它上面有路。

**已知问题**：桥头/隧道口的高度是硬跳变（0 -> 6m），没有匝道渐变过渡段。
456/802 个桥端点的邻居在不同高度层，渲染时会看到垂直悬崖。这是数据层面的已知限制，
需要在 OSM 管线中补充 ramp 过渡段。

## 从「一张图」到「一条路线」：A*

地图有了，但车要**从 A 开到 B**。谁来选路？

大地图（比如一座城的网格，1300 段路）不能靠手写路线列表——你不可能手动把北京国贸到
朝阳大悦城之间的每条路按顺序列出来。答案是 **A* 搜索**：把车道当成图的节点，车道之间的
`successors` 当成边，跑 A* 求最短路径。

```
map.json (lanes + successors) ---> build graph ---> A*(start_lane, end_lane) ---> route (road chain)
```

A* 的启发式用「当前车道中点到终点车道中点的直线距离」——可采纳，保证找到的是最优。
**这条路的容量问题真实发生过**：大地图有 5200 条车道，最初用定长数组存图直接爆栈
（1.3MB），后来改成堆分配 + 上限提到 8192。**「能跑通」和「能跑完大地图」是两回事，**
扩容是迟早要做的。

## 怎么保证地图没错：纯数据的门禁

地图是**纯数据**——错了不会编译报错，只会「车开不动」或「渲染是黑的」。所以有一整套
专门的门禁：

```bash
# 车道链连通性：每条路线，相邻路段之间必须真有车道连接
python3 tools/check_map_connectivity.py

# OpenDRIVE 转换正确性（地图要转成 esmini 能读的格式）
python3 tools/xodr_compat_check.py --runtime-test test_road_network
```

`check_map_connectivity` 特别讲一个设计：**已知断链白名单**。有些历史地图的某处断链是
「已知的数据质量问题」（比如 OSM 数据本身在某个点不连续），在 `KNOWN_BROKEN` 里列出，
命中时 WARN 不阻断；白名单外的任何断链都 ERROR 阻断。**这样既不让历史债挡住合并，
又保证「新断链必被抓」。**

## 现有地图一览

| 地图 | 类型 | 路段数 | 特点 |
|------|------|--------|------|
| `city_grid` | 程序化 | ~1300 | 9x9 网格，2600 junction，测试用 |
| `city_center` | 程序化 | ~200 | 城市中心环路 |
| `city_ring` | 程序化 | ~100 | 简单环路 |
| `osm_test` | OSM | ~500 | 小区域测试 |
| `osm_lujiazui_v2` | OSM | ~5000 | 上海陆家嘴，含建筑 |
| `osm_zhengdong` | OSM | ~42000 | 郑东新区，含桥隧高程 |

## 怎么加一张新地图

1. **获取 OSM 数据**：用 `osm_bbox_clip.py` 按 bbox 裁剪区域
2. **运行 netconvert**：OSM -> SUMO net.xml
3. **运行 osm2kmap**：net.xml -> .kmap
4. **运行 map_compiler**：.kmap -> map.json
5. **创建 scenario**：`scenarios/<map_id>.json` 引用地图
6. **验证连通性**：`check_map_connectivity.py --map maps/<map_id>`
7. **预览**：`python3 tools/preview_map.py --map <map_id>`

```bash
# 完整示例
python3 tools/osm_bbox_clip.py --bbox 34.75,113.65,34.78,113.68 \
    --ref-lat 34.765 --ref-lon 113.665 -o /tmp/zhengdong.osm.pbf
netconvert --osm-files /tmp/zhengdong.osm.pbf -o /tmp/zhengdong.net.xml
python3 tools/osm2kmap.py /tmp/zhengdong.net.xml \
    --out maps/osm_zhengdong --ref-lat 34.765 --ref-lon 113.665
python3 tools/map_compiler.py maps/osm_zhengdong/map.kmap \
    -o maps/osm_zhengdong/map.json
python3 tools/check_map_connectivity.py --map maps/osm_zhengdong
```

## 动手实践

```bash
# 1. 看一张现成地图长什么样
python3 -c "
import json
m = json.load(open('maps/osm_lujiazui_v2/map.json'))
print('roads:', len(m['roads']))
print('junctions:', len(m['junctions']))
r = m['roads'][0]
print('first road:', r['id'], 'lanes:', len(r['lanes']))
print('centerline points:', len(r['centerline']))
"

# 2. 编译一张地图（从 .kmap 到 map.json）
python3 tools/map_compiler.py maps/examples/city_ring_minimal.kmap \
    -o /tmp/my_map.json

# 3. 检查连通性
python3 tools/check_map_connectivity.py --map maps/osm_lujiazui_v2

# 4. 预览地图（打开浏览器看 3D 效果）
python3 tools/preview_map.py --map osm_lujiazui_v2
```

## 常见陷阱

1. **centerline 不是路中线**：road.centerline 是最左车道左边界，lane centerline 才是
   车道自己的中心。前端用 `laneGroupEnvelope` 修正，但你读数据时要注意。
2. **edge.type 决定渲染**：写 `viaduct_highway` 会触发高架渲染，平路场景禁用。
3. **internal connector 不渲染**：road_j* 只用于拓扑（successors/turn），不进 edges。
4. **高度是整条路常量**：不是逐点渐变。桥头到地面是 6m 硬跳变。
5. **地图文件很大**：osm_zhengdong map.json 76MB，加载需要时间。预览时走 corridor
   过滤（只保留路线附近 800m）。

## 小结

这一章你学会了地图是怎么来的：

- **地图要生成，不能手写**：真实街道来自 OSM，但 OSM 是脏数据，需要中间语言。
- **DSL 单一枢纽**：任何来源先进 .kmap，唯一编译器出 map.json——一个创作入口，
  一处升级，全链同步。
- **centerline 约定**：road.centerline = 最左车道左边界（非路中线），SUMO 继承。
- **标线按 GB 5768**：double_yellow 对向分隔、solid_white 外缘、dashed_white 同向分隔。
- **路口用 fork + connector**：fork 记录转向关系，connector 携带真实几何。
- **高度必须进数据**：高架 z=6x层、隧道 z=-4，否则车从高架下穿过。
- **A* 在车道图上选路**，大地图要扩容（堆分配 + 上限 8192）。
- **纯数据要靠门禁**：check_map_connectivity + xodr + 已知断链白名单。

## 练习（选做）

1. **读数据**：打开 `maps/osm_lujiazui_v2/map.json`，数一数有几段路、几条车道。
   找到一段路的 `lanes[0].successors`，看它指向哪条 lane——那是「这条路能开到哪」。
2. **思考题**：为什么地图要有「唯一编译器」而不是多个脚本各写 map.json？提示：四个
   入口各写各的，高度信息一个写了 0 一个写了 6，最后谁对？
3. **挑战**：A* 的启发式用「直线距离」，但它低估了实际代价（路不是直的）。如果低估
   导致 A* 找到的不是最优，你怎么修？（提示：A* 的启发式需要满足什么性质才保证最优？
   现实里大家通常怎么取舍？）
4. **进阶**：用 `osm_bbox_clip.py` 裁一小块地图，走完整管线生成 map.json，然后
   `preview_map.py` 看效果。注意观察：junction patch 是否对齐？标线是否连续？

---

**下一卷预告**：路有了、世界有了、车会开了。但你看不见它们——到目前为止，所有东西
都在内存和 JSON 里。卷六把这一切**画**出来：可视化与 3D 渲染。
