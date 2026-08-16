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

/* monitor_server /api/map/preview 的 allowlist 地图（新增地图需同步两侧） */
const ALLOWED = new Set(['city_ring', 'city_center', 'city_grid', 'osm_test', 'osm_lujiazui', 'osm_lujiazui_v2', 'osm_zhengdong']);
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

/** scenario_name → allowlist map id（不匹配返回 null = 无权威地图） */
export function resolveMapId(scenarioName) {
  if (!scenarioName) return null;
  const name = String(scenarioName);
  if (ALLOWED.has(name)) return name;
  if (ALIAS[name]) return ALIAS[name];
  const stripped = name.replace(/_map$/, '');
  return ALLOWED.has(stripped) ? stripped : null;
}

/** 由 map.json 构建消费索引：junctions 原样 + laneData 按 road.id 键控
 *  + edges（road 级，scene/frame 省略静态段时供 SceneDirector 合成 roadNetwork）
 *  + buildings（OSM 建筑轮廓，BuildingView 渲染）。 */
function buildIndex(map) {
  const rn = (map && map.road_network) || map || {};
  const roads = Array.isArray(rn.roads) ? rn.roads : [];
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
    if (r.sumo_id && String(r.sumo_id).startsWith(':')) continue;
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
    });
  }
  const junctions = Array.isArray(rn.junctions) ? rn.junctions : [];
  const buildings = Array.isArray(rn.buildings) ? rn.buildings : [];
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
        _data = buildIndex(res.map);
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
