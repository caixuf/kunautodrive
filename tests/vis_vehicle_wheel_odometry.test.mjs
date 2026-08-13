/**
 * vis_vehicle_wheel_odometry.test.mjs — 车轮滚动里程回归测试。
 *
 * 回归点：车速不变时车轮应匀速转动，不应因渲染帧率 / 帧间隔抖动而忽快忽慢。
 * 里程由"沿车头方向累计的平滑位姿位移"积分得到，与 dt / 速度信号抖动解耦。
 */

import { _advanceWheelOdometry } from '../tools/flowboard/js/vis/view/VehicleView.js';
import { ok, done } from './test-utils.mjs';

// 用平滑位姿增量模拟一辆以恒定 speed 沿 heading=0 直线前进的车。
// 每帧真实位移 = speed×dt（与真实死推算 smoothX 同构：每帧外推推进），
// 同时保留一个"会台阶式跳动"的 raw x 以验证里程优先用平滑位姿。
function simulate(entry, frames) {
  let odo = 0;
  let walked = 0;
  for (const f of frames) {
    walked += f.speed * f.dt;      // 本帧真实位移 = speed×dt
    const v = {
      smoothX: walked,             // 平滑位姿：每帧平滑推进
      smoothZ: 0,
      smoothHeading: 0,
      x: f.rawX !== undefined ? f.rawX : walked, // raw：可台阶跳动
      y: 0,
      heading: 0,
      speed: f.speed,
    };
    odo = _advanceWheelOdometry(entry, v, f.dt, f.speed);
  }
  return odo;
}

// ── 1. 帧率无关：同样匀速走 10m，60fps 与 10fps 累计里程一致 ──
const odo60 = simulate({}, Array.from({ length: 60 }, () => ({ dt: 1 / 60, speed: 10 })));
const odo10 = simulate({}, Array.from({ length: 10 }, () => ({ dt: 0.1, speed: 10 })));
ok('匀速 10m：60fps 与 10fps 里程一致（帧率无关）',
  Math.abs(odo60 - odo10) < 1e-9 && Math.abs(odo60 - 10) < 1e-9);

// ── 2. 匀速 + 抖动 dt：里程仍等于真实路程，不放大抖动 ──
const jittery = [
  { dt: 0.016, speed: 10 },
  { dt: 0.020, speed: 10 },
  { dt: 0.015, speed: 10 },
  { dt: 0.018, speed: 10 },
  { dt: 0.022, speed: 10 },
  { dt: 0.014, speed: 10 },
];
const odoJitter = simulate({}, jittery);
const trueDist = jittery.reduce((s, f) => s + f.speed * f.dt, 0);
ok('抖动 dt 下里程 == 真实路程（不随帧间隔抖动）',
  Math.abs(odoJitter - trueDist) < 1e-9);

// ── 3. 每帧里程增量均匀（匀速）→ 车轮角速度恒定，无忽快忽慢 ──
// 关键：车轮取决于"实际走过的路程"（位置增量），而平滑位姿由 sim 钟驱动、
// 每帧匀速推进，与渲染帧间隔 dt 抖动无关。故即便 dt 抖动，只要位置匀速
// 推进，每帧里程增量恒定 → 车轮匀速转动。首帧 seed 用 speed×dt，会偏离，
// 故先 warm-up 一帧，只校验后续帧的增量。
function perFrameDeltasUniform(entry, frames) {
  const out = [];
  let prev = 0;
  let walked = 0;
  const STEP = 10 / 60; // 平滑位姿每帧匀速推进量（与 dt 无关）
  let first = true;
  for (const f of frames) {
    walked += STEP;
    const v = { smoothX: walked, smoothZ: 0, smoothHeading: 0, x: walked, y: 0, heading: 0, speed: f.speed };
    const odo = _advanceWheelOdometry(entry, v, f.dt, f.speed);
    if (!first) out.push(odo - prev);
    prev = odo;
    first = false;
  }
  return out;
}
const deltas = perFrameDeltasUniform({}, Array.from({ length: 30 }, (_, i) => ({
  dt: i % 2 ? 0.020 : 0.014, speed: 10,
})));
const maxDelta = Math.max(...deltas);
const minDelta = Math.min(...deltas);
ok('匀速时每帧里程增量恒定（车轮匀速转动，与 dt 抖动解耦）',
  maxDelta - minDelta < 1e-9);

// ── 4. 优先用平滑位姿：raw x 台阶跳动、smoothX 平滑推进时里程跟平滑 ──
const entryMixed = {};
let odoMixed = 0;
let smoothWalked = 0;
// 每帧 rawX 只在前 3 帧变（模拟 SSE tick 落点），其余帧卡住；smoothX 每帧推进。
for (let i = 0; i < 12; i++) {
  smoothWalked += 10 / 60;
  const rawX = i < 3 ? (i + 1) * (10 / 60) : 3 * (10 / 60); // 之后恒定
  const v = { smoothX: smoothWalked, smoothZ: 0, smoothHeading: 0, x: rawX, y: 0, heading: 0, speed: 10 };
  odoMixed = _advanceWheelOdometry(entryMixed, v, 1 / 60, 10);
}
ok('raw 台阶跳动时里程仍跟随平滑位姿（ego 不卡轮）',
  Math.abs(odoMixed - smoothWalked) < 1e-9);

// ── 5. 倒车：沿车头反方向位移 → 里程递减（车轮反转）──
const entryRev = {};
let odoRev = 0;
let revWalked = 0;
for (let i = 0; i < 10; i++) {
  revWalked -= 5 / 60; // 倒车
  const v = { smoothX: revWalked, smoothZ: 0, smoothHeading: 0, x: revWalked, y: 0, heading: 0, speed: -5 };
  odoRev = _advanceWheelOdometry(entryRev, v, 1 / 60, -5);
}
ok('倒车里程为负（车轮反转）', odoRev < 0 && Math.abs(odoRev - revWalked) < 1e-9);

// ── 6. 钳制：远超预期的突变位移被限幅，不会整圈疯转（只校验本帧增量）──
const entryClamp = {};
const seedOdo = _advanceWheelOdometry(entryClamp, { smoothX: 0, smoothZ: 0, smoothHeading: 0, x: 0, y: 0, heading: 0, speed: 10 }, 0.1, 10);
const odoClamp = _advanceWheelOdometry(entryClamp, { smoothX: 50, smoothZ: 0, smoothHeading: 0, x: 50, y: 0, heading: 0, speed: 10 }, 0.1, 10);
const clampInc = odoClamp - seedOdo;
const cap = Math.max(0.5, 10 * 0.1 * 3); // 与实现一致
ok('瞬移位移被钳制（不会整圈疯转）', clampInc > 0 && clampInc <= cap + 1e-9);

done();
