/** MapData.js — OSM 权威地图一次性加载（P2 车道级渲染数据通道）
 *
 * live 仪表盘路径问题：scene/frame 每帧只带 road 级 road_network（lane 几何
 * ~590KB + junctions ~382KB 不适合 20Hz 透传）。车道级 lanes/junctions 由本
 * 模块按 scenario_name 经既有 POST /api/map/preview（gzip 静态服务）拉一次
 * 并缓存，由 SceneDirector 注入 roadNetwork（map_junctions / lane_data）。
 *
 * 数据到达会改变 roadNetworkHash（含注入标志位）→ 恰好触发一次 view 重建；
 * 非 OSM 场景 resolveMapId 返回 null，全程走启发式兜底（零回归）。
 */

/* scenario_name 与 map 目录名不一致的别名表 */
const ALIAS = { osm_city_map: 'osm_test' };

let _loadedMapId = null;   // 已成功加载的 map id
let _inflight = false;     // 有 fetch 进行中
/* 拉取失败冷却（替代旧版永久 _failed）：失败后冷却期内不再重试（防 404 刷请求），
 * 冷却过后自动重试——服务器短暂不可用 / CWD 未就绪等瞬时故障可自行恢复，
 * 不再永久回退到"无 lanes → 路居中左缘 → 全图左偏"。 */
const RETRY_COOLDOWN_MS = 5000;
let _failedAt = new Map();  // mapId -> 上次失败时间戳
let _data = null;          // {junctions: Array, laneData: {roadId: lanes[]}}

const LARGE_MAP_ROAD_LIMIT = 5000;
const PREVIEW_CORRIDOR_CELL_M = 800;   // 扩大走廊范围至 800m，显示更多远处道路细节
const BUILDING_CORRIDOR_M = 800;       // 建筑也保留至 800m 走廊内

/** SUMO 内部连接器在 map_compiler 后可能没有 sumo_id，road_j 前缀是当前
 * 编译产物保留下来的稳定语义。connector 仍进入 laneData 供导航消费，
 * 但绝不能进入可见道路 edges。 */
export function isInternalRoad(road) {
  return !!road && (road.internal === true ||
    (road.sumo_id && String(road.sumo_id).startsWith(':')) ||
    String(road.id || '').startsWith('road_j'));
}

/** 建筑走廊过滤：只保留 centroid 距任一选中道路中心线折线 < BUILDING_CORRIDOR_M
 * 的建筑。与 selectRoadsForPreview 同走廊语义，防止数万栋全城楼进渲染。
 * footprint 质心优先，缺失用 b.x/b.y。 */
export function selectBuildingsForPreview(buildings, roads) {
  if (!Array.isArray(buildings) || !buildings.length) return buildings || [];
  if (!Array.isArray(roads) || !roads.length) return buildings;
  const cell = 100;
  const grid = new Map();
  const cellKey = (x, y) => `${Math.floor(x / cell)},${Math.floor(y / cell)}`;
  const addSeg = (x0, y0, x1, y1) => {
    const seg = { x0, y0, x1, y1 };
    const minX = Math.min(x0, x1), maxX = Math.max(x0, x1);
    const minY = Math.min(y0, y1), maxY = Math.max(y0, y1);
    for (let cx = Math.floor(minX / cell); cx <= Math.floor(maxX / cell); cx++) {
      for (let cy = Math.floor(minY / cell); cy <= Math.floor(maxY / cell); cy++) {
        const k = cx + ',' + cy;
        let bucket = grid.get(k);
        if (!bucket) { bucket = []; grid.set(k, bucket); }
        bucket.push(seg);
      }
    }
  };
  for (const road of roads) {
    const pts = (road && (road.centerline || road.nodes)) || [];
    for (let i = 1; i < pts.length; i++) {
      const a = pts[i - 1], b = pts[i];
      if (Array.isArray(a) && Array.isArray(b)) {
        addSeg(Number(a[0]) || 0, Number(a[1]) || 0, Number(b[0]) || 0, Number(b[1]) || 0);
      }
    }
  }
  const near = (x, y) => {
    const cx = Math.floor(x / cell), cy = Math.floor(y / cell);
    let best = Infinity;
    for (let dx = -1; dx <= 1; dx++) {
      for (let dy = -1; dy <= 1; dy++) {
        const segs = grid.get((cx + dx) + ',' + (cy + dy));
        if (!segs) continue;
        for (const s of segs) {
          const abx = s.x1 - s.x0, aby = s.y1 - s.y0;
          const len2 = abx * abx + aby * aby || 1e-9;
          const t = Math.max(0, Math.min(1, ((x - s.x0) * abx + (y - s.y0) * aby) / len2));
          const d = Math.hypot(x - (s.x0 + abx * t), y - (s.y0 + aby * t));
          if (d < best) best = d;
        }
      }
    }
    return best < BUILDING_CORRIDOR_M;
  };
  return buildings.filter((b) => {
    const fp = Array.isArray(b.footprint) ? b.footprint : null;
    let bx, by;
    if (fp && fp.length) {
      bx = fp.reduce((s, p) => s + (Number(p[0]) || 0), 0) / fp.length;
      by = fp.reduce((s, p) => s + (Number(p[1]) || 0), 0) / fp.length;
    } else {
      bx = Number(b.x) || 0; by = Number(b.y) || 0;
    }
    return near(bx, by);
  });
}

