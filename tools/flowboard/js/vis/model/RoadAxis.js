/**
 * RoadAxis.js — 共享路轴原语（路网渲染单一事实源）
 *
 * 背景（为何必须有这一层）：
 *   map.json 的 `road.centerline` 实际是**最左车道 LEFT 边**，不是路中心
 *   （实测 road_1290439822s3：centerline[0]=[639.12,577.01]，真中心≈[637.6,581.6]，
 *   偏 ~4.8m = 3×3.2/2）。历史上 RoadView / TreeView / StreetlightView 等 6 个视图各自
 *   从 edge.nodes(=左缘) 建 spine、按 halfWidth 偏移，解读不一致 → 路整体左偏、家具压路、
 *   各视图互相错位。这正是"架构乱了、多个东西互相干扰"的根因。
 *
 * 本模块把所有坐标转换(ENU→THREE)与"左缘→真中心"推导**集中到一处**，全视图共享，
 * 杜绝每视图各自假设 centerline 含义。
 *
 * 移植参考（见 docs / 方案 §1.5）：
 *   - OSM2World `RoadModule.Road`：centerline + width 一步扫出路面多边形（无左缘/中心分歧）。
 *   - OSM2World `EleConstraintEnforcer`：高程在 way 间传播（阶段2 竖向分层将复用）。
 *
 * API：
 *   computeRoadAxis(road) → {
 *     ok, fromLanes,
 *     spine: [{px,py,pz,nx,nz}],   // TRUE 中心 spine（THREE 帧：px=east, pz=-north）
 *     cum:   [number],             // spine 累计弧长
 *     halfWidth,                   // 车道组半宽
 *     centerOffset,                // 左缘 spine → 真中心的横向偏移（0=不挪）
 *     leftEdge: [[x,z],...],       // 路左缘（供点-多边形测试，T11）
 *     rightEdge:[[x,z],...],       // 路右缘
 *   }
 *
 * 约定：
 *   - road.centerline 为 ENU [[east, north, up], ...]（map.json 原始坐标）。
 *   - road.lanes 为 [{ centerline:[[e,n,u],...], width }]（map.json 原始坐标）。
 *   - 无 lanes 时 fromLanes=false：以 centerline 为居中、打 console.warn，绝不静默左偏。
 */

import { sampleEdgeNodes, edgeSampleCount } from '../math/Curve.js';
import { worldToThree, tangentToNormal, offsetAlongNormal, signedAngleBetween, radiusFromChord } from '../math/Coord.js';

const LANE_WIDTH_DEFAULT = 3.5;
const _warnedRoadIds = new Set();

/**
 * 抗过冲折线重采样：OSM 急转弯/掉头道/不等步长折线（相邻弦长比 > 3.2 或转角 > 31.5°）
 * 直接喂给 CatmullRom 会产生剧烈外甩环结。此类折线改用保端点限距 Chaikin 切角 + 等弧长
 * 线性插值（严格满足凸包变差缩减性质，数学上绝不外甩打结）；平缓曲线保持原样走 CatmullRom。
 */
