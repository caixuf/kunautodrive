/** junction_markings.test.mjs — 路口斑马线/停止线几何回归测试（真实 three.js）
 *
 * 锁定 2026-08-14 修复：旧实现用「路口端最后一小段」直射线外推斑马线位置+朝向，
 * 弯道/急弯引道上方向偏差达 72°（OSM 陆家嘴实测），斑马线斜切路面。
 * 修复后 walkFromJunction 径向出圈：位置贴折线、朝向取局部切线。
 *
 * 与 shim 版测试的分工：three-shim 是 Proxy 桩只能"不抛错"冒烟；本测试用
 * 真实 three.module.js 读 InstancedMesh 矩阵做几何断言。
 *
 * 跑法：
 *   node --import ./tests/support/three-real-preload.mjs tests/junction_markings.test.mjs
 */

import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve as pathResolve, dirname } from 'node:path';
import { createConnectorView }
  from '../tools/flowboard/js/vis/view/ConnectorView.js';
import { createBarrierView }
  from '../tools/flowboard/js/vis/view/BarrierView.js';
import { walkFromJunction }
  from '../tools/flowboard/js/vis/model/TopologyModel.js';
import { detectJunctions } from '../tools/flowboard/js/vis/view/JunctionDetect.js';
import { ok, done } from './test-utils.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = pathResolve(__dirname, '..');

console.log('=== 路口标线几何回归（真实 three）===\n');

const deg = (x, y) => Math.atan2(y, x) * 180 / Math.PI;
const angDiff = (a, b) => {
  let d = Math.abs(a - b) % 360;
  if (d > 180) d = 360 - d;
  return d > 90 ? 180 - d : d;   // 无向线方向（0=平行）
};

// ── 1. walkFromJunction 直道：径向出圈位置/方向精确 ──
console.log('--- 1. walkFromJunction 直道 ---');
{
  const pts = [{ x: 0, z: 0 }, { x: -30, z: 0 }];
  const w = walkFromJunction(pts, false, 0, 0, 10);
  ok('直道出圈 x=-10', Math.abs(w.x + 10) < 1e-3 && Math.abs(w.z) < 1e-3);
  ok('直道切向 (-1,0)', Math.abs(w.ux + 1) < 1e-9 && Math.abs(w.uz) < 1e-9);
}

// ── 2. walkFromJunction 弯道：位置贴弧、切向随弯 ──
console.log('--- 2. walkFromJunction 弯道（R=8 弧）---');
{
  // 圆弧 R=8：p(φ) = (8 sinφ, 8(1-cosφ)) → THREE 域 (x=px, z=py)
  const R = 8, pts = [];
  for (let k = 0; k <= 64; k++) {
    const phi = (k / 64) * Math.PI / 2;
    pts.push({ x: R * Math.sin(phi), z: R * (1 - Math.cos(phi)) });
  }
  const w = walkFromJunction(pts, false, 0, 0, 7);
  ok('出圈点径向距离 = 7', Math.abs(Math.hypot(w.x, w.z) - 7) < 0.05);
  // 出圈点应落在弧上：到圆心 (0,8) 距离 = R
  ok('出圈点在弧上', Math.abs(Math.hypot(w.x, w.z - R) - R) < 0.3);
  // 切向 = 弧在该点的方向：与径向 (w.x,w.z-8)... 切线 ⊥ 半径
  const rx = w.x - 0, rz = w.z - R;
  const rl = Math.hypot(rx, rz);
  // 切向与半径点积 ≈ 0
  ok('切向 ⊥ 半径', Math.abs((w.ux * rx + w.uz * rz) / rl) < 0.08);
}

// ── 合成 T 字路口：弯主路 + 两条直支路（三端点共点成路口）──
// 主路 R=8 急弯 90°：旧实现斑马线按端段直线外推，方向差 ~52°、位置偏出路面。
const R = 8;
const arcNodes = [];
for (let k = 0; k <= 64; k++) {
  const phi = (k / 64) * Math.PI / 2;
  arcNodes.push([R * Math.sin(phi), R * (1 - Math.cos(phi)), 0]);
}
const sideA = [[0, 0, 0], [-12, -12, 0], [-30, -40, 0]];
const sideB = [[0, 0, 0], [-18, 4, 0], [-40, 8, 0]];
/* 隧道臂朝 ESE（与弯主路终点 (8,8)N、sideA SW、sideB WNW 都拉开距离，
 * 避免合成图过度拥挤导致别的 arm 的条纹物理上落在隧道走廊上被误判） */
