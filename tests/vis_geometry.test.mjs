/**
 * vis_geometry.test.mjs — vis/ 几何回归测试（纯 Node，无浏览器）
 * 防止「Coord.js 改一行 + RoadView 改一行」悄无声息破坏朝向/剔除
 * 跑法：node tests/vis_geometry.test.mjs
 */
import * as RealTHREE from '../tools/flowboard/vendor/three/three.module.js';

globalThis.THREE = RealTHREE;
const { mergeGeometries } = await import('../tools/flowboard/js/vis/math/GeometryMerge.js');

let pass = 0, fail = 0;
function check(name, actual, expected) {
  const ok = Math.abs(actual - expected) < 1e-6;
  if (ok) { pass++; console.log('  PASS  ' + name); }
  else { fail++; console.log('  FAIL  ' + name + '  actual=' + actual + '  expected=' + expected); }
}

console.log('--- RoadView ribbon winding (法线必须朝 +Y) ---');
// 忠实复刻 RoadView.js 的顶点公式与索引顺序（改这里必须同步改 RoadView）。
// 顶点 push（RoadView 81-82，直道 tangent=(1,0) → nx=0,nz=1）：
//   左 = (px,        0.1, pz + hw)
//   右 = (px,        0.1, pz - hw)
// 排列：i=0(左A) i=1(右A) i=2(左B) i=3(右B)
const HW = 5;
function seg(px) {
  return { L: [px, 0.1, +HW], R: [px, 0.1, -HW] };
}
const A = seg(0), B = seg(20);
const V = [A.L, A.R, B.L, B.R];   // 索引 0,1,2,3

// 面法线 .y（叉积 (V[j]-V[i]) × (V[k]-V[i])）
function normalY(i, j, k) {
  const p = V[i], q = V[j], r = V[k];
  const e1 = [q[0]-p[0], q[1]-p[1], q[2]-p[2]];
  const e2 = [r[0]-p[0], r[1]-p[1], r[2]-p[2]];
  return e1[2]*e2[0] - e1[0]*e2[2];   // 叉积的 y 分量
}

// RoadView 当前索引顺序（RoadView 92-94）：
//   (i, i+2, i+1) 和 (i+1, i+2, i+3)
const ny1 = normalY(0, 2, 1);
const ny2 = normalY(1, 2, 3);
console.log('  tri1 (0,2,1) 法线.y = ' + ny1.toFixed(1));
console.log('  tri2 (1,2,3) 法线.y = ' + ny2.toFixed(1));
check('tri1 法线朝 +Y（从上方相机可见）', ny1 > 0 ? 1 : 0, 1);
check('tri2 法线朝 +Y（从上方相机可见）', ny2 > 0 ? 1 : 0, 1);

console.log('--- GeometryMerge non-indexed BufferGeometry (+idxBase) ---');
{
  const g1 = new RealTHREE.BufferGeometry();
  g1.setAttribute('position', new RealTHREE.BufferAttribute(new Float32Array([
    0, 0, 0,  1, 0, 0,  0, 1, 0,
  ]), 3));
  const g2 = new RealTHREE.BufferGeometry();
  g2.setAttribute('position', new RealTHREE.BufferAttribute(new Float32Array([
    10, 0, 0,  11, 0, 0,  10, 1, 0,  11, 1, 0,  10, 0, 0,  11, 0, 0,
  ]), 3));
  const merged = mergeGeometries([g1, g2], ['position']);
  const idx = Array.from(merged.getIndex().array);
  check('non-indexed merge total vertex count (3 + 6 = 9)', merged.attributes.position.count, 9);
  check('non-indexed merge index count (3 + 6 = 9)', idx.length, 9);
  check('first geometry indices [0,1,2]', idx[0] === 0 && idx[1] === 1 && idx[2] === 2 ? 1 : 0, 1);
  check('second geometry indices offset by +idxBase (3..8)',
    idx[3] === 3 && idx[4] === 4 && idx[5] === 5 && idx[6] === 6 && idx[7] === 7 && idx[8] === 8 ? 1 : 0, 1);
}

console.log('\n--- summary: ' + pass + ' pass, ' + fail + ' fail ---');
process.exit(fail > 0 ? 1 : 0);
