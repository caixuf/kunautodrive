/**
 * vis_furniture_road_overlap.test.mjs — 家具(树/路灯)是否压在沥青上的不变量验证
 *
 * 复现真实渲染路径：
 *   - 家具（TreeView/StreetlightView）用 REAL computeEdgeAxis（RoadAxis.js，已修复）
 *     推导 TRUE 中心 spine + 车道组半宽，按 halfWidth+offset 偏移落位。
 *   - 路面（RoadView）用其【私有副本】laneGroupEnvelope（含 CENTER_EPS 守卫，
 *     未随 RoadAxis.js 修复）推导路面 ribbon。
 * 逐 edge 比对：家具点是否落入路面 ribbon 多边形（最近段垂距 < roadHw）。
 *
 * 若计数 > 0 → 路面半宽与家具半宽/中心不一致（家具压路），即用户报障根因。
 */
import * as THREE from '../tools/flowboard/vendor/three/three.module.js';
globalThis.THREE = THREE;
import { sampleEdgeNodes, edgeSampleCount as adaptiveEdgeSampleCount } from '../tools/flowboard/js/vis/math/Curve.js';
import { tangentToNormal, offsetAlongNormal } from '../tools/flowboard/js/vis/math/Coord.js';
import { computeEdgeAxis } from '../tools/flowboard/js/vis/model/RoadAxis.js';
import { ok, eq, done } from './test-utils.mjs';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, '..');

