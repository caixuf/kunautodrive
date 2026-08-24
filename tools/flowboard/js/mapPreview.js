import './bootstrap.js';
import { init3DScene, resize3D, update3D, setCameraMode, resetMapView, setOrbitLeftAction } from './vis/main.js';
import { headingBetweenPoints } from './vis/math/Coord.js';
import { selectRoadsForPreview, selectBuildingsForPreview, isInternalRoad } from './vis/model/MapData.js';

function clonePoint(point) {
  return [Number(point[0] || 0), Number(point[1] || 0), Number(point[2] || 0)];
}

function appendPath(out, points) {
  for (const point of points || []) {
    const next = clonePoint(point);
    const prev = out[out.length - 1];
    if (!prev || Math.hypot(prev[0] - next[0], prev[1] - next[1], prev[2] - next[2]) > 1e-6) {
      out.push(next);
    }
  }
}

function chooseRouteSpine(road, laneDirection) {
  const lanes = Array.isArray(road.lanes) ? road.lanes : [];
  const candidates = lanes
    .filter((lane) => Array.isArray(lane.centerline) && lane.centerline.length >= 2)
    .filter((lane) => laneDirection == null || Number(lane.direction || 0) === Number(laneDirection))
    .sort((a, b) => Number(a.index || 0) - Number(b.index || 0));
  if (candidates.length) return candidates[0].centerline;
  if (Array.isArray(road.centerline) && road.centerline.length >= 2) return road.centerline;
  if (Array.isArray(road.nodes) && road.nodes.length >= 2) return road.nodes;
  return [];
}

function buildRoutePath(map, route) {
  const roadMap = new Map((map.roads || []).map((road) => [road.id, road]));
  const routePath = [];
  const chain = route && Array.isArray(route.road_chain) ? route.road_chain : [];
  const laneDirection = route && route.lane_direction != null ? route.lane_direction : null;
  for (const roadId of chain) {
    const road = roadMap.get(roadId);
    if (!road) continue;
    appendPath(routePath, chooseRouteSpine(road, laneDirection));
  }
  return routePath;
}

/* orbit 模式左键动作切换：按 P / Space 在 旋转 ↔ 平移 间切换，
 * 避免右键平移与浏览器上下文菜单冲突。 */
let _orbitLeftMode = 'rotate';
function initOrbitModeSwitch(hint) {
  window.addEventListener('keydown', function (event) {
    if (event.key === 'p' || event.key === 'P' || event.key === ' ') {
      event.preventDefault();
      _orbitLeftMode = _orbitLeftMode === 'pan' ? 'rotate' : 'pan';
      setOrbitLeftAction(_orbitLeftMode);
      if (hint) {
        hint.textContent = _orbitLeftMode === 'pan'
          ? '左键平移 · 滚轮缩放（按 P/Space 切回旋转）'
          : '左键旋转 · 滚轮缩放（按 P/Space 切换平移）';
      }
    }
  });
  setOrbitLeftAction('rotate');
}

/* 城市核心取景：有真实 OSM 建筑时，相机聚焦到建筑密集区（= 城市核心），
 * 而不是整条 route 的 5km 跨度（含大片郊区/高速走廊，画面显得空旷稀疏）。
 * 用每栋建筑的 footprint 质心（无 footprint 用 x/y）算 bbox；heading 固定 0
 * （城市俯视不随某条路的方向倾斜）。无建筑（旧地图/程序化）回退 route 取景。 */
function buildingFocus(buildings) {
  const pts = [];
  for (const b of buildings) {
    const fp = Array.isArray(b.footprint) ? b.footprint : null;
    if (fp && fp.length) {
      let cx = 0, cy = 0;
      for (const p of fp) { cx += p[0]; cy += p[1]; }
      pts.push([cx / fp.length, cy / fp.length, 0]);
    } else {
      pts.push([b.x || 0, b.y || 0, 0]);
    }
  }
  return { ...routeFocus(pts), heading: 0 };
}

function routeFocus(path) {
  if (!path.length) {
    return { centerX: 0, centerY: 0, centerZ: 0, heading: 0, height: 80 };
  }
  let minX = path[0][0], maxX = path[0][0];
  let minY = path[0][1], maxY = path[0][1];
  let minZ = path[0][2] || 0, maxZ = path[0][2] || 0;
  for (const point of path) {
    minX = Math.min(minX, point[0]); maxX = Math.max(maxX, point[0]);
    minY = Math.min(minY, point[1]); maxY = Math.max(maxY, point[1]);
    minZ = Math.min(minZ, point[2] || 0); maxZ = Math.max(maxZ, point[2] || 0);
  }
  const first = path[0];
  const second = path[1] || first;
  const heading = headingBetweenPoints(
    Number(first[0] || 0), Number(first[1] || 0),
    Number(second[0] || 0), Number(second[1] || 0),
  );
  const span = Math.max(maxX - minX, maxY - minY);
  return {
    centerX: (minX + maxX) / 2,
    centerY: (minY + maxY) / 2,
    centerZ: (minZ + maxZ) / 2,
    heading: Number.isFinite(heading) ? heading : 0,
    height: Math.max(80, Math.min(380, span * 0.72)),
  };
}

