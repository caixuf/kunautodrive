/**
 * render_zhengdong.mjs — 渲染郑东路网某区域，检查道路断裂
 */
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { createRoadView } from '../tools/flowboard/js/vis/view/RoadView.js';
import { createConnectorView } from '../tools/flowboard/js/vis/view/ConnectorView.js';
import { isInternalRoad } from '../tools/flowboard/js/vis/model/MapData.js';
import { Raster } from './support/png.js';

const map = JSON.parse(readFileSync(new URL(process.env.ZD_MAP || '../maps/osm_zhengdong/map.json', import.meta.url), 'utf8'));
const roads = map.roads.filter((r) => !isInternalRoad(r));
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

console.log('building views...');
const scene = new THREE.Group();
createRoadView(scene).build({ edges, map_junctions: mapJunctions, lane_data: laneData });
createConnectorView(scene).build({ edges, map_junctions: mapJunctions, lane_data: laneData });
console.log('meshes:', scene.children.length);

// ── dump 所有 mesh 颜色 + 三角形数（找路面去哪了）──
const meshInfo = {};
scene.traverse((ch) => {
  if (!ch.isMesh || ch.isInstancedMesh) return;
  const geo = ch.geometry;
  if (!geo || !geo.attributes.position) return;
  const col = ch.material && ch.material.color ? ch.material.color.getHex() : 0;
  const indexBuf = geo.index ? geo.index.array.length : 0;
  const vertCount = geo.attributes.position.count;
  const triN = indexBuf ? indexBuf / 3 : vertCount / 3;
  meshInfo[col] = (meshInfo[col] || 0) + triN;
});
const sorted = Object.entries(meshInfo).sort((a, b) => b[1] - a[1]);
console.log('mesh color → triangles (sorted desc):');
for (const [k, v] of sorted) console.log(`  0x${Number(k).toString(16).padStart(6, '0')} → ${v} tris`);

// ── 渲染路面（提亮）确认有无 gap ──
const CX = -1000, CZ = -1000, PAD = 800, METER = 2.5;
const x0 = CX - PAD, x1 = CX + PAD, z0 = CZ - PAD, z1 = CZ + PAD;
const W = Math.round((x1 - x0) * METER), H = Math.round((z1 - z0) * METER);
const r = new Raster(W, H);
for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) r.set(x, y, [25, 28, 33]);
const sx = (x) => (x - x0) * METER;
const sy = (z) => (z - z0) * METER;

let drawn = 0;
const fillMap = { 0x24262b: [210, 210, 220], 0xcccccc: [180, 180, 180], 0xffd700: [220, 190, 60] };
scene.traverse((ch) => {
  if (!ch.isMesh || ch.isInstancedMesh) return;
  const geo = ch.geometry;
  if (!geo || !geo.attributes.position) return;
  const col = ch.material && ch.material.color ? ch.material.color.getHex() : 0;
  const fill = fillMap[col];
  if (!fill) return;
  const pos = geo.attributes.position.array;
  const idx = geo.index ? geo.index.array : null;
  const triCount = idx ? idx.length / 3 : pos.length / 9;
  for (let t = 0; t < triCount; t++) {
    const i0 = idx ? idx[t * 3] * 3 : t * 9;
    const i1 = idx ? idx[t * 3 + 1] * 3 : t * 9 + 3;
    const i2 = idx ? idx[t * 3 + 2] * 3 : t * 9 + 6;
    const p0 = [sx(pos[i0]), sy(pos[i0 + 2])];
    const p1 = [sx(pos[i1]), sy(pos[i1 + 2])];
    const p2 = [sx(pos[i2]), sy(pos[i2 + 2])];
    const cx = (p0[0] + p1[0] + p2[0]) / 3, cy = (p0[1] + p1[1] + p2[1]) / 3;
    if (cx < -5 || cx > W + 5 || cy < -5 || cy > H + 5) continue;
    r.fillPoly([p0, p1, p2], fill);
    drawn++;
  }
});
console.log(`drawn tris: ${drawn}`);
mkdirSync(new URL('../tests/out', import.meta.url), { recursive: true });
writeFileSync(new URL('../tests/out/zhengdong_road.png', import.meta.url), r.encode());
console.log('saved tests/out/zhengdong_road.png');
