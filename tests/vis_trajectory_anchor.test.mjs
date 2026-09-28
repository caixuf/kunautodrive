/**
 * vis_trajectory_anchor.test.mjs — 规划轨迹"起点连续锚定"回归测试。
 *
 * 回归点（commit d2fca62 引入）：车头锚定被改成"距离 < 3m 才吸附 +
 * 首段太短就跳过吸附"两重开关。规划快照 10Hz、车 60fps 持续运动，
 * 车头到轨迹首点的距离会在阈值附近逐帧穿越 → 吸附状态以 10Hz 翻转 →
 * 起点在"车头"与"旧规划点"之间弹跳，观感"一抽一抽"（速度高时干脆
 * 永不吸附，轨迹拖在车后 → 丑）。
 *
 * 本测试锁死"连续锚定"契约：合理范围内无条件吸附，不做细分开关。
 */

import {
  _anchorTrajectoryStart,
  ANCHOR_MAX_DIST2,
  ANCHOR_MIN_SEG2,
} from '../tools/flowboard/js/vis/math/Trajectory.js';
import { ok, done } from './test-utils.mjs';

/** 最小可锚定点：只需 x/y/z/v + set()（与 THREE.Vector3 的用法同构）。 */
function pt(x, z, v = 0) {
  return {
    x, y: 0, z, v,
    set(nx, ny, nz) { this.x = nx; this.y = ny; this.z = nz; return this; },
  };
}

function assertAnchored(name, raws, ax, az) {
  _anchorTrajectoryStart(raws, ax, 0, az, 7);
  ok(name, raws[0].x === ax && raws[0].z === az);
}

/* ── 1. 阈值内一律吸附：不存在"3m 悬崖"（旧 bug 的翻转点）── */
console.log('\n=== 连续锚定（无阈值翻转）===');
for (const d of [0, 1.5, 2.5, 2.9, 3.0, 3.1, 3.5, 4.0, 4.9]) {
  assertAnchored(`dist=${d}m 时吸附到车头（无 3m 悬崖）`,
    [pt(0, 0), pt(5, 0), pt(10, 0)], d, 0);
}

/* ── 2. 兜底上限之外（拿到不相关/陈旧路径）→ 不锚定，路径原样保留 ── */
const staleRaws = [pt(0, 0), pt(5, 0), pt(10, 0)];
_anchorTrajectoryStart(staleRaws, 6.0, 0, 0, 7);   // 6m > 5m 上限
ok('dist=6m（超上限）时不锚定，路径原样保留',
  staleRaws[0].x === 0 && staleRaws[0].z === 0);
ok('ANCHOR_MAX_DIST2 == 25（5m 兜底上限）', ANCHOR_MAX_DIST2 === 25.0);

/* ── 3. 核心回归：10Hz 规划快照 + 60fps 车运动，起点必须连续平滑 ──
 * 每帧 raw3d[0] 被重建成"该快照的旧规划首点"，再锚定到实时车头。
 * 旧代码在车头到旧首点距离穿越 3m 的帧会漏吸附 → 起点逐帧 0/跳变交替。
 * 修复后：只要在 5m 上限内，每帧都吸附 → 每帧位移恒等于车速×dt。 */
console.log('\n=== 10Hz 快照 / 60fps 运动：无逐帧弹跳 ===');
{
  const v = 20;            // m/s，使 100ms 内前进 2m，跨越旧 3m 阈值
  const dt = 1 / 60;
  const FRONT_OFFSET = 1.5; // 锚点 = 车头前方 1.5m
  const SNAP_FRAMES = Math.round(0.1 / dt); // 每 6 帧一次规划快照（10Hz）

  let egoX = 0;            // 车中心沿 +x 前进
  let snapStartX = 0;      // 当前快照的轨迹首点（旧规划点）
  const startDeltas = [];
  let prevStart = null;

  for (let f = 0; f < 60; f++) {
    if (f % SNAP_FRAMES === 0) snapStartX = egoX; // 新快照：首点 = 当时的车位置
    const frontX = egoX + FRONT_OFFSET;

    // _buildSmoothPoints 每帧重建 raw3d：首点 = 当前快照的旧规划点
    const raws = [pt(snapStartX, 0), pt(snapStartX + 5, 0), pt(snapStartX + 10, 0)];
    _anchorTrajectoryStart(raws, frontX, 0, 0, 7);

    if (prevStart !== null) startDeltas.push(raws[0].x - prevStart);
    prevStart = raws[0].x;
    egoX += v * dt;
  }

  const expected = v * dt;
  const maxDelta = Math.max(...startDeltas);
  const minDelta = Math.min(...startDeltas);
  ok('每帧起点位移恒等于车速×dt（连续锚定，无 0/跳变交替）',
    Math.abs(maxDelta - expected) < 1e-9 && Math.abs(minDelta - expected) < 1e-9);

  // 反向确认：旧代码会出现的"某帧漏吸附"= 该帧起点位移为 0 或骤跳。
  const framesNotMoving = startDeltas.filter((d) => Math.abs(d) < 1e-9).length;
  ok('全程没有任何一帧起点停滞（旧 bug 的弹跳特征）', framesNotMoving === 0);

  // 跨快照边界也连续：新快照把旧首点瞬间拉回，但车头锚定保证渲染起点不变。
  const boundaryJump = Math.abs(
    startDeltas[SNAP_FRAMES - 1] - expected + (startDeltas[SNAP_FRAMES] - expected),
  );
  ok('跨 10Hz 快照边界起点不跳变', boundaryJump < 1e-9);
}

/* ── 4. 退化保护：吸附后首段塌缩成一点 → 丢点，避免样条 NaN 抖动 ── */
console.log('\n=== 首段退化保护 ===');
{
  const raws = [pt(0, 0), pt(1.03, 0), pt(6, 0)]; // 车头落点与第 1 点仅差 0.03m
  _anchorTrajectoryStart(raws, 1.0, 0, 0, 7);
  const seg2 = (raws[1].x - raws[0].x) ** 2 + (raws[1].z - raws[0].z) ** 2;
  ok('首段 <0.1m 时丢弃该点（无零长段）', raws.length === 2 && seg2 > ANCHOR_MIN_SEG2);
  ok('ANCHOR_MIN_SEG2 == 0.01（0.1m）', ANCHOR_MIN_SEG2 === 0.01);
}
{
  const raws = [pt(0, 0), pt(2.0, 0), pt(6, 0)]; // 首段正常
  _anchorTrajectoryStart(raws, 1.0, 0, 0, 7);
  ok('首段正常时保留全部点', raws.length === 3);
}

/* ── 5. 速度沿用原规划首点，不被锚点覆盖成 0 ── */
{
  const raws = [pt(0, 0, 0), pt(5, 0, 3)];
  _anchorTrajectoryStart(raws, 1.0, 0, 0, 9.5);
  ok('锚定后首点速度 = 原规划首点速度（传参 v0）', raws[0].v === 9.5);
}

done();