/** 超大地图只取路线周边走廊，避免把整张城市路网当成一帧 3D 细节图。
 * 小地图保持全量；路线本身永远保留，走廊按中心线空间格保留约 250m 邻域。 */
export function selectRoadsForPreview(roads, route) {
  if (!Array.isArray(roads) || roads.length <= LARGE_MAP_ROAD_LIMIT ||
      !Array.isArray(route?.road_chain) || route.road_chain.length === 0) return roads || [];
  const routeIds = new Set(route.road_chain);
  const cells = new Set();
  const cell = PREVIEW_CORRIDOR_CELL_M;
  for (const road of roads) {
    if (!routeIds.has(road?.id)) continue;
    for (const p of (road.centerline || road.nodes || [])) {
      if (Array.isArray(p)) cells.add(`${Math.floor((p[0] || 0) / cell)},${Math.floor((p[1] || 0) / cell)}`);
    }
  }
  if (!cells.size) return roads;
  return roads.filter((road) => {
    if (routeIds.has(road?.id)) return true;
    return (road.centerline || road.nodes || []).some((p) => {
      if (!Array.isArray(p)) return false;
      const cx = Math.floor((p[0] || 0) / cell);
      const cy = Math.floor((p[1] || 0) / cell);
      for (let dx = -1; dx <= 1; dx++) {
        for (let dy = -1; dy <= 1; dy++) {
          if (cells.has(`${cx + dx},${cy + dy}`)) return true;
        }
      }
      return false;
    });
  });
}

/** scenario_name → map id（动态，不再需要硬编码 allowlist） */
export function resolveMapId(scenarioName) {
  if (!scenarioName) return null;
  const name = String(scenarioName);
  // 先查别名表
  if (ALIAS[name]) return ALIAS[name];
  // 直接返回 scenario_name，C端会动态检查 maps/<id>/map.json 是否存在
  return name;
}

/** 由 map.json 构建消费索引：junctions 原样 + laneData 按 road.id 键控
 *  + edges（road 级，scene/frame 省略静态段时供 SceneDirector 合成 roadNetwork）
 *  + buildings（OSM 建筑轮廓，BuildingView 渲染）。 */
