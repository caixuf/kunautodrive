/**
 * vis_viaduct_pier.test.mjs — 高架桥墩消费 bridge 标记 + centerline.z 回归
 *
 * 锁死 2026-08-16 修复（用户报障"道路没高度/分层"）：ConnectorView._buildViaductPier
 * 原读 edge.elevation_profile（本仓库 map.json 从不产出）→ 永远跳过、桥墩死代码。
 * 现改为消费 bridge 标记 + edge.nodes z（osm2kmap 写 6.0*max(1,layer)），高架
 * 每 20m 落墩。本测试锁：
 *   1. bridge:true 且 z>0.5 → 生成桥墩
 *   2. 桥墩从地面立到路面下沿（position.y = h/2，scale.y = h）
 *   3. 非 bridge（普通路 / 隧道）→ 不生成桥墩
 */

import { createConnectorView }
  from '../tools/flowboard/js/vis/view/ConnectorView.js';
import { SCENE } from '../tools/flowboard/js/vis/theme/tokens.js';
import { ok, done } from './test-utils.mjs';

console.log('=== 高架桥墩消费 bridge + centerline.z ===\n');

// 100m 直桥，z=6（高架）
const bridgeEdge = {
  id: 'viaduct', name: 'viaduct', type: 'primary', lanes: 3, lane_width: 3.5,
  bridge: true, oneway: false,
  nodes: [[0, 0, 6], [50, 0, 6], [100, 0, 6]],
};
// 普通地面路（无 bridge）
const groundEdge = {
  id: 'ground', name: 'ground', type: 'urban', lanes: 2, lane_width: 3.5,
  bridge: false, oneway: false,
  nodes: [[0, 50, 0], [100, 50, 0]],
};
// 隧道（z=-4，桥墩只服务于高架）
const tunnelEdge = {
  id: 'tun', name: 'tun', type: 'secondary', lanes: 2, lane_width: 3.5,
  tunnel: true, bridge: false, oneway: true,
  nodes: [[0, 100, -4], [100, 100, -4]],
};

const scene = new THREE.Group();
createConnectorView(scene).build({ edges: [bridgeEdge, groundEdge, tunnelEdge] });

let piers = [];
scene.traverse((ch) => {
  if (ch.isInstancedMesh) return;
  if (!ch.geometry || !ch.material || !ch.material.color) return;
  if (ch.material.color.getHex() === SCENE.pier) piers.push(ch);
});

ok('高架生成桥墩（>0）', piers.length > 0);
ok('桥墩数量符合每 20m 一根（5 根 @100m）', piers.length === 5);

let badH = 0;
for (const p of piers) {
  // z=6 → position.y 应 = 3，scale.y 应 = 6
  if (Math.abs(p.position.y - 3) > 1e-6) badH++;
  if (Math.abs(p.scale.y - 6) > 1e-6) badH++;
}
ok('桥墩高度=路面高程（position.y=3, scale.y=6）', badH === 0);

// 桥墩位置应在桥的 ENU x∈[10,90] 范围内（worldToThree x 不变）
const xs = piers.map((p) => p.position.x).sort((a, b) => a - b);
ok('桥墩沿桥分布（首墩 x≈10, 末墩 x≈90）',
  Math.abs(xs[0] - 10) < 1 && Math.abs(xs[xs.length - 1] - 90) < 1);

ok('地面路 / 隧道不生成桥墩（仅 5 根）', piers.length === 5);

done();
