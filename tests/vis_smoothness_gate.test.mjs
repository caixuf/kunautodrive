/**
 * vis_smoothness_gate.test.mjs — 视觉丝滑门禁（摆头 / 卡顿 / 倒退回弹）
 *
 * 自动闭环检验人眼可感知的三类渲染退化：
 *   1. SSE 卡顿 + 时钟漂移下，DeadReckon 不得出现"车辆倒退一点再往前跑"的回弹；
 *   2. 上匝道入弯阶段，DeadReckon 航向角不得出现左右摆头反转（headingReversals=0）；
 *   3. 尾随相机（CameraRig chase）在卡顿恢复瞬间不得出现前向倒退跳变。
 */
import {
  initDeadReckon,
  updateDeadReckon,
  tickDeadReckon,
  getDeadReckonState,
} from '../tools/flowboard/js/vis/core/DeadReckon.js';
import { createCameraRig } from '../tools/flowboard/js/vis/core/CameraRig.js';

let pass = 0;
let fail = 0;
function ok(name, cond) {
  if (cond) { pass++; console.log('  PASS  ' + name); }
  else      { fail++; console.log('  FAIL  ' + name); }
}

function waitMs(ms) {
  const start = Date.now();
  while (Date.now() - start < ms) { /* busy-wait for real wall-clock dt */ }
}

const canvas = {
  clientWidth: 1280,
  clientHeight: 720,
  style: {},
  addEventListener: () => {},
  removeEventListener: () => {},
  setPointerCapture: () => {},
  releasePointerCapture: () => {},
  getRootNode: () => ({ addEventListener: () => {}, removeEventListener: () => {} }),
};
const rig = createCameraRig(canvas);
rig.setMode('chase');
let camPos = { x: 0, y: 0, z: 0 };
rig.camera.position.set = (x, y, z) => { camPos = { x, y, z }; };
rig.camera.lookAt = () => {};

// ── 1. SSE 卡顿 + 恢复防倒退门禁 ─────────────────────────────────────
console.log('--- 1. SSE 卡顿(450ms) + 恢复防倒退门禁 ---');
initDeadReckon();
const SPEED = 15.0;
let simT = 10.0;
let prevX = 0;
let prevCamX = -9.0;
let minFwdDelta = 1e9;
let minCamFwdDelta = 1e9;

for (let i = 0; i < 20; i++) {
  const x = SPEED * (simT - 10.0);
  updateDeadReckon(x, 0, SPEED, 0, SPEED, 0, 0, simT);
  tickDeadReckon();
  const dr = getDeadReckonState();
  rig.update({ x: dr.smoothX, y: dr.smoothZ, z: 0, heading: dr.smoothHeading }, null, Date.now());
  if (i > 1) {
    minFwdDelta = Math.min(minFwdDelta, dr.smoothX - prevX);
    minCamFwdDelta = Math.min(minCamFwdDelta, camPos.x - prevCamX);
  }
  prevX = dr.smoothX;
  prevCamX = camPos.x;
  waitMs(25);
  simT += 0.025;
}

// 模拟 450ms SSE 卡顿（期间持续 60Hz 渲染）
for (let k = 0; k < 15; k++) {
  waitMs(30);
  tickDeadReckon();
  const dr = getDeadReckonState();
  rig.update({ x: dr.smoothX, y: dr.smoothZ, z: 0, heading: dr.smoothHeading }, null, Date.now());
  minFwdDelta = Math.min(minFwdDelta, dr.smoothX - prevX);
  minCamFwdDelta = Math.min(minCamFwdDelta, camPos.x - prevCamX);
  prevX = dr.smoothX;
  prevCamX = camPos.x;
}

// 卡顿后收到滞后一点的后端包，连续恢复 10 帧
for (let r = 0; r < 10; r++) {
  simT += 0.05;
  const x = SPEED * (simT - 10.0);
  updateDeadReckon(x, 0, SPEED, 0, SPEED, 0, 0, simT);
  waitMs(25);
  tickDeadReckon();
  const dr = getDeadReckonState();
  rig.update({ x: dr.smoothX, y: dr.smoothZ, z: 0, heading: dr.smoothHeading }, null, Date.now());
  minFwdDelta = Math.min(minFwdDelta, dr.smoothX - prevX);
  minCamFwdDelta = Math.min(minCamFwdDelta, camPos.x - prevCamX);
  prevX = dr.smoothX;
  prevCamX = camPos.x;
}

ok(`卡顿+恢复 ego 前向无倒退回弹 (minFwdDelta=${minFwdDelta.toFixed(4)}m >= -0.01m)`, minFwdDelta >= -0.01);
ok(`卡顿+恢复 尾随相机无倒退跳变 (minCamFwdDelta=${minCamFwdDelta.toFixed(4)}m >= -0.05m)`, minCamFwdDelta >= -0.05);

// ── 2. 上匝道入弯无摆头门禁 ──────────────────────────────────────────
console.log('\n--- 2. 上匝道入弯无摆头门禁 ---');
initDeadReckon();
let x = 0, y = 0, h = 0;
const v = 12.0;
let prevH = 0;
let prevDh = 0;
let headingReversals = 0;
let maxDh = 0;

for (let i = 0; i < 40; i++) {
  const dt = (i % 3 === 0) ? 0.045 : 0.025;
  const yr = 0.14 * Math.min(1.0, i / 10.0);
  h += yr * dt;
  x += v * Math.cos(h) * dt;
  y += v * Math.sin(h) * dt;
  updateDeadReckon(x, y, v, h, v * Math.cos(h), v * Math.sin(h), yr, 20.0 + i * 0.03);
  waitMs(Math.round(dt * 1000));
  tickDeadReckon();
  const dr = getDeadReckonState();
  if (i > 2) {
    const dh = dr.smoothHeading - prevH;
    maxDh = Math.max(maxDh, Math.abs(dh));
    if (prevDh > 0.0005 && dh < -0.0005) headingReversals++;
    prevDh = dh;
  }
  prevH = dr.smoothHeading;
}

ok(`上匝道入弯零摆头反转 (headingReversals=${headingReversals})`, headingReversals === 0);
ok(`上匝道入弯单帧航向角步长平滑 (maxDh=${maxDh.toFixed(4)}rad < 0.05)`, maxDh < 0.05);

console.log('\n--- summary: ' + pass + ' pass, ' + fail + ' fail ---');
if (fail > 0) process.exit(1);
