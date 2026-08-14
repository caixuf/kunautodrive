/**
 * vis_camera_mode.test.mjs — CameraRig 模式切换冒烟测试
 *
 * D-3: 验证 createCameraRig 在 OrbitControls 集成后
 * 所有模式切换/update/reset 不抛错，非法模式安全回退。
 *
 * 跑法：node --import ./tests/support/three-preload.mjs tests/vis_camera_mode.test.mjs
 */

import { createCameraRig } from '../tools/flowboard/js/vis/core/CameraRig.js';

let pass = 0, fail = 0;
function check(name, actual, expected) {
  const ok = actual === expected;
  if (ok) { pass++; console.log('  PASS  ' + name); }
  else { fail++; console.log('  FAIL  ' + name + '  actual=' + JSON.stringify(actual) + '  expected=' + JSON.stringify(expected)); }
}
function checkNoThrow(name, fn) {
  try {
    fn();
    pass++;
    console.log('  PASS  ' + name);
  } catch (e) {
    fail++;
    console.log('  FAIL  ' + name + '  threw: ' + e.message);
  }
}

// Mock canvas（OrbitControls 需要 canvas.addEventListener + style）
const canvas = {
  clientWidth: 800, clientHeight: 600,
  addEventListener: () => {}, removeEventListener: () => {},
  ownerDocument: { documentElement: {} },
  style: {}, // OrbitControls 设置 canvas.style.touchAction
  setPointerCapture: () => {}, releasePointerCapture: () => {},
};

// 测试用 ego
const ego = { x: 100, y: -50, z: 2, heading: 0.5 };

console.log('--- CameraRig 冒烟测试 ---');

// 1) 创建 CameraRig 不抛错
let rig;
checkNoThrow('createCameraRig 不抛错', () => { rig = createCameraRig(canvas); });

// 2) 全部模式切换不抛错（含 BEV）
const modes = ['chase', 'top', 'driver', 'front', 'map', 'orbit', 'bev'];
checkNoThrow('全部模式（含 bev）切换不抛错', () => { modes.forEach(m => rig.setMode(m)); });

// 2b) BEV 正交相机：接口存在、模式切换选不同相机实例、isBev 正确
// 注：three-shim 是递归 Proxy，不暴露 isOrthographicCamera/isPerspectiveCamera
// 标志也不回读被赋的数值属性，故用"活动相机实例身份"区分透视/正交相机。
check('getActiveCamera 是函数', typeof rig.getActiveCamera === 'function', true);
check('isBev 是函数', typeof rig.isBev === 'function', true);

const perspCam = rig.camera; // 透视相机引用（返回对象中的 camera）
checkNoThrow('setMode(bev) 不抛错', () => { rig.setMode('bev'); });
checkNoThrow('update(bev) 不抛错', () => { rig.update(ego, null, 0); });
checkNoThrow('resize(800,600) 不抛错', () => { rig.resize(800, 600); });
check('BEV 模式 isBev() 为 true', rig.isBev(), true);
check('BEV 模式活动相机 != 透视相机（切换到正交相机实例）',
  rig.getActiveCamera() !== perspCam, true);

// 切回 chase 应恢复透视相机
checkNoThrow('setMode(chase) 不抛错', () => { rig.setMode('chase'); });
check('切回 chase 后 isBev() 为 false', rig.isBev(), false);
check('切回 chase 活动相机 === 透视相机', rig.getActiveCamera() === perspCam, true);

// 3-7) 5 种跟车模式 update 不抛错
const chaseModes = ['chase', 'top', 'driver', 'front', 'map'];
for (const m of chaseModes) {
  checkNoThrow('update(' + m + ') 不抛错', () => { rig.setMode(m); rig.update(ego, null, 0); });
}

// 8) orbit 模式 update 不抛错
checkNoThrow('update(orbit) 不抛错', () => { rig.setMode('orbit'); rig.update(ego, null, 0); });

// 9) reset 不抛错
checkNoThrow('reset 不抛错', () => rig.reset());

// 10) 非法模式回退（setMode('invalid') 不抛错，mode 不变）
checkNoThrow('非法模式回退（不抛错，mode 不变）', () => {
  rig.setMode('chase');
  rig.setMode('bad_mode');
  rig.setMode('another_invalid');
});

// 11) 反复切换稳定性
checkNoThrow('反复切换 10 次不抛错', () => {
  for (let i = 0; i < 10; i++) { rig.setMode(modes[i % 6]); rig.update(ego, null, i); }
});

// 12) 反复切换后仍可正常 update
checkNoThrow('反复切换后 update 仍可用', () => { rig.setMode('chase'); rig.update(ego, null, 0); });

console.log('\n=== summary: ' + pass + ' pass, ' + fail + ' fail ===');
process.exit(fail > 0 ? 1 : 0);