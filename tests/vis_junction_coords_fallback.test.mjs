/**
 * vis_junction_coords_fallback.test.mjs — 路口中心坐标兜底回归
 *
 * 锁死 2026-08-16 修复：map.json 的 junctions[] 只带
 * {id, type, incoming_road, connecting_roads}（无 x/y/z/radius）。原
 * JunctionDetect.junctionsFromData 对缺坐标 junction 做 worldToThree(0,0) →
 * 全部 792 个路口中心塌缩到世界原点（radius=8），导致路口铺装/斑马线/停止线/
 * 转向导流全算到原点、路口裁剪失效、connector 白线成"白网"。
 *
 * 修复：detectJunctions 在 junctions 缺可用坐标时回退几何端点聚类（基于 edge
 * 真实几何），中心落到真实交叉口。本测试锁三件事：
 *   1. junctions 缺坐标 → 中心落在真实交叉口（非原点）
 *   2. junctions 带坐标 → 直接消费坐标
 *   3. junctions 不存在 → 几何聚类兜底同样正确
 */

import { detectJunctions } from '../tools/flowboard/js/vis/view/JunctionDetect.js';
import { worldToThree } from '../tools/flowboard/js/vis/math/Coord.js';
import { ok, done } from './test-utils.mjs';

console.log('=== JunctionDetect 坐标兜底 ===\n');

// 十字交叉口：四条 edge 汇聚于 ENU(100, 200)
const CROSS = { x: 100, y: 200 };
function buildEdges() {
  return [
    { id: 0, name: 'A', type: 'urban', lanes: 2, lane_width: 3.5,
      nodes: [[100, 180, 0], [100, 200, 0]] },           // 北进口
    { id: 1, name: 'B', type: 'urban', lanes: 2, lane_width: 3.5,
      nodes: [[100, 200, 0], [100, 220, 0]] },           // 南出口
    { id: 2, name: 'C', type: 'urban', lanes: 2, lane_width: 3.5,
      nodes: [[80, 200, 0], [100, 200, 0]] },            // 西进口
    { id: 3, name: 'D', type: 'urban', lanes: 2, lane_width: 3.5,
      nodes: [[100, 200, 0], [120, 200, 0]] },           // 东出口
  ];
}
const [ex, , ez] = worldToThree(CROSS.x, CROSS.y, 0);   // 期望中心 THREE 坐标

// ── 1. junctions 缺坐标 → 几何聚类兜底，中心非原点 ──
(() => {
  const rn = {
    edges: buildEdges(),
    junctions: [{ id: 0, type: 'fork', incoming_road: 'A',
      connecting_roads: [{ id: 'C', turn: 'straight' }] }],   // 无 x/y/z
  };
  const { centers } = detectJunctions(rn);
  ok('缺坐标 junctions 仍检出路口', centers.length >= 1);
  if (centers.length) {
    const c = centers[0];
    ok('中心 x 落在真实交叉口（非原点）', Math.abs(c.x - ex) < 5);
    ok('中心 z 落在真实交叉口（非原点）', Math.abs(c.z - ez) < 5);
    ok('中心半径 > 0', (c.radius || 0) > 0);
    ok('中心远离原点（起点塌缩缺陷已修）',
      Math.hypot(c.x - worldToThree(0, 0, 0)[0], c.z - worldToThree(0, 0, 0)[2]) > 10);
  }
})();

// ── 2. junctions 带坐标 → 直接消费 ──
(() => {
  const rn = {
    edges: buildEdges(),
    junctions: [{ id: 0, x: CROSS.x, y: CROSS.y, z: 0, radius: 12 }],
  };
  const { centers } = detectJunctions(rn);
  ok('带坐标 junctions 检出路口', centers.length === 1);
  if (centers.length === 1) {
    const c = centers[0];
    ok('中心坐标被直接消费', Math.abs(c.x - ex) < 1e-6 && Math.abs(c.z - ez) < 1e-6);
    ok('半径被直接消费', Math.abs((c.radius || 0) - 12) < 1e-6);
  }
})();

// ── 3. 无 junctions → 几何聚类兜底同样正确 ──
(() => {
  const rn = { edges: buildEdges() };
  const { centers } = detectJunctions(rn);
  ok('无 junctions 仍检出路口（几何兜底）', centers.length >= 1);
  if (centers.length) {
    const c = centers[0];
    ok('无 junctions 中心落真实交叉口', Math.abs(c.x - ex) < 5 && Math.abs(c.z - ez) < 5);
  }
})();

done();
