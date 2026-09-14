/** vis_marking_types.test.mjs — 车道标线 type→mesh golden（真实 three）
 *
 * 前端只认 map.json 的 type，禁止发明第五套 view。每种已知类型必须落到
 * 约定颜色/高度/横向位置；未知 type 顶点 = 0。
 *
 * 跑法：
 *   node --import ./tests/support/three-real-preload.mjs tests/vis_marking_types.test.mjs
 */

import { createRoadView } from '../tools/flowboard/js/vis/view/RoadView.js';
import { SCENE } from '../tools/flowboard/js/vis/theme/tokens.js';
import { ok, eq, done } from './test-utils.mjs';

console.log('=== 车道标线 type→mesh golden ===\n');

const WHITE = SCENE.lineWhite;
const YELLOW = SCENE.lineYellow;
const Y_MARK = 0.13;
const Y_EDGE = 0.14;

function buildLane(type, side, cz = -1.75) {
  const scene = new THREE.Group();
  const view = createRoadView(scene);
  view.build({
    edges: [{
      id: 'mk', name: 'mk', type: 'highway', lanes: 1, lane_width: 3.5,
      nodes: [[0, 0, 0], [80, 0, 0]], oneway: true,
    }],
    lane_data: {
      mk: [{
        id: 'mk.lane.1', index: 1, direction: 1, width: 3.5,
        centerline: [[0, cz, 0], [80, cz, 0]],
        markings: [{ type, side }],
      }],
    },
  });
  return { scene, view };
}

function collect(scene) {
  const white = [], yellow = [];
  scene.traverse((ch) => {
    if (!ch.isMesh || ch.isInstancedMesh || !ch.material || !ch.material.color) return;
    const hex = ch.material.color.getHex();
    const pos = ch.geometry.getAttribute('position');
    if (!pos) return;
    for (let i = 0; i < pos.count; i++) {
      const v = { x: pos.getX(i), y: pos.getY(i), z: pos.getZ(i) };
      if (hex === WHITE) white.push(v);
      else if (hex === YELLOW) yellow.push(v);
    }
  });
  return { white, yellow };
}

function medianZ(arr) {
  const zs = arr.map((v) => v.z).sort((a, b) => a - b);
  return zs[Math.floor(zs.length / 2)];
}

function nearY(arr, y0) {
  return arr.filter((v) => Math.abs(v.y - y0) < 0.015).length;
}

// 车道中心 ENU y=-1.75 → THREE z=+1.75；hw=1.75
// side=right → 边界 z=3.5；side=left → 边界 z=0
{
  const { scene, view } = buildLane('solid_white', 'right');
  const { white, yellow } = collect(scene);
  eq('solid_white 走 P2', view.getStats().p2Edges, 1);
  ok('solid_white 白色边线（Y_EDGE）', nearY(white, Y_EDGE) > 10);
  eq('solid_white 无黄线', yellow.length, 0);
  ok(`solid_white 落在 |z|≈3.5（med=${medianZ(white).toFixed(2)}）`,
    Math.abs(Math.abs(medianZ(white)) - 3.5) < 0.25);
}

{
  const { scene } = buildLane('dashed_white', 'right');
  const { white, yellow } = collect(scene);
  ok('dashed_white 白色虚线（Y_MARK）', nearY(white, Y_MARK) > 10);
  eq('dashed_white 无黄线', yellow.length, 0);
  ok(`dashed_white 落在 |z|≈3.5（med=${medianZ(white).toFixed(2)}）`,
    Math.abs(Math.abs(medianZ(white)) - 3.5) < 0.25);
}

{
  const { scene } = buildLane('double_yellow', 'left');
  const { white, yellow } = collect(scene);
  eq('double_yellow 无白线', white.length, 0);
  ok('double_yellow 黄线顶点', yellow.length > 20);
  ok(`double_yellow 贴中心 |z|<0.4（med=${medianZ(yellow).toFixed(2)}）`,
    Math.abs(medianZ(yellow)) < 0.4);
}

{
  const { scene } = buildLane('single_yellow', 'left');
  const { white, yellow } = collect(scene);
  eq('single_yellow 无白线', white.length, 0);
  ok('single_yellow 黄线（Y_MARK）', nearY(yellow, Y_MARK) > 10);
  ok(`single_yellow 贴中心 |z|<0.3（med=${medianZ(yellow).toFixed(2)}）`,
    Math.abs(medianZ(yellow)) < 0.3);
}

{
  const { scene } = buildLane('dashed_yellow', 'right');
  const { white, yellow } = collect(scene);
  eq('dashed_yellow 无白线', white.length, 0);
  ok('dashed_yellow 黄虚线（Y_MARK）', nearY(yellow, Y_MARK) > 10);
}

{
  const { scene } = buildLane('left_turn_waiting', 'right');
  const { white, yellow } = collect(scene);
  ok('left_turn_waiting 白虚线', nearY(white, Y_MARK) > 10);
  eq('left_turn_waiting 无黄线', yellow.length, 0);
}

{
  const { scene } = buildLane('deceleration', 'right');
  const { white, yellow } = collect(scene);
  ok('deceleration 白减速块（多于一条边线）', white.length > 40);
  eq('deceleration 无黄线', yellow.length, 0);
  ok(`deceleration 落在右边界附近（med=${medianZ(white).toFixed(2)}）`,
    Math.abs(Math.abs(medianZ(white)) - 3.5) < 0.6);
}

{
  const { scene, view } = buildLane('not_a_real_type', 'right');
  const { white, yellow } = collect(scene);
  eq('未知 type 仍计 P2、不发明启发式', view.getStats().heuristicEdges, 0);
  eq('未知 type 不发明白线', white.length, 0);
  eq('未知 type 不发明黄线', yellow.length, 0);
}

done();
