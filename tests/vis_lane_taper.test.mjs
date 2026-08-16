/**
 * vis_lane_taper.test.mjs — LaneTaper 拓扑邻接回归（真实 three）
 *
 * 锁死 2026-08-16 修复：原 LaneTaper 仅当 b.id === a.id+1（id 连续）才画锥形过渡，
 * 真实宽度/车道数过渡多在路口/弯道处、edge id 并不连续 → 大量漏判（接缝处路宽
 * 突变无过渡，路宽跳变）。现改用拓扑邻接（edge 末端与另一 edge 始端落在同一路口
 * 中心）检测，并贴贯穿方向铺设。本测试用**非连续 id** 十字路口锁：
 *   1. 宽度不同的相邻 edge（id 不连续）→ 仍生成锥形过渡（旧实现 0 个）
 *   2. 宽度相同的相邻 edge → 不生成
 *   3. 生成的锥形位于路口中心（贴方向，不假设沿 +X）
 */

import { createConnectorView }
  from '../tools/flowboard/js/vis/view/ConnectorView.js';
import { SCENE } from '../tools/flowboard/js/vis/theme/tokens.js';
import { ok, done } from './test-utils.mjs';

console.log('=== LaneTaper 拓扑邻接 ===\n');

// 十字路口 @ ENU(0,0)，edge id 故意非连续（10/20/30/40）
// A/C 进口（lanes=2）终于路口，B 出口（lanes=4）起于路口，D 出口（lanes=2）起于路口
function mkEdges(outgoingB = 4) {
  return [
    { id: 10, name: 'A', type: 'urban', lanes: 2, lane_width: 3.5, oneway: false, nodes: [[-30, 0, 0], [0, 0, 0]] },
    { id: 20, name: 'B', type: 'urban', lanes: outgoingB, lane_width: 3.5, oneway: false, nodes: [[0, 0, 0], [30, 0, 0]] },
    { id: 30, name: 'C', type: 'urban', lanes: 2, lane_width: 3.5, oneway: false, nodes: [[0, -30, 0], [0, 0, 0]] },
    { id: 40, name: 'D', type: 'urban', lanes: 2, lane_width: 3.5, oneway: false, nodes: [[0, 0, 0], [0, 30, 0]] },
  ];
}

// 统计 ConnectorView 内的"锥形过渡"mesh：沥青色、4 顶点（梯形）的普通 Mesh
function countTapers(scene) {
  let n = 0, centroids = [];
  scene.traverse((ch) => {
    if (ch.isInstancedMesh) return;
    if (!ch.geometry || !ch.material || !ch.material.color) return;
    if (ch.material.color.getHex() !== SCENE.asphalt) return;
    const pos = ch.geometry.getAttribute('position');
    if (!pos || pos.count !== 4) return;   // 仅 4 顶点梯形 = taper
    n++;
    let sx = 0, sz = 0;
    for (let i = 0; i < 4; i++) { sx += pos.getX(i); sz += pos.getZ(i); }
    centroids.push({ x: sx / 4, z: sz / 4 });
  });
  return { n, centroids };
}

// ── 1. 宽度不同（B=4 vs A/C=2）+ 非连续 id → 旧实现 0，现应 ≥1 ──
{
  const scene = new THREE.Group();
  createConnectorView(scene).build({ edges: mkEdges(4) });
  const { n, centroids } = countTapers(scene);
  ok('非连续 id 的宽度过渡仍被检出（旧实现 0）', n >= 1);
  const atJunction = centroids.some((c) => Math.hypot(c.x, c.z) < 3.5);
  ok('锥形落在路口中心', n >= 1 && atJunction);
}

// ── 2. 宽度相同（全部 lanes=2）→ 不生成锥形 ──
{
  const scene = new THREE.Group();
  createConnectorView(scene).build({ edges: mkEdges(2) });
  const { n } = countTapers(scene);
  ok('宽度相同不生成锥形', n === 0);
}

done();
