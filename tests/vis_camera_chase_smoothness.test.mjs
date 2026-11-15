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

console.log('\n--- 3. 左转/掉头/右转后 chase/driver/front 视角矢量方向门禁 ---');
// 连续左转收敛到 +pi/2 后，chase 必须仍在车尾之后（ENU forward=(0,+1) -> Three forward=(0,-1)）
{
  const leftSteps = 45;
  for (let i = 1; i <= leftSteps; i++) {
    simT += 0.05;
    const h = (Math.PI / 2) * Math.min(1, i / 20);
    rig.update({ x: 220, y: 10 + i * 0.5, z: 0, heading: h }, null, simT * 1000);
  }
  const egoX = 220;
  const egoY = 10 + leftSteps * 0.5;
  const egoZ = -egoY;
  // Three.js forward at heading=+pi/2 is (0, -1): cam must be at z > egoZ (behind), look at z < egoZ (ahead)
  const behindDot = (egoX - camPos.x) * 0 + (egoZ - camPos.z) * (-1);
  const lookAheadDot = (camLook.x - egoX) * 0 + (camLook.z - egoZ) * (-1);
  assert('连续左转 90° 收敛后追车相机位于车尾之后（toEgo · forward > 8.5m）',
    behindDot > 8.5, `behindDot=${behindDot.toFixed(3)}, camZ=${camPos.z.toFixed(2)}, egoZ=${egoZ.toFixed(2)}`);
  assert('连续左转 90° 收敛后注视点位于车头前方（lookAhead · forward > 3.5m）',
    lookAheadDot > 3.5, `lookAheadDot=${lookAheadDot.toFixed(3)}`);
}

for (const { label, heading } of [
  { label: '左转 +90° (heading=+π/2)', heading: Math.PI / 2 },
  { label: '掉头 180° (heading=π)', heading: Math.PI },
  { label: '右转 -90° (heading=-π/2)', heading: -Math.PI / 2 },
]) {
  const dirRig = createCameraRig(canvas);
  let dirLook = null;
  dirRig.camera.lookAt = (x, y, z) => { dirLook = { x, y, z }; };
  const dirPos = dirRig.camera.position;
  const ego = { x: 200, y: 40, z: 0, heading };
  const egoZ = -ego.y;
  const fwdX = Math.cos(heading);
  const fwdZ = -Math.sin(heading);

  dirRig.setMode('chase');
  dirRig.update(ego, null, ++simT * 1000);
  const chaseToEgoDot = (ego.x - dirPos.x) * fwdX + (egoZ - dirPos.z) * fwdZ;
  const chaseLookDot = (dirLook.x - ego.x) * fwdX + (dirLook.z - egoZ) * fwdZ;
  assert(`${label} chase 相机在车尾后方 9m`, Math.abs(chaseToEgoDot - 9.0) < 1e-4, `dot=${chaseToEgoDot}`);
  assert(`${label} chase 注视点在车头前方 4m`, Math.abs(chaseLookDot - 4.0) < 1e-4, `dot=${chaseLookDot}`);

  dirRig.setMode('driver');
  dirRig.update(ego, null, ++simT * 1000);
  const drvCamDot = (dirPos.x - ego.x) * fwdX + (dirPos.z - egoZ) * fwdZ;
  const drvLookDot = (dirLook.x - dirPos.x) * fwdX + (dirLook.z - dirPos.z) * fwdZ;
  assert(`${label} driver 视角向前看（cam=+1m, look=+20m -> delta=+19m）`,
    Math.abs(drvCamDot - 1.0) < 1e-4 && Math.abs(drvLookDot - 19.0) < 1e-4,
    `camDot=${drvCamDot.toFixed(2)} lookDot=${drvLookDot.toFixed(2)}`);

  dirRig.setMode('front');
  dirRig.update(ego, null, ++simT * 1000);
  const frtCamDot = (dirPos.x - ego.x) * fwdX + (dirPos.z - egoZ) * fwdZ;
  const frtLookFromCamDot = (dirLook.x - dirPos.x) * (-fwdX) + (dirLook.z - dirPos.z) * (-fwdZ);
  assert(`${label} front 相机位于车头前 8m 回看自车中心`,
    Math.abs(frtCamDot - 8.0) < 1e-4 && Math.abs(frtLookFromCamDot - 8.0) < 1e-4,
    `camDot=${frtCamDot.toFixed(2)} lookFromCamDot=${frtLookFromCamDot.toFixed(2)}`);
}

console.log(`\n=== summary: ${pass} pass, ${fail} fail ===`);
process.exit(fail > 0 ? 1 : 0);
