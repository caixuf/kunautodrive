/**
 * vis_scene_invariants.test.mjs — 3D 场景视觉几何不变量（Phase 0 机器眼睛）
 *
 * 1. P0-6: 静态/停放车辆俯仰角检查 |rotation.z| < 0.05 rad（杜绝跨层“屁股朝天”）
 * 2. P0-6: B1 地库 + open_ramp 追尾相机 Raycaster 视线零遮挡（自动抓“绿色封顶”）
 * 3. P0-7: RoadFacilityView 斑马线 / 停止线 / 导向箭头全量贴地检查 |z - roadHeightAt| <= 0.05m
 *
 * 跑法：
 *   node --import ./tests/support/three-real-preload.mjs tests/vis_scene_invariants.test.mjs
 */

import { readFileSync } from 'node:fs';
import { dirname, resolve as pathResolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createSceneDirector } from '../tools/flowboard/js/vis/director/SceneDirector.js';
import * as ViewRegistry from '../tools/flowboard/js/vis/core/ViewRegistry.js';
import { worldToThree } from '../tools/flowboard/js/vis/math/Coord.js';
import { roadHeightAt } from '../tools/flowboard/js/vis/math/RoadHeight.js';
import { ok, done } from './test-utils.mjs';

const THREE = globalThis.THREE;
const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = pathResolve(__dirname, '..');

console.log('=== 3D 场景几何不变量（Raycaster + 俯仰 + 标线贴地）===\n');

const scenario = JSON.parse(readFileSync(pathResolve(ROOT, 'scenarios/park_to_park_city.json'), 'utf8'));
const entities = [
  ...(scenario.actors || []).map((a, idx) => ({
    id: idx + 1,
    type: a.type || 'car',
    x: a.x,
    y: a.y,
    z: a.z ?? 0,
    heading: a.heading ?? 0,
    speed: a.speed ?? 0,
    parked_fixed: !!a.parked_fixed,
    is_parked: !!a.is_parked || ((a.speed ?? 0) === 0),
  })),
  ...(scenario.traffic_lights || []).map((tl, idx) => ({
    id: 900 + idx,
    type: 'tl',
    x: tl.x,
    y: tl.y_lane ?? tl.y ?? 0,
    stop_x: tl.x,
    stop_y: tl.y_lane ?? tl.y ?? 0,
    z: tl.z ?? 0,
    heading: 0,
  })),
];

ViewRegistry.clear();
const scene = new THREE.Scene();
const director = createSceneDirector(scene);
director.update({
  metrics: {
    scene: {
      road_network: scenario.road_network,
      ego: { x: 180, y: -10.5, z: -2.625, heading: 0, speed: 4.0, steering: 0 },
      entities,
      parking_slots: scenario.parking_slots || [],
      park_out: scenario.park_out || null,
      etc_gates: scenario.etc_gates || [],
    },
    fusion_localization: { loc_mode: 'SLAM_INDOOR' },
  },
});
director.tickAnimation(16);
scene.updateMatrixWorld(true);

const store = director.getStore();

// 1. 静态/停放车辆俯仰门禁 (|rotation.z| < 0.05 rad)
const vehicleView = ViewRegistry.get('vehicle');
const pitchViolations = [];
const vehicleMeshes = new Set();
if (vehicleView && typeof vehicleView.getVehicleMap === 'function') {
  for (const [id, entry] of vehicleView.getVehicleMap().entries()) {
    if (!entry?.group) continue;
    entry.group.traverse((obj) => {
      if (obj.isMesh) vehicleMeshes.add(obj);
    });
    if (id === 'ego') continue;
    const grpZ = entry.group.rotation?.z || 0;
    let sprungZ = 0;
    entry.group.traverse((child) => {
      if (child.userData?.isSprung || child.name === 'sprungGroup') {
        sprungZ = child.rotation?.z || 0;
      }
    });
    if (Math.abs(grpZ) > 0.05 || Math.abs(sprungZ) > 0.05) {
      pitchViolations.push({ id, grpZ, sprungZ });
    }
  }
}
ok('P0-6: 所有停放/NPC 车辆俯仰角 |rotation.z| < 0.05 rad', pitchViolations.length === 0);
if (pitchViolations.length) console.log('  pitchViolations:', pitchViolations);

// 2. B1 + open_ramp 追尾相机视线 Raycaster 门禁（匹配 CameraRig.js 尾随参数）
const candidateMeshes = [];
scene.traverse((obj) => {
  if (!obj.isMesh || !obj.visible || vehicleMeshes.has(obj)) return;
  if (obj.material && obj.material.transparent && obj.material.opacity < 0.25) return;
  candidateMeshes.push(obj);
});

const raycaster = new THREE.Raycaster();
const occlusions = [];
const ramp = scenario.road_network.edges.find((e) => e.type === 'open_ramp');
const hall = scenario.road_network.edges.find((e) => e.type === 'garage_hall');
const samplePoints = [];
if (hall && hall.nodes?.length >= 2) {
  const [hx0, hy0, hz0] = hall.nodes[0];
  const [hx1] = hall.nodes[hall.nodes.length - 1];
  for (let x = Math.max(hx0 + 25, 25); x <= hx1 - 5; x += 10) {
    samplePoints.push({ x, y: hy0, z: hz0, seg: 'b1' });
  }
}
if (ramp && ramp.nodes?.length >= 2) {
  const [rx0, ry0, rz0] = ramp.nodes[0];
  const [rx1, ry1, rz1] = ramp.nodes[ramp.nodes.length - 1];
  for (let t = 0; t <= 1.15; t += 0.05) {
    samplePoints.push({
      x: rx0 + (rx1 - rx0) * t,
      y: ry0 + (ry1 - ry0) * t,
      z: Math.min(0, rz0 + (rz1 - rz0) * t),
      seg: 'ramp',
    });
  }
}