const tunnelArm = [[0, 0, 0], [20, -6, 0], [40, -10, 0]];

function mkEdges() {
  return [
    { id: 'main_curve', name: 'main_curve', type: 'secondary', lanes: 2, lane_width: 3.5, nodes: arcNodes, oneway: false },
    { id: 'side_a', name: 'side_a', type: 'secondary', lanes: 2, lane_width: 3.5, nodes: sideA, oneway: false },
    { id: 'side_b', name: 'side_b', type: 'secondary', lanes: 2, lane_width: 3.5, nodes: sideB, oneway: false },
    { id: 'test_tunnel', name: '试验隧道', type: 'secondary', lanes: 2, lane_width: 3.5, nodes: tunnelArm, oneway: true },
  ];
}

function extractCrosswalkStripes(scene) {
  const out = [];
  scene.traverse((ch) => {
    if (!ch.isInstancedMesh) return;
    if (!ch.material || !ch.material.color) return;
    if (ch.material.color.getHex() !== 0xf5f5f0) return;   // 只取斑马线白
    const m = new THREE.Matrix4();
    for (let i = 0; i < ch.count; i++) {
      ch.getMatrixAt(i, m);
      const p = new THREE.Vector3(), q = new THREE.Quaternion(), s = new THREE.Vector3();
      m.decompose(p, q, s);
      const ax = new THREE.Vector3(1, 0, 0).applyQuaternion(q);
      // ENU 方向角：THREE 轴 (ax.x, ax.z) → ENU (ax.x, -ax.z)，deg(x,y)=atan2(y,x)
      out.push({ x: p.x, z: p.z, dirDeg: deg(ax.x, -ax.z) });
    }
  });
  return out;
}

// ENU 点到折线段投影：返回 {dist, dirDeg}
function nearestPolyline(px, py, nodes) {
  let best = { dist: Infinity, dirDeg: 0 };
  for (let i = 0; i < nodes.length - 1; i++) {
    const a = nodes[i], b = nodes[i + 1];
    const abx = b[0] - a[0], aby = b[1] - a[1];
    const len2 = abx * abx + aby * aby || 1e-12;
    let t = ((px - a[0]) * abx + (py - a[1]) * aby) / len2;
    t = Math.max(0, Math.min(1, t));
    const qx = a[0] + t * abx, qy = a[1] + t * aby;
    const d = Math.hypot(qx - px, qy - py);
    if (d < best.dist) best = { dist: d, dirDeg: deg(abx, aby) };
  }
  return best;
}

console.log('--- 3. 弯道路口斑马线：位置贴路 + 朝向平行 ---');
{
  const edges = mkEdges();
  const scene = new THREE.Group();
  createConnectorView(scene).build({ edges });
  const stripes = extractCrosswalkStripes(scene);
  ok('斑马线已生成（>20 条）', stripes.length > 20);

  let worstAng = 0, worstDist = 0, offRoad = 0, misaligned = 0;
  for (const s of stripes) {
    // ENU 坐标（THREE z → ENU y 取负）
    const ex = s.x, ey = -s.z;
    // 归属判据：存在一条路「距离 ≤4.2m 且方向差 ≤12°」即正确生成。
    // （近距离取最近路的判据在夹角小的 arm 之间会误归属——sideA SW 与
    //  sideB WNW 的条纹落在两路之间的楔形区，物理上离另一条路更近。）
    let bestCombo = { dist: Infinity, ang: 180 };
    let nearestDist = Infinity;
    for (const e of edges) {
      const r = nearestPolyline(ex, ey, e.nodes);
      const a = angDiff(s.dirDeg, r.dirDeg);
      if (r.dist < nearestDist) nearestDist = r.dist;
      if (r.dist <= 4.2 && a < bestCombo.ang) bestCombo = { dist: r.dist, ang: a };
    }
    worstDist = Math.max(worstDist, nearestDist);
    worstAng = Math.max(worstAng, bestCombo.ang);
    if (bestCombo.dist === Infinity) offRoad++;       // 没有任何路在 4.2m 内
    if (bestCombo.ang > 12) misaligned++;
  }
  ok(`条纹全部在路上（离中心线 ≤4.2m，worst=${worstDist.toFixed(2)}）`, offRoad === 0);
  ok(`条纹与归属路向平行（≤12°，worst=${worstAng.toFixed(1)}°）`, misaligned === 0);
}

