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
const ALLOWED = new Set(['city_ring', 'city_center', 'city_grid', 'osm_test', 'osm_lujiazui']);
/* scenario_name 与 map 目录名不一致的别名表 */
const ALIAS = { osm_city_map: 'osm_test' };

let _loadedMapId = null;   // 已成功加载的 map id
let _inflight = false;     // 有 fetch 进行中
let _failed = new Set();   // 拉取失败过的 map id（会话内不再重试，防 404 刷请求）
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

/** 由 map.json 构建消费索引：junctions 原样 + laneData 按 road.id 键控。 */
function buildIndex(map) {
  const rn = (map && map.road_network) || map || {};
  const roads = Array.isArray(rn.roads) ? rn.roads : [];
  const laneData = {};
  for (const r of roads) {
    if (r && r.id && Array.isArray(r.lanes) && r.lanes.length) laneData[r.id] = r.lanes;
  }
  const junctions = Array.isArray(rn.junctions) ? rn.junctions : [];
  return { junctions, laneData };
}

/** 取当前场景的权威地图数据：已加载 → {junctions, laneData}；
 *  未加载 → 触发一次性 fetch 并返回 null（下帧再来问）。
 *  非 OSM 场景 / 拉取失败 → 永久 null（调用方走兜底）。 */
export function mapDataForScenario(scenarioName) {
  const id = resolveMapId(scenarioName);
  if (!id || _failed.has(id)) return null;
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
      } else {
        _failed.add(id);
      }
    })
    .catch(() => { _failed.add(id); })
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
  _failed = new Set();
  _data = null;
}
