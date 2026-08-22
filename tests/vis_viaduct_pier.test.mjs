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

let columns = [];
let caps = [];
scene.traverse((ch) => {
  if (!ch.geometry || !ch.material || !ch.material.color) return;
  if (ch.material.color.getHex() !== SCENE.pier) return;
  if (ch.isInstancedMesh) {
    const dummy = new THREE.Object3D();
    for (let i = 0; i < ch.count; i++) {
      ch.getMatrixAt(i, dummy.matrix);
      dummy.matrix.decompose(dummy.position, dummy.quaternion, dummy.scale);
      const item = {
        position: { x: dummy.position.x, y: dummy.position.y, z: dummy.position.z },
        scale: { x: dummy.scale.x, y: dummy.scale.y, z: dummy.scale.z },
      };
      if (ch.geometry.type === 'CylinderGeometry') columns.push(item);
      else if (ch.geometry.type === 'BoxGeometry') caps.push(item);
    }
  }
});

ok('高架生成桥墩立柱（>0）', columns.length > 0);
ok('桥墩立柱数量符合分布（4 根立柱 @100m）', columns.length === 4);
ok('桥墩盖梁数量符合分布（4 个盖梁 @100m）', caps.length === 4);

let badH = 0;
for (const p of columns) {
  // z=6 → 路面下沿高程 h≈5.7m, position.y = h/2, scale.y = h
  if (Math.abs(p.position.y - p.scale.y / 2) > 1e-4 || p.scale.y < 5.0) badH++;
}
ok('桥墩立柱高度自洽（position.y = scale.y/2, scale.y ≈ 5.7m）', badH === 0);

// 桥墩位置应在桥的 ENU x∈[10,90] 范围内（worldToThree x 不变）
const xs = columns.map((p) => p.position.x).sort((a, b) => a - b);
ok('桥墩沿桥分布（首墩 x≈12, 末墩 x≈78）',
  Math.abs(xs[0] - 12) < 1 && Math.abs(xs[xs.length - 1] - 78) < 1);

ok('地面路 / 隧道不生成桥墩（立柱仅 4 根）', columns.length === 4);

done();
