import { init3DScene, resize3D, update3D, setCameraMode, setPerfTier } from './vis/main.js';

function toTopo(map, routes, routeId) {
  const route = routes.find((item) => item.id === routeId);
  const chain = route && Array.isArray(route.road_chain) ? route.road_chain : null;
  const roads = (map.roads || []).filter((road) => !chain || chain.includes(road.id));
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
  const first = edges[0] && edges[0].nodes[0] ? edges[0].nodes[0] : [0, 0, 0];
  return {metrics: {scene: {
    t_us: 0,
    lighting: 'day',
    road_network: {edges},
    ego: {type: 'ego', id: 0, x: first[0], y: first[1], z: first[2],
      heading: 0, speed: 0, vx: 0, vy: 0, length: 4.6, width: 2},
    entities: [],
  }}};
}

async function boot() {
  const params = new URLSearchParams(location.search);
  const mapId = params.get('map') || 'city_ring';
  const routeId = params.get('route') || 'main';
  const msg = document.getElementById('msg');
  try {
    init3DScene(document.getElementById('scene3d'));
    // Preview is isolated from the live dashboard, so use the full downtown
    // asset budget and let the existing software-renderer guard downgrade it.
    setPerfTier('high');
    const response = await fetch('/api/map/preview', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({map: mapId}),
      cache: 'no-store',
    });
    const result = await response.json();
    if (!response.ok || !result.ok) throw new Error(result.error || '地图加载失败');
    update3D(toTopo(result.map, result.routes.routes || [], routeId));
    setCameraMode('map');
    resize3D();
    if (msg) msg.remove();
  } catch (error) {
    if (msg) msg.textContent = `预览失败：${error.message}`;
    console.error('[map-preview]', error);
  }
}

window.addEventListener('resize', resize3D);
boot();