console.log('--- 4. 隧道/地道 arm 不画斑马线 ---');
{
  const edges = mkEdges();
  const scene = new THREE.Group();
  createConnectorView(scene).build({ edges });
  const stripes = extractCrosswalkStripes(scene);
  // 判据：「与隧道局部切向平行（±10°）且落在走廊 4m 内」的条纹 = 为隧道臂
  // 生成的斑马线，应为 0。（路口条纹物理上邻近他路是常态，不能按距离判）
  let tunnelStripes = 0;
  for (const s of stripes) {
    const ex = s.x, ey = -s.z;
    const r = nearestPolyline(ex, ey, tunnelArm);
    if (r.dist < 4 && angDiff(s.dirDeg, r.dirDeg) < 10) tunnelStripes++;
  }
  ok('隧道臂无斑马线（朝向+走廊双判据）', tunnelStripes === 0);
}

// ── 5. 真实大地图回归：OSM 陆家嘴全图斑马线朝向（曾 974/5548 条纹 >15°）──
console.log('--- 5. OSM 陆家嘴真实图（skip 若缺图）---');
{
  const mapPath = pathResolve(ROOT, 'maps/osm_lujiazui/map.json');
  if (!existsSync(mapPath)) {
    ok('osm_lujiazui 缺图跳过', true);
  } else {
    const map = JSON.parse(readFileSync(mapPath, 'utf8'));
    const edges = (map.roads || []).map((road, i) => ({
      id: road.id || i, name: road.id || `road_${i}`, type: road.type || 'road',
      lanes: (Array.isArray(road.lanes) ? road.lanes.length : road.lanes) || 2,
      lane_width: (Array.isArray(road.lanes) && road.lanes[0] && road.lanes[0].width) || 3.5,
      nodes: (road.centerline || road.nodes || []).map(p => [p[0] || 0, p[1] || 0, p[2] || 0]),
      oneway: road.oneway === true,
    }));
    const scene = new THREE.Group();
    createConnectorView(scene).build({ edges });
    const stripes = extractCrosswalkStripes(scene);
    ok(`大地图斑马线生成（${stripes.length} 条）`, stripes.length > 1000);

    // 生成端用的路口/arm（与 view 同一 detectJunctions + walkFromJunction）
    const { centers, byId } = detectJunctions({ edges });
    const edgeMap = new Map(edges.map(e => [String(e.id), e]));
    const anchors = [];
    centers.forEach((c, ci) => {
      for (const [edgeId, entry] of byId) {
        if (entry.start !== ci && entry.end !== ci) continue;
        const e = edgeMap.get(edgeId);
        if (!e || e.nodes.length < 2) continue;
        if (/隧道|地道|tunnel/i.test(String(e.name || e.id))) continue;
        const pts = e.nodes.map(n => ({ x: n[0], z: -n[1] }));   // ENU→THREE XZ
        anchors.push(walkFromJunction(pts, entry.end === ci, c.x, c.z, c.radius + 2.0));
      }
    });
    let bad = 0, worst = 0;
    for (const s of stripes) {
      let minAng = 180, near = false;
      for (const a of anchors) {
        const d = Math.hypot(a.x - s.x, a.z - s.z);
        if (d > 12) continue;
        near = true;
        const aDeg = deg(a.ux, -a.uz);   // walk 返回 THREE XZ 方向 → ENU 角
        const dd = angDiff(s.dirDeg, aDeg);
        if (dd < minAng) minAng = dd;
      }
      if (!near) continue;   // 远离任何锚点的条纹不参与（理论上不存在）
      if (minAng > worst) worst = minAng;
      if (minAng > 15) bad++;
    }
    ok(`大地图条纹朝向全平行（>15° 的 ${bad}/${stripes.length}，worst=${worst.toFixed(1)}°）`, bad === 0);
  }
}