for (const pt of samplePoints) {
  const subBlend = Math.max(0, Math.min(1, (-3.2 - pt.z) / 1.0));
  const behind = 9 - 1.6 * subBlend;
  const height = 3.0 - 0.95 * subBlend;
  const [ex, ey, ez] = worldToThree(pt.x, pt.y, pt.z);
  const camPos = new THREE.Vector3(ex - behind, ey + height, ez);
  const egoPos = new THREE.Vector3(ex, ey + 0.85, ez);
  const dir = new THREE.Vector3().subVectors(egoPos, camPos);
  const dist = dir.length();
  dir.normalize();
  raycaster.set(camPos, dir);
  raycaster.far = dist - 0.25;
  const hits = raycaster.intersectObjects(candidateMeshes, false);
  if (hits.length > 0) {
    occlusions.push({
      seg: pt.seg,
      x: +pt.x.toFixed(1),
      z: +pt.z.toFixed(2),
      hitY: +hits[0].point.y.toFixed(2),
      dist: +hits[0].distance.toFixed(2),
      color: hits[0].object.material?.color?.getHexString?.() || 'unknown',
    });
  }
}
ok('P0-6: B1+open_ramp 追尾相机视线 0 遮挡', occlusions.length === 0);
if (occlusions.length) console.log('  occlusions:', occlusions.slice(0, 8));

// 2b. open_ramp 天顶净空 Raycaster 门禁（敞口斜坡路面上方不应有任何路面/绿地/顶板封顶，跳过洞口镂空防眩格栅）
const skyBlocked = [];
if (ramp && ramp.nodes?.length >= 2) {
  const [rx0, ry0, rz0] = ramp.nodes[0];
  const [rx1, ry1, rz1] = ramp.nodes[ramp.nodes.length - 1];
  for (let t = 0.03; t <= 0.995; t += 0.02) {
    const rx = rx0 + (rx1 - rx0) * t;
    const ry = ry0 + (ry1 - ry0) * t;
    const rz = rz0 + (rz1 - rz0) * t;
    for (const lat of [-2.0, 0, 2.0]) {
      const [wx, wy, wz] = worldToThree(rx, ry + lat, rz + 0.16);
      raycaster.set(new THREE.Vector3(wx, wy, wz), new THREE.Vector3(0, 1, 0));
      raycaster.far = 8.0;
      const hits = raycaster.intersectObjects(candidateMeshes, false)
        .filter((h) => h.object.name !== 'rampLouvers');
      if (hits.length > 0) {
        skyBlocked.push({
          t: +t.toFixed(2),
          x: +rx.toFixed(1),
          lat,
          roadZ: +rz.toFixed(2),
          hitY: +hits[0].point.y.toFixed(3),
          color: hits[0].object.material?.color?.getHexString?.() || 'unknown',
        });
        break;
      }
    }
  }
}
ok('P0-6: open_ramp 敞口斜坡上方净空无任何封顶网格', skyBlocked.length === 0);
if (skyBlocked.length) console.log('  skyBlocked:', skyBlocked.slice(0, 10));

// 3. RoadFacilityView 斑马线 / 停止线 / 箭头贴地误差 <= 0.05m
const PAINT_Y = 0.162;
const facilityView = ViewRegistry.get('roadFacility');
const facilityStats = facilityView?.getStats?.() || {};
ok('P0-7: 路口/道路标线已构建 (arrows/stopLines/crosswalks > 0)',
  (facilityStats.arrows || 0) > 0 && (facilityStats.stopLines || 0) > 0 && (facilityStats.crosswalks || 0) > 0);

const markViolations = [];
const facilityGroup = facilityView?.getGroup?.();
if (facilityGroup) {
  const m = new THREE.Matrix4();
  const pos = new THREE.Vector3();
  const staticGroup = facilityGroup.getObjectByName('staticFacilities');
  staticGroup?.traverse((obj) => {
    if (!obj.isInstancedMesh || !obj.geometry || obj.geometry.type !== 'BoxGeometry') return;
    for (let i = 0; i < obj.count; i++) {
      obj.getMatrixAt(i, m);
      pos.setFromMatrixPosition(m);
      const wx = pos.x;
      const wy = -pos.z;
      const baseZ = pos.y - PAINT_Y;
      const refZ = roadHeightAt(store, wx, wy, baseZ);
      const err = Math.abs(baseZ - refZ);
      if (!Number.isFinite(err) || err > 0.05) {
        markViolations.push({ i, wx: +wx.toFixed(1), wy: +wy.toFixed(1), baseZ: +baseZ.toFixed(3), refZ: +refZ.toFixed(3), err: +err.toFixed(3) });
      }
    }
  });
}
ok('P0-7: 全部道路设施标线贴地误差 <= 0.05m', markViolations.length === 0);
if (markViolations.length) console.log('  markViolations:', markViolations.slice(0, 8));

done();