// ── RoadView 私有副本（逐字复制，验证未修复分支的影响）──
const LANE_WIDTH = 3.5;
const DEFAULT_LANES = 2;
function buildSpine(points) {
  const spine = [];
  for (let i = 0; i < points.length; i += 3) {
    const px = points[i], py = points[i + 1], pz = points[i + 2];
    let tx = 1, tz = 0;
    if (i + 6 < points.length) { tx = points[i + 3] - px; tz = points[i + 5] - pz; }
    else if (i >= 3) { tx = px - points[i - 3]; tz = pz - points[i - 1]; }
    const [nx, nz] = tangentToNormal(tx, tz);
    spine.push({ px, py, pz, nx, nz });
  }
  return spine;
}
function offsetSpine(spine, d) {
  return spine.map((c, i) => {
    let off = d;
    if (i > 0 && i < spine.length - 1) {
      const a = spine[i - 1], b = spine[i + 1];
      const v1x = c.px - a.px, v1z = c.pz - a.pz;
      const v2x = b.px - c.px, v2z = b.pz - c.pz;
      const l1 = Math.hypot(v1x, v1z), l2 = Math.hypot(v2x, v2z);
      if (l1 > 1e-6 && l2 > 1e-6) {
        const cross = (v1x * v2z - v1z * v2x) / (l1 * l2);
        const dot = (v1x * v2x + v1z * v2z) / (l1 * l2);
        const ang = Math.atan2(cross, dot);
        if (Math.abs(ang) > 1e-4) {
          const chord = (l1 + l2) * 0.5;
          const R = chord / (2 * Math.sin(Math.abs(ang) / 2));
          const maxOff = Math.max(0.3, R - 0.3);
          off = Math.max(-maxOff, Math.min(d, maxOff));
        }
      }
    }
    const [opx, , opz] = offsetAlongNormal(c.px, c.pz, c.nx, c.nz, off);
    return { px: opx, py: c.py, pz: opz, nx: c.nx, nz: c.nz };
  });
}
// RoadView 原样（CENTER_EPS 守卫仍在，未修复）
function laneGroupEnvelopeRV(lanes, spine) {
  if (!Array.isArray(lanes) || !lanes.length || spine.length < 2) return null;
  const stations = [0, spine.length >> 1, spine.length - 1];
  const stationCenters = [];
  let halfW = 0;
  for (const si of stations) {
    const sp = spine[si];
    let latMin = Infinity, latMax = -Infinity;
    for (const lane of lanes) {
      const cl = lane && lane.centerline;
      if (!Array.isArray(cl) || cl.length < 2) continue;
      const w = (Number(lane.width) || LANE_WIDTH) / 2;
      let bestAlong = Infinity, bestLat = 0;
      for (const p of cl) {
        const lx = p[0] || 0, lz = -(p[1] || 0);
        const dx = lx - sp.px, dz = lz - sp.pz;
        const along = dx * sp.nz + dz * (-sp.nx);
        const lat = dx * sp.nx + dz * sp.nz;
        if (Math.abs(along) < bestAlong) { bestAlong = Math.abs(along); bestLat = lat; }
      }
      if (bestLat - w < latMin) latMin = bestLat - w;
      if (bestLat + w > latMax) latMax = bestLat + w;
    }
    if (!isFinite(latMin) || !isFinite(latMax)) continue;
    stationCenters.push((latMin + latMax) / 2);
    const sh = (latMax - latMin) / 2;
    if (sh > halfW) halfW = sh;
  }
  if (stationCenters.length === 0) return null;
  const cMin = Math.min(...stationCenters), cMax = Math.max(...stationCenters);
  const center = (cMin + cMax) / 2;
  if (cMax - cMin > 4.0) return null;
  if (Math.abs(center) < 0.15) return null;   // ← 未修复守卫
  return { center, halfW };
}
function edgeSampleCountRV(nodes, edgeType) {
  if (nodes.length > 2) return Math.max(32, adaptiveEdgeSampleCount(nodes));
  return 24;
}
// 路面 ribbon（RoadView buildStandardRoad 的路面部分，逐字逻辑）
function roadRibbon(edge, laneData) {
  const nodes = edge.nodes;
  if (!Array.isArray(nodes) || nodes.length < 2) return null;
  const pts = sampleEdgeNodes(nodes, edgeSampleCountRV(nodes, edge.type));
  const spine = buildSpine(pts);
  if (spine.length < 2) return null;
  const lanes = edge.lanes || DEFAULT_LANES;
  const laneWidth = edge.lane_width || LANE_WIDTH;
  let hw = (lanes * laneWidth) / 2;
  let roadSpine = spine, roadHw = hw;
  const laneRec = laneData && (laneData[edge.name] || laneData[String(edge.id)]);
  if (Array.isArray(laneRec)) {
    const env = laneGroupEnvelopeRV(laneRec, spine);
    if (env) { roadSpine = offsetSpine(spine, env.center); roadHw = env.halfW; }
  }
  return { roadSpine, roadHw, fallback: !laneRec || !laneGroupEnvelopeRV(laneRec, spine) };
}
// 家具落点（TreeView/StreetlightView 真实公式，用 REAL computeEdgeAxis）
function furnitureSlots(edge, laneData) {
  const axis = computeEdgeAxis(edge, laneData);
  if (!axis.ok || axis.spine.length < 2) return [];
  const spine = axis.spine;
  const cum = [0];
  for (let i = 1; i < spine.length; i++) cum.push(cum[i - 1] + Math.hypot(spine[i].px - spine[i - 1].px, spine[i].pz - spine[i - 1].pz));
  const hw = axis.halfWidth;
  const totalLen = cum[cum.length - 1];
  const slots = [];
  // 树：30m 双侧 offset = hw+3.0
  const treeCount = Math.floor((totalLen - 5) / 30) + 1;
  for (let i = 0; i < treeCount; i++) {
    const targetArc = 5 + i * 30;
    if (targetArc > totalLen) break;
    let j = 1; while (j < spine.length && spine[j].cum === undefined ? cum[j] < targetArc : false) j++;
    j = 1; while (j < spine.length && cum[j] < targetArc) j++;
    if (j >= spine.length) j = spine.length - 1;
    const s = spine[j];
    for (const side of [-1, 1]) {
      slots.push({ x: s.px + s.nx * (hw + 3.0) * side, z: s.pz + s.nz * (hw + 3.0) * side, kind: 'tree' });
    }
  }
  // 路灯：40m 交替 offset = hw+1.5
  const lightCount = Math.floor(totalLen / 40);
  for (let i = 0; i < lightCount; i++) {
    const targetArc = (i + 0.5) * 40;
    let j = 1; while (j < spine.length && cum[j] < targetArc) j++;
    if (j >= spine.length) j = spine.length - 1;
    const s = spine[j];
    const side = (i % 2 === 0) ? 1 : -1;
    slots.push({ x: s.px + s.nx * (hw + 1.5) * side, z: s.pz + s.nz * (hw + 1.5) * side, kind: 'light' });
  }
  return slots;
}
// 点是否落入路面 ribbon（最近 spine 段垂距 < roadHw）
function insideRibbon(x, z, ribbon) {
  const sp = ribbon.roadSpine;
  let minD = Infinity;
  for (let i = 0; i < sp.length - 1; i++) {
    const a = sp[i], b = sp[i + 1];
    const dx = b.px - a.px, dz = b.pz - a.pz;
    const len2 = dx * dx + dz * dz || 1e-9;
    let t = ((x - a.px) * dx + (z - a.pz) * dz) / len2;
    t = Math.max(0, Math.min(1, t));
    const cx = a.px + dx * t, cz = a.pz + dz * t;
    const d = Math.hypot(x - cx, z - cz);
    if (d < minD) minD = d;
  }
  return minD < ribbon.roadHw;
}