// ── 6. 护栏路口裁剪：护栏立柱/横梁端点不得落在路口圆内（P0 用户报障修复）──
console.log('--- 6. 护栏路口裁剪（highway 十字）---');
{
  // 两条 highway 在 (0,0) 相接 + 两条支路成 ≥3 臂路口；highway 才触发 BarrierView
  const hwEdges = [
    { id: 'hwy_w', name: 'hwy_w', type: 'highway', lanes: 2, lane_width: 3.5, nodes: [[-60, 0, 0], [0, 0, 0]], oneway: false },
    { id: 'hwy_e', name: 'hwy_e', type: 'highway', lanes: 2, lane_width: 3.5, nodes: [[0, 0, 0], [60, 0, 0]], oneway: false },
    { id: 'side_s', name: 'side_s', type: 'secondary', lanes: 2, lane_width: 3.5, nodes: [[0, -30, 0], [0, 0, 0]], oneway: false },
    { id: 'side_n', name: 'side_n', type: 'secondary', lanes: 2, lane_width: 3.5, nodes: [[0, 0, 0], [0, 30, 0]], oneway: false },
  ];
  const { centers } = detectJunctions({ edges: hwEdges });
  ok('合成十字恰好 1 个路口', centers.length === 1);
  const JC = centers[0] || { x: 0, z: 0, radius: 5 };   // THREE 坐标（≈(0,0)，半径≈5）
  const scene = new THREE.Group();
  createBarrierView(scene).build({ edges: hwEdges });
  let posts = 0, beams = 0, insidePosts = 0, insideBeamEnds = 0;
  scene.traverse((ch) => {
    if (!ch.isInstancedMesh || !ch.material || !ch.material.color) return;
    const hex = ch.material.color.getHex();
    const isPost = hex === 0x8a9095, isBeam = hex === 0x9aa0a4;
    if (!isPost && !isBeam) return;
    const baseLen = (ch.geometry.parameters && ch.geometry.parameters.width) || 1;
    const m = new THREE.Matrix4();
    for (let i = 0; i < ch.count; i++) {
      ch.getMatrixAt(i, m);
      const p = new THREE.Vector3(), q = new THREE.Quaternion(), s = new THREE.Vector3();
      m.decompose(p, q, s);
      if (isPost) {
        posts++;
        if (Math.hypot(p.x - JC.x, p.z - JC.z) < JC.radius - 1e-6) insidePosts++;
      } else {
        beams++;
        const ax = new THREE.Vector3(1, 0, 0).applyQuaternion(q);
        const half = (s.x * baseLen) / 2;   // 梁实际半长 = scale.x × 几何底长 / 2
        for (const sgn of [1, -1]) {
          const ex = p.x + ax.x * half * sgn, ez = p.z + ax.z * half * sgn;
          if (Math.hypot(ex - JC.x, ez - JC.z) < JC.radius - 1e-6) insideBeamEnds++;
        }
      }
    }
  });
  ok(`护栏已生成（posts=${posts} beams=${beams}）`, posts > 20 && beams > 20);
  ok(`立柱不穿路口（${insidePosts}/${posts}）`, insidePosts === 0);
  ok(`横梁端点不穿路口（${insideBeamEnds}）`, insideBeamEnds === 0);
}

// ── 7. 护栏弯道打结防护：急弯内侧偏移收缩时不得产出过短/反向横梁 ──
console.log('--- 7. 护栏弯道打结防护（R=6 发卡，内侧偏移 4m 收缩比 0.33<0.35）---');
{
  // 发卡：直段 + R=6 半圆 + 直段（内侧偏移 R-d=2m，每 2m 弧长弦收缩到 0.67m）
  const nodes = [[-40, 6, 0], [-6, 6, 0]];
  for (let k = 0; k <= 36; k++) {
    const th = Math.PI - (k / 36) * Math.PI;   // 180°→0°
    nodes.push([6 * Math.cos(th), 6 + 6 * Math.sin(th), 0]);
  }
  nodes.push([40, 6, 0]);
  const hairpin = [
    { id: 'hairpin', name: 'hairpin_hwy', type: 'highway', lanes: 2, lane_width: 3.5, nodes, oneway: false },
  ];
  const scene = new THREE.Group();
  createBarrierView(scene).build({ edges: hairpin });
  let beams = 0, minLen = Infinity;
  scene.traverse((ch) => {
    if (!ch.isInstancedMesh || !ch.material || !ch.material.color) return;
    if (ch.material.color.getHex() !== 0x9aa0a4) return;   // 只看横梁
    const baseLen = (ch.geometry.parameters && ch.geometry.parameters.width) || 1;
    const m = new THREE.Matrix4();
    for (let i = 0; i < ch.count; i++) {
      ch.getMatrixAt(i, m);
      const p = new THREE.Vector3(), q = new THREE.Quaternion(), s = new THREE.Vector3();
      m.decompose(p, q, s);
      beams++;
      const actualLen = s.x * baseLen;
      if (actualLen < minLen) minLen = actualLen;
    }
  });
  // 无防护时内侧发卡段会产出 ~0.66m 的打结短梁；有防护全部 ≥ 弦比阈值
  ok(`发卡横梁无打结短梁（min=${minLen.toFixed(2)}m，阈值 0.7m，beams=${beams}）`,
    beams > 0 && minLen >= 0.7);
}

done();
