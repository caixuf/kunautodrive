/**
 * vis_perception_wireframe.test.mjs — 感知 3D 线框门禁（真实 three 几何）
 *
 * 用 vendored three.js（能读到真实 BufferAttribute）验证：
 *  1. 每个动态目标生成 8 顶点 / 12 边完整线框（drawRange = 24 verts）
 *  2. 线框按类别着色（车青 / 人琥珀），不是单一颜色
 *  3. 所有顶点有限、底环低于顶环、框高与目标高度一致
 *  4. 装饰性 facility（building/pier）不被当成感知目标画框
 *
 * 跑法：
 *   node --import ./tests/support/three-real-preload.mjs tests/vis_perception_wireframe.test.mjs
 */

import * as THREE from '../tools/flowboard/vendor/three/three.module.js';
import { createPerceptionView } from '../tools/flowboard/js/vis/view/PerceptionView.js';
import { SCENE } from '../tools/flowboard/js/vis/theme/tokens.js';
import { ok, eq, done } from './test-utils.mjs';

console.log('=== 感知 3D 线框门禁 ===\n');

const store = {
  ego: { x: 0, y: 0, z: 0, heading: 0, length: 4.6 },
  entities: [
    { id: 1, type: 'car', x: 12, y: 0, z: 0, heading: 0, length: 4.6, width: 2.0, height: 1.5, speed: 8 },
    { id: 2, type: 'pedestrian', x: 6, y: 6, z: 0, heading: 0, length: 0.55, width: 0.55, height: 1.75 },
  ],
  perceived: [],
  facilities: [
    { kind: 'building', x: 30, y: 30, z: 0, length: 20, width: 20, height: 40, heading: 0 },
    { kind: 'pier', x: 24, y: 0, z: 0, length: 1.2, width: 1.2, height: 3.2, heading: 0 },
    { kind: 'bollard', x: 8, y: -3.2, z: 0, length: 0.18, width: 0.18, height: 0.78, heading: 0 },
  ],
  predictions: [],
  pointCloud: [],
  roadNetwork: { edges: [] },
  layerVisibility: { predictions: true, perception: true },
  env: { isNight: false, lighting: 'day' },
  isBev: false,
};

const scene = new THREE.Scene();
const view = createPerceptionView(scene);
view.update(store, 0);

// ── 找到带顶点色的 LineSegments（完整线框） ──
const wires = scene.children.filter(
  (o) => o && o.geometry && o.geometry.attributes && o.geometry.attributes.color &&
    o.geometry.attributes.position && o.geometry.attributes.position.array.length === 128 * 72,
);
eq('线框 mesh 已创建（128 实体缓冲）', wires.length, 1);

const wire = wires[0];
const pos = wire.geometry.attributes.position.array;
const col = wire.geometry.attributes.color.array;
const drawVerts = wire.geometry.drawRange.count;

// car + pedestrian + bollard = 3 个感知目标；building/pier 不算
eq('装饰建筑/桥墩不画感知框，仅 3 个真实危险目标', drawVerts, 3 * 24);

let finite = true;
let minY = Infinity;
let maxY = -Infinity;
const usedVerts = drawVerts * 3;
for (let i = 0; i < usedVerts; i += 3) {
  if (!Number.isFinite(pos[i]) || !Number.isFinite(pos[i + 1]) || !Number.isFinite(pos[i + 2])) finite = false;
  minY = Math.min(minY, pos[i + 1]);
  maxY = Math.max(maxY, pos[i + 1]);
}
ok('全部线框顶点有限（无 NaN/Inf）', finite);
ok('线框有高度（底环 < 顶环）', maxY - minY > 1.5);

// 距离排序：pedestrian(6m) → bollard(8.6m) → car(12m)
const rgb = (hex) => [((hex >> 16) & 255) / 255, ((hex >> 8) & 255) / 255, (hex & 255) / 255];
const closeRgb = (off, hex) => {
  const [r, g, b] = rgb(hex);
  return Math.abs(col[off] - r) < 1e-6 && Math.abs(col[off + 1] - g) < 1e-6 && Math.abs(col[off + 2] - b) < 1e-6;
};
ok('行人线框使用 percPedestrian 分类色', closeRgb(0, SCENE.percPedestrian));
ok('静态危险物线框使用 percStatic 分类色', closeRgb(24 * 3, SCENE.percStatic));
ok('车辆线框使用 percVehicle 分类色', closeRgb(48 * 3, SCENE.percVehicle));

// 每框 12 条边：底环 4 / 顶环 4 / 竖边 4 —— 竖边两端 y 不同
let verticalEdges = 0;
for (let box = 0; box < 3; box++) {
  for (let e = 0; e < 12; e++) {
    const base = box * 72 + e * 6;
    if (Math.abs(pos[base + 1] - pos[base + 4]) > 0.3) verticalEdges++;
  }
}
eq('每框 4 条竖边（12 边完整线框）', verticalEdges, 12);

// ── 感知源三态：GT / Perception / Diff ──
store.perceptionMode = 'gt';
view.update(store, 0);
eq('GT 模式：只画真值 + 静态危险物', wire.geometry.drawRange.count, 3 * 24);

store.perceptionMode = 'perception';
store.perceived = [
  { id: 1, type: 'car', x: 12, y: 0, z: 0, heading: 0, length: 4.6, width: 2.0, height: 1.5, vx: 8 },
  { id: 77, type: 'car', x: 20, y: 3, z: 0, heading: 0, length: 4.6, width: 2.0, height: 1.5, vx: 6 },
];
view.update(store, 0);
eq('Perception 模式：只画感知输出，不画装饰设施', wire.geometry.drawRange.count, 2 * 24);

store.perceptionMode = 'diff';
view.update(store, 0);
eq('Diff 模式：真值与感知合并去重（重复车只留一个）', wire.geometry.drawRange.count, 4 * 24);

store.perceived = [];
store.perceptionMode = 'diff';

// 关闭感知层后线框应隐藏
store.layerVisibility.perception = false;
view.update(store, 0);
ok('perception 层关闭时线框隐藏', wire.visible === false);

view.dispose();
ok('dispose 不抛错', true);

done();
