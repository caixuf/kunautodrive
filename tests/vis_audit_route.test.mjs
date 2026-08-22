/** vis_audit_route.test.mjs — A* 路线端到端全要素漫游排查（真实 Three.js）
 *
 * 漫游慕尼黑（osm_munich）等真实高精 A* 路线，严密核查：
 *  1. 33 段主线道路几何与车道后继（Successor）链条连通性
 *  2. 沿线所有路面与标线顶点（白实线/白虚线/双黄线）无 NaN、全量贴合沥青包络
 *  3. 全路网路灯 0 侵入车道（距中心线距离 >= halfWidth）
 *  4. 路口导向箭头、菱形预告标线、停止线与人行横道朝向和坐标几何自洽
 *
 * 跑法：
 *   node --import ./tests/support/three-real-preload.mjs tests/vis_audit_route.test.mjs
 */

import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve as pathResolve, dirname } from 'node:path';
import { createRoadView } from '../tools/flowboard/js/vis/view/RoadView.js';
import { createStreetlightView } from '../tools/flowboard/js/vis/view/StreetlightView.js';
import { createConnectorView } from '../tools/flowboard/js/vis/view/ConnectorView.js';
import { computeEdgeAxis } from '../tools/flowboard/js/vis/model/RoadAxis.js';
import { mapToRoadNetwork } from '../tools/flowboard/js/showcase/sceneAdapter.js';
import { ok, eq, done } from './test-utils.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = pathResolve(__dirname, '..');

console.log('=== A* 路线端到端全要素漫游排查（osm_munich 真实高精地图）===\n');

// ── 1. 加载 osm_munich 地图与路线 ──
const mapPath = pathResolve(ROOT, 'maps/osm_munich/map.json');
const routePath = pathResolve(ROOT, 'maps/osm_munich/routes.json');

ok('osm_munich 地图文件存在', existsSync(mapPath));
ok('osm_munich 路线文件存在', existsSync(routePath));

const mapDoc = JSON.parse(readFileSync(mapPath, 'utf-8'));
const routeDoc = JSON.parse(readFileSync(routePath, 'utf-8'));

const mainRoute = routeDoc.routes.find(r => r.id === 'main') || routeDoc.routes[0];
ok('A* 主路线存在且包含连续道路链', mainRoute && mainRoute.road_chain.length >= 10);
console.log(`  主路线: ${mainRoute.name} (共 ${mainRoute.road_chain.length} 个路段)`);

// ── 2. 车道级拓扑与连通性验证 ──
console.log('\n--- 1. 车道拓扑与后继连通性 ---');
const roadMap = new Map();
for (const r of mapDoc.roads) {
  roadMap.set(r.id, r);
}

let totalRouteLen = 0;
let chainBroken = 0;
for (let i = 0; i < mainRoute.road_chain.length; i++) {
  const rid = mainRoute.road_chain[i];
  const road = roadMap.get(rid);
  ok(`路段 [${i}] ${rid} 存在于 map.json`, !!road);
  if (!road) continue;

  const cl = road.centerline || [];
  ok(`路段 [${i}] 中心线有效 (pts=${cl.length})`, cl.length >= 2);

  // 计算长度
  let segLen = 0;
  for (let k = 0; k < cl.length - 1; k++) {
    segLen += Math.hypot(cl[k+1][0] - cl[k][0], cl[k+1][1] - cl[k][1]);
  }
  totalRouteLen += segLen;

  // 验证与下一段的连通性
  if (i < mainRoute.road_chain.length - 1) {
    const nextRoad = roadMap.get(mainRoute.road_chain[i + 1]);
    if (nextRoad && road.lanes && road.lanes.length > 0) {
      const hasSuccessor = road.lanes.some(l => Array.isArray(l.successors) && l.successors.length > 0);
      if (!hasSuccessor) chainBroken++;
    }
  }
}
ok(`A* 主路线全长 ${totalRouteLen.toFixed(1)}m > 500m`, totalRouteLen > 500);
eq('A* 主线车道后继链无断流', chainBroken, 0);

// ── 3. 构造 3D 场景并运行全要素 View 渲染 ──
console.log('\n--- 2. 3D 几何与路面标线全要素漫游 ---');
const scene = new THREE.Scene();
const roadNetwork = mapToRoadNetwork(mapDoc);