export function sampleCleanPolyline(nodes, sampleCount) {
  if (!Array.isArray(nodes) || nodes.length < 2) return [];
  const raw = nodes.map((n) => (Array.isArray(n)
    ? [Number(n[0]) || 0, Number(n[1]) || 0, Number(n[2]) || 0]
    : [Number(n.x) || 0, Number(n.y) || 0, Number(n.z) || 0]));

  // 剔除 <0.25m 的近重合控制点（保留首尾），防止零弦长导数爆炸
  const clean = [raw[0]];
  for (let i = 1; i < raw.length - 1; i++) {
    const prev = clean[clean.length - 1];
    if (Math.hypot(raw[i][0] - prev[0], raw[i][1] - prev[1]) >= 0.25) {
      clean.push(raw[i]);
    }
  }
  const lastRaw = raw[raw.length - 1];
  if (clean.length === 1 || Math.hypot(lastRaw[0] - clean[clean.length - 1][0], lastRaw[1] - clean[clean.length - 1][1]) >= 1e-4) {
    clean.push(lastRaw);
  }
  if (clean.length === 1) clean.push([lastRaw[0] + 0.01, lastRaw[1], lastRaw[2]]);
  if (clean.length === 2) return sampleEdgeNodes(clean, sampleCount);

  let sharpOrUneven = false;
  for (let i = 1; i < clean.length - 1; i++) {
    const a = clean[i - 1], m = clean[i], b = clean[i + 1];
    const v1x = m[0] - a[0], v1y = m[1] - a[1];
    const v2x = b[0] - m[0], v2y = b[1] - m[1];
    const l1 = Math.hypot(v1x, v1y), l2 = Math.hypot(v2x, v2y);
    if (l1 < 1e-4 || l2 < 1e-4) { sharpOrUneven = true; break; }
    const ratio = l1 > l2 ? l1 / l2 : l2 / l1;
    const ang = Math.abs(signedAngleBetween(v1x, -v1y, v2x, -v2y));
    if (ang > 0.55 || ratio > 3.2) { sharpOrUneven = true; break; }
  }
  if (!sharpOrUneven) return sampleEdgeNodes(clean, sampleCount);

  // 2 轮保端点限距 Chaikin 圆角化（切角距离不超过 min(0.25*弦长, 3.5m)，长直边不塌缩）
  let poly = clean;
  for (let pass = 0; pass < 2; pass++) {
    const next = [poly[0]];
    for (let i = 1; i < poly.length - 1; i++) {
      const a = poly[i - 1], m = poly[i], b = poly[i + 1];
      const l1 = Math.hypot(m[0] - a[0], m[1] - a[1]);
      const l2 = Math.hypot(b[0] - m[0], b[1] - m[1]);
      const t1 = l1 > 1e-6 ? Math.min(0.25, 3.5 / l1) : 0.25;
      const t2 = l2 > 1e-6 ? Math.min(0.25, 3.5 / l2) : 0.25;
      next.push([
        m[0] + (a[0] - m[0]) * t1,
        m[1] + (a[1] - m[1]) * t1,
        m[2] + (a[2] - m[2]) * t1,
      ]);
      next.push([
        m[0] + (b[0] - m[0]) * t2,
        m[1] + (b[1] - m[1]) * t2,
        m[2] + (b[2] - m[2]) * t2,
      ]);
    }
    next.push(poly[poly.length - 1]);
    poly = next;
  }

  const cum = [0];
  for (let i = 1; i < poly.length; i++) {
    cum.push(cum[i - 1] + Math.hypot(poly[i][0] - poly[i - 1][0], poly[i][1] - poly[i - 1][1]));
  }
  const total = cum[cum.length - 1];
  const count = Math.max(2, sampleCount || 16);
  const out = [];
  let segIdx = 1;
  for (let k = 0; k < count; k++) {
    const s = (total * k) / (count - 1);
    while (segIdx < cum.length - 1 && cum[segIdx] < s) segIdx++;
    const segLen = cum[segIdx] - cum[segIdx - 1];
    const t = segLen > 1e-9 ? Math.max(0, Math.min(1, (s - cum[segIdx - 1]) / segLen)) : 0;
    const p0 = poly[segIdx - 1], p1 = poly[segIdx];
    const ex = p0[0] + (p1[0] - p0[0]) * t;
    const ey = p0[1] + (p1[1] - p0[1]) * t;
    const ez = p0[2] + (p1[2] - p0[2]) * t;
    const [tx, ty, tz] = worldToThree(ex, ey, ez);
    out.push(tx, ty, tz);
  }
  return out;
}

/* 一致性守卫（移植自 RoadView.laneGroupEnvelope）：
 * 各站位横向偏移应稳定；急弯/环道/掉头路 lane centerline 相对 spine 大幅摆动
 * （cMax-cMin 很大）→ 单一偏移 ribbon 无法表达，回退居中避免畸形。 */
const CONSISTENCY_MAX = 4.0;
/* center≈0 表示 centerline 本就是车道组中心 → 不偏移、零回归。 */
const CENTER_EPS = 0.15;

