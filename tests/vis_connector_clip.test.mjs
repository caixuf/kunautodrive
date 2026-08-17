/**
 * vis_connector_clip.test.mjs — 路口内部 connector 不再渲染白线回归
 *
 * 锁死两次"交叉处车道线混乱"→ 白网：
 *   - 2026-08-16：road_j connector 中心线整条平铺 STOP_Y 且不裁剪 → 全图白网
 *   - 2026-08-18：connector 中心线裁剪到路口圆后仍会与真实 lane marking /
 *     斑马线 / 停止线叠加（截图仍乱）。最终收敛：SUMO connector 是导航拓扑，
 *     **不是可见车道边界**，一律不画白线。真实转向导流统一由 fork turn 数据生成。
 *
 * 本测试锁：
 *   1. 无论 connector 中心线在路口内/外/跨界，都不产生白线 mesh（白网消失）
 *   2. 路口铺装（沥青色）与斑马线/停止线（instanced）仍正常生成
 *
 * 用真实 three（three-real-preload）——路口几何断言需要可遍历的 scene 树，
 * three-shim 的 children 是空数组无法验证 mesh 存在。
 */

import { createConnectorView }
  from '../tools/flowboard/js/vis/view/ConnectorView.js';
import { detectJunctions } from '../tools/flowboard/js/vis/view/JunctionDetect.js';
import { SCENE } from '../tools/flowboard/js/vis/theme/tokens.js';
import { ok, done } from './test-utils.mjs';

console.log('=== connector 不再渲染白线 ===\n');

// 十字路口：四条 arm 汇聚于 ENU(0,0) → 真实 THREE 中心 (0,0)，半径≈7.6m
const edges = [
  { id: 'n', name: 'n', type: 'urban', lanes: 2, lane_width: 3.5, nodes: [[0, -30, 0], [0, 0, 0]], oneway: false },
  { id: 's', name: 's', type: 'urban', lanes: 2, lane_width: 3.5, nodes: [[0, 0, 0], [0, 30, 0]], oneway: false },
  { id: 'w', name: 'w', type: 'urban', lanes: 2, lane_width: 3.5, nodes: [[-30, 0, 0], [0, 0, 0]], oneway: false },
  { id: 'e', name: 'e', type: 'urban', lanes: 2, lane_width: 3.5, nodes: [[0, 0, 0], [30, 0, 0]], oneway: false },
];
const { centers } = detectJunctions({ edges });
ok('合成十字检出 1 个路口', centers.length === 1);

// lane_data：三个 connector（路口内 / 路口外 / 跨界）
const lane_data = {
  road_j_inside: [{ centerline: [[0, 0, 0], [0, 3, 0], [0, 6, 0]] }],
  road_j_outside: [{ centerline: [[100, 100, 0], [100, 103, 0]] }],
  road_j_cross: [{ centerline: [[0, 0, 0], [0, 6, 0], [0, 20, 0]] }],
};

const scene = new THREE.Group();
createConnectorView(scene).build({ edges, lane_data, map_junctions: [] });

// 提取所有非 instanced mesh
const meshes = [];
scene.traverse((ch) => {
  if (ch.isInstancedMesh) return;
  if (ch.geometry && ch.material) meshes.push(ch);
});

const whiteLines = meshes.filter((m) =>
  m.material && m.material.color && m.material.color.getHex() === SCENE.guideLine);
ok('connector 中心线不生成白线 mesh', whiteLines.length === 0);

// 路口铺装（沥青色）仍然存在 —— 标线层只是被收敛，不是整块消失
const patchMesh = meshes.find((m) =>
  m.material && m.material.color && m.material.color.getHex() === SCENE.asphalt);
ok('路口沥青铺装仍生成', !!patchMesh);

// 斑马线 / 停止线 instanced 仍然生成（真实语义标线保留）
let crossInstanced = 0;
scene.traverse((ch) => {
  if (ch.isInstancedMesh) crossInstanced++;
});
ok('斑马线/停止线 instanced 仍生成', crossInstanced > 0);

done();
