/**
 * vis_connector_clip.test.mjs — connector 转向车道线裁剪到路口回归（真实 three）
 *
 * 锁死 2026-08-16 修复（用户报障"交叉处车道线混乱"→白网）：ConnectorView 把
 * lane_data 中 road_j connector 的 lane centerline 画成白色车道线。原实现整条
 * 平铺 STOP_Y、不裁剪 → 因 junctions[] 缺坐标导致 792 个路口中心塌缩到原点，
 * 7054 段 connector 线横竖交织成全图"白网"。
 *
 * 修复后（配合 JunctionDetect 几何聚类还原真实路口中心）：每段只在"落入某路口
 * 圆内"才画、且跟随 centerline.z 高程。本测试锁：
 *   1. 完全落在路口内的 connector → 全部画出
 *   2. 完全在路口外的 connector → 一条都不画（白网消除）
 *   3. 跨边界的 connector → 仅路口内那段画出（裁剪生效）
 *   4. 画出的所有顶点都在某路口圆内（无溢出）
 */

import { createConnectorView }
  from '../tools/flowboard/js/vis/view/ConnectorView.js';
import { detectJunctions } from '../tools/flowboard/js/vis/view/JunctionDetect.js';
import { SCENE } from '../tools/flowboard/js/vis/theme/tokens.js';
import { ok, done } from './test-utils.mjs';

console.log('=== connector 转向车道线裁剪回归 ===\n');

// 十字路口：四条 arm 汇聚于 ENU(0,0) → 真实 THREE 中心 (0,0)，半径≈7.6m
const edges = [
  { id: 'n', name: 'n', type: 'urban', lanes: 2, lane_width: 3.5, nodes: [[0, -30, 0], [0, 0, 0]], oneway: false },
  { id: 's', name: 's', type: 'urban', lanes: 2, lane_width: 3.5, nodes: [[0, 0, 0], [0, 30, 0]], oneway: false },
  { id: 'w', name: 'w', type: 'urban', lanes: 2, lane_width: 3.5, nodes: [[-30, 0, 0], [0, 0, 0]], oneway: false },
  { id: 'e', name: 'e', type: 'urban', lanes: 2, lane_width: 3.5, nodes: [[0, 0, 0], [30, 0, 0]], oneway: false },
];
const { centers } = detectJunctions({ edges });
ok('合成十字检出 1 个路口', centers.length === 1);
const JC = centers[0];

// lane_data：三个 connector
const lane_data = {
  // 完全在路口内（ENU (0,0)→(0,6)，THREE 距离≤6 < 半径）→ 2 段都画
  road_j_inside: [{ centerline: [[0, 0, 0], [0, 3, 0], [0, 6, 0]] }],
  // 完全在路口外（ENU (100,100) 附近，远离任何路口）→ 不画
  road_j_outside: [{ centerline: [[100, 100, 0], [100, 103, 0]] }],
  // 跨边界：ENU (0,0)→(0,20)，前段在圆内、后段在圆外 → 仅圆内那段画
  road_j_cross: [{ centerline: [[0, 0, 0], [0, 6, 0], [0, 20, 0]] }],
};

const scene = new THREE.Group();
createConnectorView(scene).build({ edges, lane_data, map_junctions: [] });

// 提取 connector 白线 mesh（普通 Mesh、MERGE_LINE_COLOR）
let connMesh = null;
scene.traverse((ch) => {
  if (ch.isInstancedMesh) return;
  if (!ch.geometry || !ch.material || !ch.material.color) return;
  if (ch.material.color.getHex() === SCENE.guideLine) connMesh = ch;
});
ok('connector 白线 mesh 已生成', connMesh !== null);

function vertsOf(mesh) {
  const pos = mesh.geometry.getAttribute('position');
  const out = [];
  for (let i = 0; i < pos.count; i++) out.push({ x: pos.getX(i), z: pos.getZ(i) });
  return out;
}

if (connMesh) {
  const verts = vertsOf(connMesh);
  // road_j_inside: 2 段 ×4 顶点 = 8；road_j_cross: 1 段（圆内） ×4 = 4；
  // road_j_outside: 0。合计 12 顶点。
  ok('仅路口内的段被画出（12 顶点 = inside2段+cross1段）', verts.length === 12);
  // 所有顶点都在某路口圆内（无溢出白网）
  const allInside = verts.every((v) =>
    centers.some((c) => Math.hypot(v.x - c.x, v.z - c.z) <= (c.radius || 0) + 1e-6));
  ok('画出的顶点全部落在路口圆内（裁剪生效）', allInside);
}
// 路口外 connector 不得贡献顶点
ok('路口外 connector 无顶点（白网消除）', connMesh ? vertsOf(connMesh).length === 12 : false);

done();
