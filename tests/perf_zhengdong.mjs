/**
 * perf_zhengdong.mjs — 郑东全量构建耗时测试
 * 验证提高 LARGE_MAP_ROAD_LIMIT 后全量渲染的可行性（CPU 建图耗时）。
 */
import { readFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { createConnectorView } from '../tools/flowboard/js/vis/view/ConnectorView.js';
import { createRoadView } from '../tools/flowboard/js/vis/view/RoadView.js';
import { getTopology } from '../tools/flowboard/js/vis/model/TopologyModel.js';
import { isInternalRoad } from '../tools/flowboard/js/vis/model/MapData.js';

const map = JSON.parse(readFileSync(new URL('../maps/osm_zhengdong/map.json', import.meta.url), 'utf8'));
const roads = map.roads.filter((r) => !isInternalRoad(r));
console.log(`郑东全量 non-internal roads: ${roads.length}`);

const t0 = performance.now();
const edges = roads.map((road, index) => {
  const lanes = Array.isArray(road.lanes) ? road.lanes : [];
  return {
    id: road.id || index, name: road.id || `road_${index}`, type: road.type || 'road',
    lanes: lanes.length || road.lanes || 2,
    lane_width: lanes[0] && lanes[0].width ? lanes[0].width : 3.5,
    length: road.length_m || 0,
    nodes: (road.centerline || road.nodes || []).map((p) => [p[0] || 0, p[1] || 0, p[2] || 0]),
    oneway: road.oneway === true, speed_limit: road.speed_limit,
  };
});
const mapJunctions = Array.isArray(map.junctions) ? map.junctions : [];
const laneData = {};
for (const road of map.roads) {
  if (road && road.id && Array.isArray(road.lanes) && road.lanes.length) laneData[road.id] = road.lanes;
}
console.log(`build edges: ${(performance.now() - t0).toFixed(0)}ms`);

const t1 = performance.now();
const topo = getTopology({ edges, map_junctions: mapJunctions, lane_data: laneData });
console.log(`getTopology: ${(performance.now() - t1).toFixed(0)}ms centers=${topo.centers.length}`);

const t2 = performance.now();
const scene = new THREE.Group();
createConnectorView(scene).build({ edges, map_junctions: mapJunctions, lane_data: laneData });
console.log(`ConnectorView.build: ${(performance.now() - t2).toFixed(0)}ms meshes=${scene.children.length}`);

const t3 = performance.now();
const scene2 = new THREE.Group();
createRoadView(scene2).build({ edges, map_junctions: mapJunctions, lane_data: laneData });
console.log(`RoadView.build: ${(performance.now() - t3).toFixed(0)}ms`);
