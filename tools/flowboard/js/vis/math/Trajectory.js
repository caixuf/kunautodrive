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
/* 轨迹起点沿行进方向相对自车中心的保险杠内侧偏移（m）：
 * SU7 车头约 +2.12m、车尾约 -2.05m；取 1.65m 使光带恰好从前保险杠（倒车时后保险杠）
 * 下缘无缝探出，既无车头断档，也不会漫到车身侧面或车屁股后方。 */
export const FRONT_BUMPER_ANCHOR_OFFSET = 1.65;

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

/**
 * 按自车实时位姿与当前挡位行进方向，裁剪掉已落到锚点后方的历史轨迹点，
 * 并将轨迹起点连续锚定在保险杠内侧（前进锚车头，倒车锚车尾）。
 *
 * 解决高速（如 74~120 km/h）下 60fps DeadReckon 超前 10Hz 规划快照 3~8m 时：
 *   1. 旧首点落到车尾后方且超出 5m 欧氏阈值 → 放弃锚定，光带直接拖在车屁股后；
 *   2. 仅替换 raw3d[0] 为车头而保留车底/车后的 raw3d[1..k] → CatmullRom 样条从车头
 *      向车尾倒折穿出后保险杠再折返，造成车屁股后双倍叠亮辉光。
 *
 * @param {Array<Array<number>>} segPath 单一挡位轨迹段 [[x, y, v], ...]
 * @param {{x:number, y:number, heading?:number, vx?:number, vy?:number}} ego
 * @param {number} [anchorOffset=FRONT_BUMPER_ANCHOR_OFFSET] 沿行进方向的锚点偏移（m）
 * @returns {Array<Array<number>>} 严格从锚点向前推进的 ENU 轨迹点 [[x, y, v], ...]
 */
export function clipTrajectoryFromAnchor(segPath, ego, anchorOffset = FRONT_BUMPER_ANCHOR_OFFSET) {
  if (!segPath || segPath.length < 2 || !ego) return [];

  const [fx, fy] = forwardENU(ego.heading || 0);
  let strokeSign = 0;
  for (let i = 0; i < segPath.length && strokeSign === 0; i++) {
    strokeSign = motionSign(segPath[i][2] || 0);
  }
  if (strokeSign === 0) {
    strokeSign = motionSign((ego.vx || 0) * fx + (ego.vy || 0) * fy);
  }
  const isReverse = strokeSign < 0;
  const ux = isReverse ? -fx : fx;
  const uy = isReverse ? -fy : fy;

  const off = Number.isFinite(anchorOffset) ? anchorOffset : FRONT_BUMPER_ANCHOR_OFFSET;
  const ax = (ego.x || 0) + ux * off;
  const ay = (ego.y || 0) + uy * off;

  const alongAt = (p) => (p[0] - ax) * ux + (p[1] - ay) * uy;

  // 仅裁剪前导已驶过段：找到首个跨过锚点法平面 (P - A)·u > 0 的点，
  // 跨越平面之后的 90° 转弯 / 180° 掉头弧段完整保留。
  let firstAhead = -1;
  for (let i = 0; i < segPath.length; i++) {
    if (alongAt(segPath[i]) > 0.02) {
      firstAhead = i;
      break;
    }
  }
  if (firstAhead < 0) return [];

  let crossX = segPath[firstAhead][0];
  let crossY = segPath[firstAhead][1];
  let crossV = segPath[firstAhead][2] || 0;

  if (firstAhead > 0) {
    const prev = segPath[firstAhead - 1];
    const next = segPath[firstAhead];
    const s0 = alongAt(prev);
    const s1 = alongAt(next);
    const denom = s1 - s0;
    const t = denom > 1e-6 ? Math.min(Math.max(-s0 / denom, 0), 1) : 1;
    crossX = prev[0] + (next[0] - prev[0]) * t;
    crossY = prev[1] + (next[1] - prev[1]) * t;
    crossV = (prev[2] || 0) + ((next[2] || 0) - (prev[2] || 0)) * t;
  }

  const dx = crossX - ax;
  const dy = crossY - ay;
  const dist2 = dx * dx + dy * dy;

  if (dist2 >= ANCHOR_MAX_DIST2) {
    // 轨迹横向/起始偏离走廊超过 5m（陈旧或不相关路径）：不强拉回车身，但仍从法平面交点起画
    const out = [];
    if (firstAhead > 0) out.push([crossX, crossY, crossV]);
    for (let i = firstAhead; i < segPath.length; i++) {
      const p = segPath[i];
      if (out.length > 0) {
        const sx = p[0] - out[out.length - 1][0];
        const sy = p[1] - out[out.length - 1][1];
        if (sx * sx + sy * sy < ANCHOR_MIN_SEG2) continue;
      }
      out.push([p[0], p[1], p[2] || 0]);
    }
    return out.length >= 2 ? out : [];
  }

  // 走廊内连续锚定到保险杠锚点 (ax, ay)，并随横向偏差动态留出前视过渡距离防变道折角
  const latDist = Math.abs(-dx * uy + dy * ux);
  const minLookahead = Math.max(0.35, Math.min(3.5, latDist * 1.6));

  const out = [[ax, ay, crossV]];
  let idx = firstAhead;
  while (idx < segPath.length - 1 && alongAt(segPath[idx]) < minLookahead) {
    idx++;
  }
  for (let i = idx; i < segPath.length; i++) {
    const p = segPath[i];
    const prev = out[out.length - 1];
    const sx = p[0] - prev[0];
    const sy = p[1] - prev[1];
    if (sx * sx + sy * sy < ANCHOR_MIN_SEG2) continue;
    out.push([p[0], p[1], p[2] || 0]);
  }
  return out.length >= 2 ? out : [];
}
