/**
 * vis_camera_chase_smoothness.test.mjs — 追车视角动态防顿挫门禁测试
 *
 * 背景：2026-08 顿挫复盘已明确指出：对已被 DeadReckon 平滑过的 ego 位置
 * 再叠一层追车相机位置指数平滑，会与车体渲染形成双低通相位差，导致帧率波动或
 * SSE 交付抖动时"车相对画面前后蹿"（一顿一顿）。
 *
 * 本测试模拟 60Hz/30Hz 帧率抖动 + 离散 SSE 推送 + 变道转向，验证：
 *   1. chase 模式下相机位置与 ego 平滑位置保持刚性锁定（距离恒等于 behind=9m，
 *      垂向高度恒等于 height=3m），零相位滞后、零前后拉扯；
 *   2. 转向过程中距离同样严格保持 9m，无发散或振荡。
 *
 * 跑法：node --import ./tests/support/three-preload.mjs tests/vis_camera_chase_smoothness.test.mjs
 */

import { createCameraRig } from '../tools/flowboard/js/vis/core/CameraRig.js';
import {
  initDeadReckon,
  updateDeadReckon,
  tickDeadReckon,
  getDeadReckonState,
} from '../tools/flowboard/js/vis/core/DeadReckon.js';

let pass = 0, fail = 0;
function assert(name, cond, detail = '') {
  if (cond) {
    pass++;
    console.log('  PASS  ' + name);
  } else {
    fail++;
    console.log('  FAIL  ' + name + (detail ? '  (' + detail + ')' : ''));
  }
}

const canvas = {
  clientWidth: 1280,
  clientHeight: 720,
  addEventListener: () => {},
  removeEventListener: () => {},
  ownerDocument: { documentElement: {} },
  style: {},
  setPointerCapture: () => {},
  releasePointerCapture: () => {},
};

const rig = createCameraRig(canvas);
rig.setMode('chase');

let camPos = null;
let camLook = null;
rig.camera.position.set = (x, y, z) => { camPos = { x, y, z }; };
rig.camera.lookAt = (x, y, z) => { camLook = { x, y, z }; };

console.log('--- 1. 直线巡航 + 帧率抖动 (16ms/33ms/50ms) + 20Hz SSE 推送 ---');
initDeadReckon();

const SPEED = 15.0; // m/s
let maxDistErr = 0;
let maxHeightErr = 0;
let maxLookDistErr = 0;

// 模拟 120 帧渲染，期间穿插掉帧 (33ms / 50ms) 和 50ms 间隔数据推送
let simT = 0;
let nextDataT = 0;
const frameDts = [0.016, 0.016, 0.033, 0.016, 0.050, 0.016, 0.017, 0.033];

for (let i = 0; i < 120; i++) {
  const dt = frameDts[i % frameDts.length];
  simT += dt;
  if (simT >= nextDataT) {
    const trueX = SPEED * nextDataT;
    updateDeadReckon(trueX, 0, SPEED, 0, SPEED, 0, 0, nextDataT + 1.0);
    nextDataT += 0.05; // 20Hz
  }
  tickDeadReckon();
  const dr = getDeadReckonState();
  const ego = {
    x: dr.smoothX,
    y: dr.smoothZ, // DeadReckon 第二参数 z 对应 sim 横向 y
    z: 6.0,
    heading: dr.smoothHeading,
  };
  rig.update(ego, null, simT * 1000);

  const horizDist = Math.hypot(camPos.x - ego.x, camPos.z - (-ego.y));
  const heightDiff = camPos.y - ego.z;
  const lookHorizDist = Math.hypot(camLook.x - ego.x, camLook.z - (-ego.y));

  maxDistErr = Math.max(maxDistErr, Math.abs(horizDist - 9.0));
  maxHeightErr = Math.max(maxHeightErr, Math.abs(heightDiff - 3.0));
  maxLookDistErr = Math.max(maxLookDistErr, Math.abs(lookHorizDist - 4.0));
}

assert('追车相机水平跟车距离恒定 9.0m（零前后拉扯）', maxDistErr < 1e-6, `maxErr=${maxDistErr}`);
assert('追车相机垂向高度差恒定 3.0m', maxHeightErr < 1e-6, `maxErr=${maxHeightErr}`);
assert('追车相机注视点前瞻距恒定 4.0m', maxLookDistErr < 1e-6, `maxErr=${maxLookDistErr}`);

console.log('--- 2. 转向/变道动态过程（heading 连续变化）---');
let maxTurnDistErr = 0;
for (let i = 0; i < 60; i++) {
  const dt = frameDts[i % frameDts.length];
  simT += dt;
  const heading = 0.35 * Math.sin(i * 0.08);
  const ego = {
    x: 100 + i * 0.25,
    y: 2.0 * Math.sin(i * 0.08),
    z: 6.0,
    heading,
  };
  rig.update(ego, null, simT * 1000);
  const horizDist = Math.hypot(camPos.x - ego.x, camPos.z - (-ego.y));
  maxTurnDistErr = Math.max(maxTurnDistErr, Math.abs(horizDist - 9.0));
}
assert('转向过程中相机距车体仍恒定 9.0m（无滞后甩尾拉扯）', maxTurnDistErr < 1e-6, `maxErr=${maxTurnDistErr}`);

console.log(`\n=== summary: ${pass} pass, ${fail} fail ===`);
process.exit(fail > 0 ? 1 : 0);