/** 由采样后的 flat points 构建 spine（THREE 帧：px=east, pz=-north）。 */
function buildSpineFromPoints(points) {
  const spine = [];
  for (let i = 0; i < points.length; i += 3) {
    const px = points[i], py = points[i + 1], pz = points[i + 2];
    // 注：向后差分必须用上一点的 z（索引 i-1），不是 i-2（那是上一点的 y/up）。
    // 两点直道采样时各点 y=0，若误用 i-2 会让 tz 取到 -pz_last 使切线竖向翻转、
    // 末站位法线翻转 → 横向投影符号反相 → 一致性守卫误杀 → 不偏移（路整体错位）。
    let tx = 1, tz = 0;
    if (i + 6 < points.length) { tx = points[i + 3] - px; tz = points[i + 5] - pz; }
    else if (i >= 3) { tx = px - points[i - 3]; tz = pz - points[i - 1]; }
    const [nx, nz] = tangentToNormal(tx, tz);
    spine.push({ px, py, pz, nx, nz });
  }
  return spine;
}

/** 构建 spine 累计弧长。 */
export function buildCumulative(spine) {
  const cum = [0];
  for (let i = 1; i < spine.length; i++) {
    cum.push(cum[i - 1] + Math.hypot(spine[i].px - spine[i - 1].px, spine[i].pz - spine[i - 1].pz));
  }
  return cum;
}

/** 把 spine 整体横向偏移 d（沿各点法线）。含曲率钳制与前向单调防折叠保护，杜绝急弯/掉头自相交打结。 */
export function offsetSpine(spine, d) {
  const n = spine.length;
  if (n === 0) return [];
  const raw = spine.map((c, i) => {
    let off = d;
    if (n >= 3) {
      const idx = i === 0 ? 1 : (i === n - 1 ? n - 2 : i);
      const a = spine[idx - 1], m = spine[idx], b = spine[idx + 1];
      const v1x = m.px - a.px, v1z = m.pz - a.pz;
      const v2x = b.px - m.px, v2z = b.pz - m.pz;
      const l1 = Math.hypot(v1x, v1z), l2 = Math.hypot(v2x, v2z);
      if (l1 > 1e-6 && l2 > 1e-6) {
        const ang = signedAngleBetween(v1x, v1z, v2x, v2z);
        if (Math.abs(ang) > 1e-4) {
          const chord = (l1 + l2) * 0.5;
          const R = radiusFromChord(chord, ang);
          const maxOff = Math.max(0.25, R * 0.85 - 0.25);
          off = Math.max(-maxOff, Math.min(d, maxOff));
        }
      }
    }
    const [opx, , opz] = offsetAlongNormal(c.px, c.pz, c.nx, c.nz, off);
    return { px: opx, py: c.py, pz: opz, nx: c.nx, nz: c.nz };
  });
  if (n <= 2) return raw;
  const out = [raw[0]];
  let lastSpineIdx = 0;
  for (let i = 1; i < n - 1; i++) {
    const prevOut = out[out.length - 1];
    const spPrev = spine[lastSpineIdx];
    const spCur = spine[i];
    const fdx = spCur.px - spPrev.px, fdz = spCur.pz - spPrev.pz;
    const odx = raw[i].px - prevOut.px, odz = raw[i].pz - prevOut.pz;
    if (odx * fdx + odz * fdz <= 0) continue; // 急弯内侧回折尖点直接剔除，防鱼尾环结
    out.push(raw[i]);
    lastSpineIdx = i;
  }
  out.push(raw[n - 1]);
  return out;
}

function projectSpinePointToLane(sp, cl) {
  let bestAlong = Infinity, bestLat = null;
  for (let i = 0; i < cl.length - 1; i++) {
    const ax = cl[i][0] || 0, az = -(cl[i][1] || 0);
    const bx = cl[i + 1][0] || 0, bz = -(cl[i + 1][1] || 0);
    const segX = bx - ax, segZ = bz - az;
    const len2 = segX * segX + segZ * segZ;
    if (len2 < 1e-6) continue;
    const t = ((sp.px - ax) * segX + (sp.pz - az) * segZ) / len2;
    const clampedT = Math.max(0, Math.min(1, t));
    const px = ax + clampedT * segX;
    const pz = az + clampedT * segZ;
    const dx = px - sp.px, dz = pz - sp.pz;
    const along = Math.abs(dx * sp.nz - dz * sp.nx);
    const lat = dx * sp.nx + dz * sp.nz;
    if (along < bestAlong) {
      bestAlong = along;
      bestLat = lat;
    }
  }
  return { along: bestAlong, lat: bestLat };
}

