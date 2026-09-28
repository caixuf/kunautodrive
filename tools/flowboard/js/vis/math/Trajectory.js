import { forwardENU } from './Coord.js';

function motionSign(v) {
  return v < -0.05 ? -1 : v > 0.05 ? 1 : 0;
}

/* 车头锚定的合理上限：车头与轨迹首点距离超过 ~5m 视为拿到不相关/陈旧路径，
 * 放弃锚定（不把错的路径硬拖到车头）。阈值以内一律连续吸附。 */
export const ANCHOR_MAX_DIST2 = 25.0;
/* 吸附后首段塌缩成一点的下限（0.1m）：丢点，避免 CatmullRom 在重合点上
 * 切线退化（NaN / 剧烈抖动）。 */
export const ANCHOR_MIN_SEG2 = 0.01;

/**
 * 把轨迹首点**连续**锚定到车头实时位置（防脱节）。
 *
 * 关键在"连续"：规划快照 10Hz，而车以 60fps 持续运动，若用小距离阈值决定
 * 吸附/不吸附（或"首段太短就跳过吸附"），车头与首点的距离会在阈值附近
 * 逐帧来回穿越 → 吸附状态以 10Hz 频率翻转 → 起点在"车头"与"旧规划点"
 * 之间弹跳，观感就是"一抽一抽"。因此这里只保留一个很宽的兜底上限，
 * 阈值内无条件吸附，不做任何细分开关。
 *
 * @param {Array<{x:number,y:number,z:number,v:number}>} raw3d 已转 THREE 坐标的原始轨迹点（原地修改）
 * @param {number} ax 车头 THREE x
 * @param {number} ay 车头 THREE y
 * @param {number} az 车头 THREE z
 * @param {number} v0 起点速度（沿用原规划首点速度）
 */
export function _anchorTrajectoryStart(raw3d, ax, ay, az, v0) {
  if (!raw3d || raw3d.length < 1) return;
  const dx = ax - raw3d[0].x;
  const dz = az - raw3d[0].z;
  if (dx * dx + dz * dz >= ANCHOR_MAX_DIST2) return;
  raw3d[0].set(ax, ay, az);
  raw3d[0].v = v0;
  if (raw3d.length >= 2) {
    const sx = raw3d[1].x - raw3d[0].x;
    const sz = raw3d[1].z - raw3d[0].z;
    if (sx * sx + sz * sz < ANCHOR_MIN_SEG2) raw3d.splice(1, 1);
  }
}

/**
 * Select the active forward/reverse stroke from a cached maneuver trajectory.
 * A gear change is a hard path boundary and must never be spline-smoothed.
 */
export function selectCurrentMotionSegment(trajPath, ego) {
  if (!trajPath || trajPath.length < 2 || !ego) return [];

  const heading = ego.heading || 0;
  const [forwardX, forwardY] = forwardENU(heading);
  const alongVelocity = (ego.vx || 0) * forwardX + (ego.vy || 0) * forwardY;
  const preferredSign = motionSign(alongVelocity);

  function findNearest(requiredSign) {
    let index = -1;
    let distance2 = Infinity;
    for (let i = 0; i < trajPath.length; i++) {
      if (requiredSign !== 0 && motionSign(trajPath[i][2] || 0) !== requiredSign) continue;
      const dx = trajPath[i][0] - ego.x;
      const dy = trajPath[i][1] - ego.y;
      const d2 = dx * dx + dy * dy;
      if (d2 < distance2) {
        index = i;
        distance2 = d2;
      }
    }
    return index;
  }

  let nearest = findNearest(preferredSign);
  if (nearest < 0) nearest = findNearest(0);
  if (nearest < 0) return [];

  let sign = motionSign(trajPath[nearest][2] || 0);
  if (sign === 0) {
    sign = preferredSign;
    for (let i = nearest + 1; i < trajPath.length && sign === 0; i++) {
      sign = motionSign(trajPath[i][2] || 0);
    }
    for (let i = nearest - 1; i >= 0 && sign === 0; i--) {
      sign = motionSign(trajPath[i][2] || 0);
    }
  }
  if (sign === 0) return trajPath.slice(nearest);

  let start = nearest;
  if (start > 0 && motionSign(trajPath[start - 1][2] || 0) === sign) start--;
  let end = nearest + 1;
  while (end < trajPath.length) {
    const nextSign = motionSign(trajPath[end][2] || 0);
    if (nextSign !== 0 && nextSign !== sign) break;
    end++;
  }
  return trajPath.slice(start, end);
}