function toTopo(map, routes, routeId) {
  const route = routes.find((item) => item.id === routeId);
  /* 小地图保持全量；超大地图由 selectRoadsForPreview 保留路线及其 250m
   * 走廊，避免把整张城市路网一次性作为高精度 3D 细节。route 仍用于取景与轨迹线。
   * SUMO 路口内部 connector（road_j*）只服务导航拓扑，预览同样不渲染为普通道路。 */
  const roads = selectRoadsForPreview(map.roads || [], route)
    .filter((road) => !isInternalRoad(road));
  const routePath = buildRoutePath(map, route || {});
  const edges = roads.map((road, index) => {
    const lanes = Array.isArray(road.lanes) ? road.lanes : [];
    return {
      id: road.id || index,
      name: road.id || `road_${index}`,
      type: road.type || 'road',
      lanes: lanes.length || road.lanes || 2,
      lane_width: lanes[0] && lanes[0].width ? lanes[0].width : 3.5,
      length: road.length_m || 0,
      nodes: (road.centerline || road.nodes || []).map((point) => [
        point[0] || 0, point[1] || 0, point[2] || 0,
      ]),
      oneway: road.oneway === true,
      speed_limit: road.speed_limit,
    };
  });
  /* 取景：有真实建筑 → 聚焦城市核心（建筑密集区）；否则回退 route 全跨度。 */
  const focus = (Array.isArray(map.buildings) && map.buildings.length)
    ? buildingFocus(map.buildings)
    : routeFocus(routePath);
  const first = routePath[0] || (edges[0] && edges[0].nodes[0] ? edges[0].nodes[0] : [0, 0, 0]);
  const second = routePath[1] || (edges[0] && edges[0].nodes[1] ? edges[0].nodes[1] : first);
  const egoHeading = headingBetweenPoints(
    Number(first[0] || 0), Number(first[1] || 0),
    Number(second[0] || 0), Number(second[1] || 0),
  );
  /* P1 路口渠化：map.json 的 junctions[]（fork + connecting_roads[].turn）
   * 透传给 ConnectorView 画转向导流线/按来车归属停止线；无数据时几何兜底。
   * 字段名必须叫 map_junctions 而不是 junctions——junctions 是 {x,y,z,radius}
   * 路口中心格式（JunctionDetect 数据层优先消费），fork 记录没有 x/y/z，
   * 若同名传入会被当成 843 个位于 (0,0) 的路口中心，预览渲染全崩。 */
  const mapJunctions = Array.isArray(map.junctions) ? map.junctions : [];
  /* P2 车道级渲染：roads[].lanes[]（centerline/width/markings）按 road.id
   * 键控注入 road_network.lane_data，RoadView 直接消费（与 live 路径的
   * MapData 注入同形同键）。
   * laneData 必须包含内部 connector（road_j*）的 lanes —— ConnectorView
   * 需要 connector 的 successors 来解析 fork turn 的目标 arm（OSM 大地图
   * fork.connecting_roads 全是 road_j，不在 edges/arms 里，走不通直查）。 */
  const laneData = {};
  for (const road of (map.roads || [])) {
    if (road && road.id && Array.isArray(road.lanes) && road.lanes.length) {
      laneData[road.id] = road.lanes;
    }
  }
  return {metrics: {scene: {
    t_us: 0,
    lighting: 'day',
    /* OSM 真实建筑（单源真相）：透传 map.buildings[]（footprint/height），
     * 与 live showcase 路径（sceneAdapter.js）一致。SceneDirector 读取
     * frame.buildings → rn.buildings → BuildingView 按真实轮廓挤出。
     * 仅保留选中路网走廊内的建筑，防数万栋全城楼进渲染。
     * 不传则 BuildingView 回退到程序化天际线（hash 随机盒体，看起来"胡来"）。 */
    buildings: selectBuildingsForPreview(
      Array.isArray(map.buildings) ? map.buildings : [], roads),
    road_network: {edges, map_junctions: mapJunctions, lane_data: laneData},
    ego: {type: 'ego', id: 0,
      x: Number(first[0] || 0),
      y: Number(first[1] || 0),
      z: Number(first[2] || 0),
      heading: Number.isFinite(egoHeading) ? egoHeading : 0,
      speed: 0, vx: 0, vy: 0, length: 4.6, width: 2,
      map_view_height: focus.height,
      map_view_target_x: focus.centerX,
      map_view_target_y: focus.centerY,
      map_view_target_z: focus.centerZ,
      route_anchor_x: first[0],
      route_anchor_y: first[1]},
    trajectory_path: routePath.map((point) => [point[0], point[1], 0]),
    entities: [],
  }}};
}

async function boot() {
  const params = new URLSearchParams(location.search);
  const mapId = params.get('map') || 'city_ring';
  const routeId = params.get('route') || 'main';
  const msg = document.getElementById('msg');
  const resetBtn = document.getElementById('reset-view');
  const hint = document.getElementById('hint');
  if (resetBtn) {
    resetBtn.addEventListener('click', () => resetMapView());
  }
  try {
    const canvas = document.getElementById('scene3d-canvas');
    if (!canvas || typeof canvas.getContext !== 'function') {
      throw new Error('3D canvas unavailable');
    }
    if (!init3DScene(canvas)) {
      throw new Error('WebGL renderer initialization failed');
    }
    const response = await fetch('/api/map/preview', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({map: mapId}),
      cache: 'no-store',
    });
    const result = await response.json();
    if (!response.ok || !result.ok) throw new Error(result.error || '地图加载失败');
    const route = (result.routes.routes || []).find((item) => item.id === routeId);
    if (hint) {
      hint.textContent = (route ? route.name + ' · ' : '') + '左键旋转 · 滚轮缩放 · 按 P/Space 切换平移';
    }
    update3D(toTopo(result.map, result.routes.routes || [], routeId));
    setCameraMode('orbit');
    resetMapView();
    resize3D();
    initOrbitModeSwitch(hint);
    if (msg) msg.remove();
  } catch (error) {
    if (msg) msg.textContent = `预览失败：${error.message}`;
    console.error('[map-preview]', error);
  }
}

export { toTopo, buildRoutePath, routeFocus, buildingFocus };

if (typeof window !== 'undefined' && document.getElementById('scene3d-canvas')) {
  window.addEventListener('resize', resize3D);
  boot();
}