/**
 * 车道组几何包络：按 lane centerline 实测车道组相对 spine 的横向范围，
 * 返回 { center, halfW }（相对 spine 的偏移与半宽）。无数据/小偏差/非平行路返回 null。
 * lane.centerline 是 ENU(east,north,0)，spine 是 THREE 帧，需先转：THREE.x=p[0], THREE.z=-(p[1])。
 * 横向偏移取线段垂直投影（连续线段插值），通过 LANE_OVERRUN_MAX 门限过滤路口停止线截断引入的
 * 纵向差（>7.5m）：典型截断场景 road cl 比 lane cl 长 11m；路口内部短路（≤10m 总长）最大 along≤3m。
 */
/* 路口停止线截断最大允许纵向溢出（m）：11m 截断会被过滤，≤7m 路口内部路不受影响。 */
const LANE_OVERRUN_MAX = 7.5;
function laneGroupEnvelope(lanes, spine, laneWidth) {
  if (!Array.isArray(lanes) || !lanes.length || spine.length < 2) return null;
  const stations = [0, spine.length >> 1, spine.length - 1];
  const stationCenters = [];
  let halfW = 0;
  for (const si of stations) {
    const sp = spine[si];
    let latMin = Infinity, latMax = -Infinity;
    let stationOk = true;
    for (const lane of lanes) {
      const cl = lane && lane.centerline;
      if (!Array.isArray(cl) || cl.length < 2) continue;
      const w = (Number(lane.width) || laneWidth) / 2;
      const proj = projectSpinePointToLane(sp, cl);
      // 路口停止线截断守卫：若沿路纵向溢出 > LANE_OVERRUN_MAX，
      // 说明该站位超出车道数据范围（road cl 延伸入路口中心而 lane cl 在停止线结束），
      // 丢弃本站位，防止 11m 纵向差泄漏进横向投影导致路面整体横向漂移。
      if (proj.along > LANE_OVERRUN_MAX || proj.lat === null) {
        stationOk = false;
        break;
      }
      if (proj.lat - w < latMin) latMin = proj.lat - w;
      if (proj.lat + w > latMax) latMax = proj.lat + w;
    }
    if (stationOk && isFinite(latMin) && isFinite(latMax)) {
      stationCenters.push((latMin + latMax) / 2);
      const sh = (latMax - latMin) / 2;
      if (sh > halfW) halfW = sh;
    }
  }

  // 极端边缘情况回退：若全部站位（包括中点）被截断守卫过滤，以中点无条件投影兜底
  if (stationCenters.length === 0) {
    const midSp = spine[spine.length >> 1];
    let latMin = Infinity, latMax = -Infinity;
    for (const lane of lanes) {
      const cl = lane && lane.centerline;
      if (!Array.isArray(cl) || cl.length < 2) continue;
      const w = (Number(lane.width) || laneWidth) / 2;
      const proj = projectSpinePointToLane(midSp, cl);
      if (proj.lat !== null) {
        if (proj.lat - w < latMin) latMin = proj.lat - w;
        if (proj.lat + w > latMax) latMax = proj.lat + w;
      }
    }
    if (isFinite(latMin) && isFinite(latMax)) {
      stationCenters.push((latMin + latMax) / 2);
      halfW = (latMax - latMin) / 2;
    }
  }

  if (stationCenters.length === 0) return null;
  stationCenters.sort((a, b) => a - b);
  const mid = stationCenters.length >> 1;
  const center = stationCenters.length % 2 === 1
    ? stationCenters[mid]
    : (stationCenters[mid - 1] + stationCenters[mid]) / 2;
  const cMin = stationCenters[0], cMax = stationCenters[stationCenters.length - 1];
  if (cMax - cMin > CONSISTENCY_MAX) return null;   // 非平行/环道 → 回退居中
  if (Math.abs(center) < CENTER_EPS) {
    /* centerline 本就是车道组中心 → 不偏移（offset=0），但务必保留 envelope 算出的
     * 精确半宽 halfW，而非回退 laneCount*laneWidth/2（仅当车道严格按 lane_width 紧贴
     * 排布时才相等；存在中央分隔带/车道宽不一致时回退值会偏小，使家具/路缘落进沥青）。
     * 典型触发：net2map 已把 road.centerline 对齐到车道组中心（如 osm_lujiazui_v2 的
     * 2664 条路），此处分支避免回退、直接用真实半宽。offset=0 保证路网位置零回归。 */
    return { center: 0, halfW };
  }
  return { center, halfW };
}

