import { forwardENU } from './Coord.js';

function motionSign(v) {
  return v < -0.05 ? -1 : v > 0.05 ? 1 : 0;
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