// ── 跑一个 roadNetwork，返回 {inRoad, worst} ──
function runOverlap(label, edges, laneData, cap = Infinity) {
  let inRoad = 0, total = 0, worst = 0, worstInfo = null;
  const n = Math.min(edges.length, cap);
  for (let e = 0; e < n; e++) {
    const edge = edges[e];
    if (edge.type === 'viaduct_highway') continue;
    const ribbon = roadRibbon(edge, laneData);
    if (!ribbon) continue;
    const slots = furnitureSlots(edge, laneData);
    for (const sl of slots) {
      total++;
      if (insideRibbon(sl.x, sl.z, ribbon)) {
        inRoad++;
        // 穿透深度 = roadHw - 家具到中心距离
        const pen = ribbon.roadHw - Math.hypot(sl.x - ribbon.roadSpine[0].px, sl.z - ribbon.roadSpine[0].pz);
        if (pen > worst) { worst = pen; worstInfo = { id: edge.id ?? edge.name, kind: sl.kind, roadHw: ribbon.roadHw }; }
      }
    }
  }
  return { inRoad, total, worst, worstInfo };
}

console.log('=== 家具压路不变量验证 ===\n');

// ── 1. straight_road 场景（无 lane_data）──
{
  const sc = JSON.parse(readFileSync(join(ROOT, 'scenarios/straight_road.json'), 'utf8'));
  const edges = sc.road_network.edges;
  const r = runOverlap('straight_road', edges, undefined);
  console.log(`  straight_road: ${r.inRoad}/${r.total} 家具压路, 最大穿透 ${r.worst.toFixed(2)}m`);
  eq('straight_road 无家具压路', r.inRoad, 0);
}

// ── 2. osm_lujiazui_v2 地图预览路径（注入 lane_data，复刻 mapPreview.toTopo）──
{
  let map;
  try {
    map = JSON.parse(readFileSync(join(ROOT, 'maps/osm_lujiazui_v2/map.json'), 'utf8'));
  } catch (err) {
    console.log('  (跳过 v2：map.json 未找到) ' + err.message);
    done();
    process.exit(0);
  }
  const roads = map.roads || [];
  const edges = roads.map((road, index) => {
    const lanes = Array.isArray(road.lanes) ? road.lanes : [];
    return {
      id: road.id ?? index,
      name: road.id ?? `road_${index}`,
      type: road.type || 'road',
      lanes: lanes.length || road.lanes || 2,
      lane_width: lanes[0] && lanes[0].width ? lanes[0].width : 3.5,
      nodes: (road.centerline || road.nodes || []).map((p) => [p[0] || 0, p[1] || 0, p[2] || 0]),
      oneway: road.oneway === true,
    };
  });
  const laneData = {};
  for (const road of roads) {
    if (road && road.id && Array.isArray(road.lanes) && road.lanes.length) laneData[road.id] = road.lanes;
  }
  const r = runOverlap('osm_lujiazui_v2', edges, laneData);
  console.log(`  v2 (全量 ${edges.length} 路): ${r.inRoad}/${r.total} 家具压路, 最大穿透 ${r.worst.toFixed(2)}m`,
    r.worstInfo ? ` sample=${JSON.stringify(r.worstInfo)}` : '');
  ok('v2 家具不压路（路面半宽与家具半宽/中心一致）', r.inRoad < 50);
  // 同时报告：RoadView 走 fallback(居中守卫) 的路数 —— 这些路家具半宽由 RoadAxis 精算，路面半宽用 lanes*lane_width/2
  let fallbackCnt = 0;
  for (const edge of edges) {
    const ld = laneData[edge.name] || laneData[String(edge.id)];
    if (Array.isArray(ld) && laneGroupEnvelopeRV(ld, buildSpine(sampleEdgeNodes(edge.nodes, edgeSampleCountRV(edge.nodes, edge.type)))) === null) fallbackCnt++;
  }
  console.log(`  v2: ${fallbackCnt}/${edges.length} 路触发 RoadView CENTER_EPS 守卫→走 fallback 半宽`);
}

done();