/** 由 spine + halfWidth 生成左右缘（THREE 帧 x,z），供点-多边形测试。 */
function edgesFromSpine(spine, halfWidth) {
  const left = [], right = [];
  for (const c of spine) {
    const [lx, , lz] = offsetAlongNormal(c.px, c.pz, c.nx, c.nz, -halfWidth);
    const [rx, , rz] = offsetAlongNormal(c.px, c.pz, c.nx, c.nz, halfWidth);
    left.push([lx, lz]);
    right.push([rx, rz]);
  }
  return { left, right };
}

/**
 * 计算一条路的共享路轴。
 * @param {object} road { centerline:[[e,n,u]...], lanes:[{centerline,width}], lane_width, id? }
 * @returns 见文件头 API 说明。
 */
export function computeRoadAxis(road) {
  const cl = (road && road.centerline) || [];
  if (!Array.isArray(cl) || cl.length < 2) {
    return { ok: false, fromLanes: false, spine: [], cum: [], halfWidth: 0, centerOffset: 0, leftEdge: [], rightEdge: [] };
  }

  // 1) 基础 spine（THREE 帧）。多点曲线按弧长自适应采样并走抗过冲采样，
  // 两点直道无需按 4m 过密切分（polygonOffset 已根除地面穿模，限制在 ≤32 站降低大图碰撞网格开销）。
  let sampleCount = 24;
  if (cl.length > 2) {
    sampleCount = edgeSampleCount(cl);
  } else {
    const chord = Math.hypot((cl[1][0] || 0) - (cl[0][0] || 0), (cl[1][1] || 0) - (cl[0][1] || 0));
    sampleCount = Math.min(64, Math.max(24, Math.ceil(chord / 18)));
  }
  const points = sampleCleanPolyline(cl, sampleCount);
  const spine = buildSpineFromPoints(points);
  if (spine.length < 2) {
    return { ok: false, fromLanes: false, spine: [], cum: [], halfWidth: 0, centerOffset: 0, leftEdge: [], rightEdge: [] };
  }

  const laneWidth = Number(road.lane_width) || LANE_WIDTH_DEFAULT;
  /* 注意：laneCount 必须区分「空数组（无车道数据）」与「有车道」。computeEdgeAxis
   * 对无 lane_data 的 edge 传 lanes=[]，若用 Array.isArray(road.lanes)?len:... 会
   * 得到 0 → fallbackHw=0 → 家具按 halfWidth=0 落位直接压在路面上（实测 straight_
   * road S 弯 277/277 压路）。空数组按"未知车道数"处理，用 lanes_count 兜底。 */
  const laneCount = (Array.isArray(road.lanes) && road.lanes.length)
    ? road.lanes.length
    : (typeof road.lanes === 'number' && road.lanes > 0
      ? road.lanes
      : (Number(road.lanes_count) || 2));
  const fallbackHw = laneCount * laneWidth / 2;

  /* 阶段2 竖向分层（安全消费，零回归）：若 road 携带逐路 elevation / level
   * （OSM layer / bridge / tunnel，由 DSL 枢纽 net2map 抽取，方案 §阶段2），
   * 把 spine 各点 py 抬到该真实高程，重叠路按高程自然分离，不靠 y 微偏移硬扛。
   * 当前 osm_lujiazui_v2 等地图暂无该字段 → elev=null → spine py 保持原始
   * up（=0），行为不变；字段落地后本处自动点亮分层，无需回改 RoadAxis。 */
  const elevRaw = road.elevation != null ? Number(road.elevation)
    : (road.level != null ? Number(road.level) : null);
  const elev = (elevRaw != null && isFinite(elevRaw)) ? elevRaw : null;
  const applyElevation = (s) => {
    if (elev !== null) for (const c of s) c.py = elev;
    return s;
  };

  // 2) 有车道数据或可由车道数与中心线合成虚拟车道 → 垂直投影 envelope 求 TRUE 中心
  let lanes = Array.isArray(road.lanes) ? road.lanes : [];
  if (lanes.length === 0 && laneCount > 0 && cl.length >= 2) {
    lanes = [];
    for (let k = 0; k < laneCount; k++) {
      const d = (k - (laneCount - 1) / 2.0) * laneWidth;
      const laneCl = [];
      for (let i = 0; i < cl.length; i++) {
        let tx = 1, ty = 0;
        if (i + 1 < cl.length) {
          tx = cl[i + 1][0] - cl[i][0];
          ty = cl[i + 1][1] - cl[i][1];
        } else if (i > 0) {
          tx = cl[i][0] - cl[i - 1][0];
          ty = cl[i][1] - cl[i - 1][1];
        }
        const len = Math.hypot(tx, ty);
        const nx = len > 1e-6 ? -ty / len : 0;
        const ny = len > 1e-6 ? tx / len : 1;
        laneCl.push([
          cl[i][0] + nx * d,
          cl[i][1] + ny * d,
          cl[i][2] || 0,
        ]);
      }
      lanes.push({ centerline: laneCl, width: laneWidth });
    }
  }

  if (lanes.length > 0) {
    const env = laneGroupEnvelope(lanes, spine, laneWidth);
    if (env) {
      const shifted = offsetSpine(spine, env.center);
      const cum = buildCumulative(shifted);
      const { left, right } = edgesFromSpine(shifted, env.halfW);
      return { ok: true, fromLanes: true, spine: applyElevation(shifted), cum, halfWidth: env.halfW, centerOffset: env.center, leftEdge: left, rightEdge: right };
    }
    // 非平行/环道/数据不足：居中（用 fallbackHw），fromLanes=false 但位置安全
    const cum = buildCumulative(spine);
    const { left, right } = edgesFromSpine(spine, fallbackHw);
    return { ok: true, fromLanes: false, spine: applyElevation(spine), cum, halfWidth: fallbackHw, centerOffset: 0, leftEdge: left, rightEdge: right };
  }

  // 3) 无 lanes：以 centerline 为居中、单次去重记录，避免每帧刷屏
  const roadIdStr = String((road && road.id) || '0');
  if (!_warnedRoadIds.has(roadIdStr)) {
    _warnedRoadIds.add(roadIdStr);
    if (typeof console !== 'undefined' && console.warn) {
      console.warn('[RoadAxis] road 无 lanes，以 centerline 居中（不偏移、零回归）', road && road.id);
    }
  }
  const cum = buildCumulative(spine);
  const { left, right } = edgesFromSpine(spine, fallbackHw);
  return { ok: true, fromLanes: false, spine: applyElevation(spine), cum, halfWidth: fallbackHw, centerOffset: 0, leftEdge: left, rightEdge: right };
}