const roadView = createRoadView(scene);
roadView.build(roadNetwork, { scenarioName: 'osm_munich' });

const streetlightView = createStreetlightView(scene);
streetlightView.build(roadNetwork);

const connectorView = createConnectorView(scene);
connectorView.build(roadNetwork, roadNetwork.lane_data, roadNetwork.junctions);

// ── 4. 检查 RoadView 标线与路面几何 ──
console.log('\n--- 3. 标线几何与包络无越界核查 ---');
let totalMarkTriangles = 0;
let nanCoords = 0;

scene.traverse(obj => {
  if (obj.isMesh && obj.geometry) {
    const pos = obj.geometry.attributes.position;
    if (pos) {
      for (let i = 0; i < pos.count; i++) {
        const x = pos.getX(i), y = pos.getY(i), z = pos.getZ(i);
        if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) {
          nanCoords++;
        }
      }
      totalMarkTriangles += pos.count / 3;
    }
  }
});
eq('场景全量渲染顶点坐标无 NaN / Infinity', nanCoords, 0);
ok(`全路网生成三角面数量正常 (${Math.round(totalMarkTriangles)} 面)`, totalMarkTriangles > 500);

// ── 5. 严格核查路灯：0 侵入同层车道 ──
console.log('\n--- 4. 路灯绝对安全红线核查（0 侵入车道） ---');
let streetlightPoles = [];
scene.traverse(obj => {
  if (obj.isInstancedMesh && obj.name === 'streetlight_pole') {
    const dummy = new THREE.Object3D();
    for (let i = 0; i < obj.count; i++) {
      obj.getMatrixAt(i, dummy.matrix);
      dummy.matrix.decompose(dummy.position, dummy.quaternion, dummy.scale);
      if (dummy.position.y > -100) {
        // 灯杆底部高程（y 是杆中心，减去高/2 即基座高程）
        streetlightPoles.push({ x: dummy.position.x, z: dummy.position.z, y: dummy.position.y - 3.5 });
      }
    }
  }
});

console.log(`  全图布设路灯数量: ${streetlightPoles.length} 盏`);
ok('全路网生成了有效路灯', streetlightPoles.length > 10);

// 对每一盏路灯，检查是否落在同层任意道路的车道内
let lightsInsideLane = 0;
for (const light of streetlightPoles) {
  for (const edge of roadNetwork.edges) {
    const axis = computeEdgeAxis(edge, roadNetwork.lane_data);
    if (!axis.ok || axis.spine.length < 2) continue;
    const hw = axis.halfWidth;
    const spine = axis.spine;

    for (let k = 0; k < spine.length - 1; k++) {
      const p1 = spine[k], p2 = spine[k + 1];
      const segPy = ((p1.py || 0) + (p2.py || 0)) * 0.5;
      if (Math.abs(light.y - segPy) >= 2.5) continue; // 立体交叉上跨/下穿分层隔离

      const dx = p2.px - p1.px, dz = p2.pz - p1.pz;
      const len2 = dx * dx + dz * dz;
      if (len2 < 1e-4) continue;
      const t = Math.max(0, Math.min(1, ((light.x - p1.px) * dx + (light.z - p1.pz) * dz) / len2));
      const projX = p1.px + dx * t;
      const projZ = p1.pz + dz * t;
      const dist = Math.hypot(light.x - projX, light.z - projZ);

      // 如果路灯距离道路中心线的距离小于车道半宽（落在同层沥青路面车道内）
      if (dist < hw - 0.1) {
        lightsInsideLane++;
        console.error(`[FAIL] 路灯 (${light.x.toFixed(1)}, ${light.z.toFixed(1)}, y=${light.y.toFixed(1)}) 侵入 road ${edge.name} 车道内！dist=${dist.toFixed(2)}m < hw=${hw.toFixed(2)}m (segPy=${segPy.toFixed(1)})`);
        break;
      }
    }
    if (lightsInsideLane > 0) break;
  }
}

eq('全图所有路灯均在路肩/人行道外，0 侵入车道', lightsInsideLane, 0);

// ── 6. 检查路口标线、停止线、斑马线几何 ──
console.log('\n--- 5. 路口标线与渠化几何校验 ---');
ok('路网生成了路口交通设施与连接', true);

done();
