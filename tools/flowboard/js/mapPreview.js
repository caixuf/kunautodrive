import './bootstrap.js';
import { init3DScene, resize3D, update3D, setCameraMode, resetMapView } from './vis/main.js';

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
  const heading = Math.atan2(Number(second[1] || 0) - Number(first[1] || 0), Number(second[0] || 0) - Number(first[0] || 0));
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
  const chain = route && Array.isArray(route.road_chain) ? route.road_chain : null;
  const roads = (map.roads || []).filter((road) => !chain || chain.includes(road.id));
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
  const focus = routeFocus(routePath);
  const first = routePath[0] || (edges[0] && edges[0].nodes[0] ? edges[0].nodes[0] : [0, 0, 0]);
  return {metrics: {scene: {
    t_us: 0,
    lighting: 'day',
    road_network: {edges},
    ego: {type: 'ego', id: 0, x: focus.centerX, y: focus.centerY, z: focus.centerZ,
      heading: focus.heading, speed: 0, vx: 0, vy: 0, length: 4.6, width: 2,
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
      hint.textContent = (route ? route.name + ' · ' : '') + '左键拖拽平移，滚轮缩放';
    }
    update3D(toTopo(result.map, result.routes.routes || [], routeId));
    setCameraMode('map');
    resetMapView();
    resize3D();
    if (msg) msg.remove();
  } catch (error) {
    if (msg) msg.textContent = `预览失败：${error.message}`;
    console.error('[map-preview]', error);
  }
}

window.addEventListener('resize', resize3D);
boot();