function buildIndex(map, routes) {
  const rn = (map && map.road_network) || map || {};
  const roads = selectRoadsForPreview(Array.isArray(rn.roads) ? rn.roads : [],
    Array.isArray(routes?.routes) ? routes.routes[0] : null);
  const laneData = {};
  const edges = [];
  for (let i = 0; i < roads.length; i++) {
    const r = roads[i];
    if (!r || r.id == null) continue;
    /* net2map 把 SUMO 路口内部 connector（sumo_id 以 ':' 开头）也输出成 road。
     * 渲染成路面会在每个路口缠成一团、污染路口几何聚类（树/护栏/路灯槽位判定
     * 跟着错），路口本身由 junction patch 覆盖——故不进 edges（不铺路面）。
     * 但 connector 是 OSM→SUMO 的真实转向车道（lane successors 串起转向），
     * 2026-08-16 起保留其 laneData，供 ConnectorView 在交叉口内部画转向引导线
     * （此前整条跳过 → 交叉处无车道线"混乱"）。 */
    if (Array.isArray(r.lanes) && r.lanes.length) laneData[r.id] = r.lanes;
    if (isInternalRoad(r)) continue;
    const cl = Array.isArray(r.centerline) ? r.centerline : [];
    if (cl.length < 2) continue;
    /* edge schema 对齐 scene_pub.cpp build_road_network_json（FrameValidator
     * 必填：id/name/type/lanes/lane_width/nodes/oneway + length）；
     * name 用字符串 road id —— lane_data 键控依赖它（RoadView laneGroupEnvelope）。 */
    let length = 0;
    for (let k = 0; k < cl.length - 1; k++) {
      length += Math.hypot((cl[k + 1][0] || 0) - (cl[k][0] || 0), (cl[k + 1][1] || 0) - (cl[k][1] || 0));
    }
    edges.push({
      id: i,
      name: String(r.id),
      type: r.type || 'urban',
      lanes: Array.isArray(r.lanes) && r.lanes.length ? r.lanes.length : 2,
      lane_width: Number(r.lane_width) || 3.5,
      length,
      oneway: !!r.oneway,
      speed_limit: r.speed_limit,
      nodes: cl.map((p) => [p[0] || 0, p[1] || 0, p[2] || 0]),
      tunnel: r.tunnel != null,    // elevation_patch 补的 OSM tunnel 标记
      bridge: r.bridge != null,    // elevation_patch 补的 OSM bridge 标记
      detail: r.detail || 'high',  // LOD 分级（corridor_map.py 生成，缺省高细节零回归）
    });
  }
  const junctions = Array.isArray(rn.junctions) ? rn.junctions : [];
  const buildings = selectBuildingsForPreview(
    Array.isArray(rn.buildings) ? rn.buildings : [], roads);
  return { junctions, laneData, edges, buildings };
}

/** 取当前场景的权威地图数据：已加载 → {junctions, laneData}；
 *  未加载 → 触发一次性 fetch 并返回 null（下帧再来问）。
 *  非 OSM 场景 / 拉取失败 → 永久 null（调用方走兜底）。 */
export function mapDataForScenario(scenarioName) {
  const id = resolveMapId(scenarioName);
  if (!id) return null;
  // 冷却期内不重试（防 404 刷请求）；冷却过后自动重试，瞬时故障自恢复。
  if (_failedAt.has(id) && Date.now() - _failedAt.get(id) < RETRY_COOLDOWN_MS) return null;
  if (_loadedMapId === id && _data) return _data;
  if (_inflight) return null;
  _inflight = true;
  fetch('/api/map/preview', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ map: id }),
  })
    .then((r) => (r.ok ? r.json() : null))
    .then((res) => {
      if (res && res.ok && res.map) {
        _data = buildIndex(res.map, res.routes);
        _loadedMapId = id;
        _failedAt.delete(id);
      } else {
        _failedAt.set(id, Date.now());
      }
    })
    .catch(() => { _failedAt.set(id, Date.now()); })
    .finally(() => { _inflight = false; });
  return null;
}

/** 测试/预览直连：已有 map 对象时免 fetch 注入（mapPreview 用）。 */
export function setMapData(mapId, map) {
  _data = buildIndex(map);
  _loadedMapId = mapId;
}

/** 测试辅助：复位全部缓存。 */
export function resetMapData() {
  _loadedMapId = null;
  _inflight = false;
  _failedAt = new Map();
  _data = null;
}