/**
 * 由 roadNetwork 的一条 edge（含 lane_data）计算共享路轴——家具/护栏/路灯视图的
 * 便捷入口。集中处理：① edge.nodes 归一（兼容 {x,y,z} 对象格式）；
 * ② lane_data 按 edge.name / edge.id 取车道组；③ 调用 computeRoadAxis。
 * edge.nodes 为 ENU 折线（MapData.buildIndex 产出 [east,north,up] 数组格式）。
 * laneData 为 { roadId: lanes[] }（MapData 注入到 rn.lane_data）。
 * 无 nodes / laneData 缺失时安全回退（fromLanes=false、居中、零回归）。
 */
export function computeEdgeAxis(edge, laneData) {
  if (!edge) {
    return { ok: false, fromLanes: false, spine: [], cum: [], halfWidth: 0, centerOffset: 0, leftEdge: [], rightEdge: [] };
  }
  const nodes = edge.nodes;
  if (!Array.isArray(nodes) || nodes.length < 2) {
    return { ok: false, fromLanes: false, spine: [], cum: [], halfWidth: 0, centerOffset: 0, leftEdge: [], rightEdge: [] };
  }
  let centerline = nodes;
  if (nodes[0] && typeof nodes[0] === 'object' && !Array.isArray(nodes[0])) {
    centerline = nodes.map(n => [n.x || 0, n.y || 0, n.z || 0]);
  }
  const key = edge.name != null ? String(edge.name) : String(edge.id);
  const lanes = (laneData && (laneData[key] || laneData[String(edge.id)])) || [];
  return computeRoadAxis({
    centerline,
    lanes,
    lane_width: edge.lane_width,
    lanes_count: edge.lanes,
    id: key,
  });
}

export default computeRoadAxis;
