/**
 * junction_fork_alignment.test.mjs — fork→路口 1:1 归属回归测试
 *
 * 锁死 2026-08-19 修复：旧 prefix+altId 无脑匹配让同一条 road 的两端路口
 * 各自被 fork 错画一次——40–100m 外的鬼停止线。判别信号：A 域停止线数。
 *
 * 合成场景：横向 main_mid 把路口 A（西，x=0）和 B（东，x=80）串成两个 T
 * 字路口。fork.incoming_road = "main_mid"（终点在 B）→ main_mid_r U-turn。
 *
 * 修前：A 的 main_mid 命中（exact，旧 fromIndices 无 fromEnd 校验）→ A 鬼
 *   conn，incomingIdx_A = {main_mid} → A 在 main_mid 锚点画 1 条鬼 stop。
 * 修后：A 的 main_mid fromEnd=false 被 refined 拒 → A 无 conn → fallback
 *   全画 → 6 条 stop（每个 arm 一个）。B 域两边都是 1（refined 不影响 B）。
 *
 * 跑法：
 *   node --import ./tests/support/three-real-preload.mjs tests/junction_fork_alignment.test.mjs
 */

import { createConnectorView }
  from '../tools/flowboard/js/vis/view/ConnectorView.js';
import { getTopology }
  from '../tools/flowboard/js/vis/model/TopologyModel.js';
import { ok, done } from './test-utils.mjs';

console.log('=== fork→路口 1:1 归属（修 fork 鬼连接）===\n');

// 几何：横向 main 三段（A 西 stub / A-B 干道 / B 东 stub），南北向 side 把
// A、B 各自补成 T（≥3 臂端点聚簇）。main_mid 在 A 端 fromEnd=false（起点），
// B 端 fromEnd=true（终点）——这是 fork 鬼连接的关键：同一 road 段同时是
// 两端 arm，fromEnd 一真一假。
const edges = [
  { id: 'main_w',   name: 'main_w',   type: 'secondary', lanes: 2, lane_width: 3.5,
    nodes: [[-30, 0, 0], [0, 0, 0]],   oneway: false },
  { id: 'main_w_r', name: 'main_w_r', type: 'secondary', lanes: 2, lane_width: 3.5,
    nodes: [[0, 0, 0], [-30, 0, 0]],   oneway: false },
  { id: 'main_mid',   name: 'main_mid',   type: 'secondary', lanes: 2, lane_width: 3.5,
    nodes: [[0, 0, 0], [80, 0, 0]],     oneway: false },
  { id: 'main_mid_r', name: 'main_mid_r', type: 'secondary', lanes: 2, lane_width: 3.5,
    nodes: [[80, 0, 0], [0, 0, 0]],     oneway: false },
  { id: 'main_e',   name: 'main_e',   type: 'secondary', lanes: 2, lane_width: 3.5,
    nodes: [[80, 0, 0], [110, 0, 0]],  oneway: false },
  { id: 'main_e_r', name: 'main_e_r', type: 'secondary', lanes: 2, lane_width: 3.5,
    nodes: [[110, 0, 0], [80, 0, 0]],  oneway: false },
  { id: 'side_a', name: 'side_a', type: 'secondary', lanes: 2, lane_width: 3.5,
    nodes: [[0, 0, 0], [0, 30, 0]],     oneway: false },
  { id: 'side_b', name: 'side_b', type: 'secondary', lanes: 2, lane_width: 3.5,
    nodes: [[80, 0, 0], [80, 30, 0]],   oneway: false },
];

// U-turn fork：西向来车 main_mid（终点在 B）→ 东向 main_mid_r 回到 B 起点。
// target main_mid_r 也同时在 A、B，是触发鬼连接的第二个必要条件（target
// 也得在错误路口存在，否则 toIdxs 空，无 conn）。
const mapJunctions = [
  { id: 0, type: 'fork', incoming_road: 'main_mid',
    connecting_roads: [{ id: 'main_mid_r', turn: 'left' }] },
];

const rn = { edges, map_junctions: mapJunctions, lane_data: {} };
const topo = getTopology(rn);
ok('合成两 T 路口全检出（2 个 center）', topo.centers.length === 2);

const ca = topo.centers.reduce((m, c) => (m && m.x < c.x) ? m : c, null);
const cb = topo.centers.reduce((m, c) => (!m || c.x > m.x) ? c : m, null);
ok('西侧路口 A 在 THREE x≈0', Math.abs(ca.x) < 0.1);
ok('东侧路口 B 在 THREE x≈80', Math.abs(cb.x - 80) < 0.1);

const scene = new THREE.Group();
createConnectorView(scene).build(rn);

const A_BBOX = { x0: -25, x1: 25,  z0: -25, z1: 35 };
const B_BBOX = { x0: 55,  x1: 105, z0: -25, z1: 35 };
const inBox = (x, z, b) => x >= b.x0 && x <= b.x1 && z >= b.z0 && z <= b.z1;

let aStops = 0, bStops = 0;
scene.traverse((ch) => {
  if (!ch.isInstancedMesh || !ch.material || !ch.material.color) return;
  if (ch.material.color.getHex() !== 0xffffff) return;
  const m = new THREE.Matrix4();
  const p = new THREE.Vector3(), q = new THREE.Quaternion(), s = new THREE.Vector3();
  for (let i = 0; i < ch.count; i++) {
    ch.getMatrixAt(i, m);
    m.decompose(p, q, s);
    if (Math.abs(s.z - 0.45) < 0.02) {
      if (inBox(p.x, p.z, A_BBOX)) aStops++;
      else if (inBox(p.x, p.z, B_BBOX)) bStops++;
    }
  }
});
ok(`A 域停止线 = 5（refined+fallback；A 有 5 arm；修前 = 1 鬼 stop on main_mid）`, aStops === 5);
ok(`B 域停止线 = 1（real fork 来车 on main_mid）`, bStops === 1);

done();
