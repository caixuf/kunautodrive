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
 *     leftEdge: [[x,z],...],       // 路左缘（供点-多边形测试，T11）
 *     rightEdge:[[x,z],...],       // 路右缘
 *   }
 *
 * 约定：
 *   - road.centerline 为 ENU [[east, north, up], ...]（map.json 原始坐标）。
 *   - road.lanes 为 [{ centerline:[[e,n,u],...], width }]（map.json 原始坐标）。
 *   - 无 lanes 时 fromLanes=false：以 centerline 为居中、打 console.warn，绝不静默左偏。
 */

import { sampleEdgeNodes } from '../math/Curve.js';
import { tangentToNormal, offsetAlongNormal, signedAngleBetween, radiusFromChord } from '../math/Coord.js';

const LANE_WIDTH_DEFAULT = 3.5;
const _warnedRoadIds = new Set();

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
function buildCumulative(spine) {
  const cum = [0];
  for (let i = 1; i < spine.length; i++) {
    cum.push(cum[i - 1] + Math.hypot(spine[i].px - spine[i - 1].px, spine[i].pz - spine[i - 1].pz));
  }
  return cum;
}

/** 把 spine 整体横向偏移 d（沿各点法线）。含曲率钳制防止急弯自相交打结。 */
function offsetSpine(spine, d) {
  return spine.map((c, i) => {
    let off = d;
    if (i > 0 && i < spine.length - 1) {
      const a = spine[i - 1], b = spine[i + 1];
      const v1x = c.px - a.px, v1z = c.pz - a.pz;
      const v2x = b.px - c.px, v2z = b.pz - c.pz;
      const l1 = Math.hypot(v1x, v1z), l2 = Math.hypot(v2x, v2z);
      if (l1 > 1e-6 && l2 > 1e-6) {
        const ang = signedAngleBetween(v1x, v1z, v2x, v2z);
        if (Math.abs(ang) > 1e-4) {
          const chord = (l1 + l2) * 0.5;
          const R = radiusFromChord(chord, ang);
          const maxOff = Math.max(0.3, R - 0.3);
          off = Math.max(-maxOff, Math.min(d, maxOff));
        }
      }
    }
    const [opx, , opz] = offsetAlongNormal(c.px, c.pz, c.nx, c.nz, off);
    return { px: opx, py: c.py, pz: opz, nx: c.nx, nz: c.nz };
  });
}

/**
 * 车道组几何包络：按 lane centerline 实测车道组相对 spine 的横向范围，
 * 返回 { center, halfW }（相对 spine 的偏移与半宽）。无数据/小偏差/非平行路返回 null。
 * lane.centerline 是 ENU(east,north,0)，spine 是 THREE 帧，需先转：THREE.x=p[0], THREE.z=-(p[1])。
 * 横向偏移取"沿路纵向最近点"的垂距（垂直投影），非欧氏最近点（曲线路退化）。
 */
function laneGroupEnvelope(lanes, spine, laneWidth) {
  if (!Array.isArray(lanes) || !lanes.length || spine.length < 2) return null;
  const stations = [0, spine.length >> 1, spine.length - 1];
  const stationCenters = [];
  let halfW = 0;
  for (const si of stations) {
    const sp = spine[si];
    let latMin = Infinity, latMax = -Infinity;
    for (const lane of lanes) {
      const cl = lane && lane.centerline;
      if (!Array.isArray(cl) || cl.length < 2) continue;
      const w = (Number(lane.width) || laneWidth) / 2;
      let bestAlong = Infinity, bestLat = 0;
      for (const p of cl) {
        const lx = p[0] || 0, lz = -(p[1] || 0);
        const dx = lx - sp.px, dz = lz - sp.pz;
        const along = dx * sp.nz + dz * (-sp.nx);
        const lat = dx * sp.nx + dz * sp.nz;
        if (Math.abs(along) < bestAlong) { bestAlong = Math.abs(along); bestLat = lat; }
      }
      if (bestLat - w < latMin) latMin = bestLat - w;
      if (bestLat + w > latMax) latMax = bestLat + w;
    }
    if (!isFinite(latMin) || !isFinite(latMax)) continue;
    stationCenters.push((latMin + latMax) / 2);
    const sh = (latMax - latMin) / 2;
    if (sh > halfW) halfW = sh;
  }
  if (stationCenters.length === 0) return null;
  const cMin = Math.min(...stationCenters), cMax = Math.max(...stationCenters);
  const center = (cMin + cMax) / 2;
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
    return { ok: false, fromLanes: false, spine: [], cum: [], halfWidth: 0, leftEdge: [], rightEdge: [] };
  }

  // 1) 基础 spine（THREE 帧）。采样密度：多段 32，直道 24。
  const sampleCount = cl.length > 2 ? 32 : 24;
  const points = sampleEdgeNodes(cl, sampleCount);
  const spine = buildSpineFromPoints(points);
  if (spine.length < 2) {
    return { ok: false, fromLanes: false, spine: [], cum: [], halfWidth: 0, leftEdge: [], rightEdge: [] };
  }

  const laneWidth = Number(road.lane_width) || LANE_WIDTH_DEFAULT;
  /* 注意：laneCount 必须区分「空数组（无车道数据）」与「有车道」。computeEdgeAxis
   * 对无 lane_data 的 edge 传 lanes=[]，若用 Array.isArray(road.lanes)?len:... 会
   * 得到 0 → fallbackHw=0 → 家具按 halfWidth=0 落位直接压在路面上（实测 straight_
   * road S 弯 277/277 压路）。空数组按"未知车道数"处理，用 lanes_count 兜底。 */
  const laneCount = (Array.isArray(road.lanes) && road.lanes.length)
    ? road.lanes.length
    : (Number(road.lanes_count) || 2);
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

  // 2) 有车道数据 → 垂直投影 envelope 求 TRUE 中心
  const lanes = Array.isArray(road.lanes) ? road.lanes : [];
  if (lanes.length > 0) {
    const env = laneGroupEnvelope(lanes, spine, laneWidth);
    if (env) {
      const shifted = offsetSpine(spine, env.center);
      const cum = buildCumulative(shifted);
      const { left, right } = edgesFromSpine(shifted, env.halfW);
      return { ok: true, fromLanes: true, spine: applyElevation(shifted), cum, halfWidth: env.halfW, leftEdge: left, rightEdge: right };
    }
    // 非平行/环道/数据不足：居中（用 fallbackHw），fromLanes=false 但位置安全
    const cum = buildCumulative(spine);
    const { left, right } = edgesFromSpine(spine, fallbackHw);
    return { ok: true, fromLanes: false, spine: applyElevation(spine), cum, halfWidth: fallbackHw, leftEdge: left, rightEdge: right };
  }

  // 3) 无 lanes：以 centerline 为居中、警告（单次去重，避免每帧刷屏），绝不静默左偏
  const roadIdStr = String((road && road.id) || '0');
  if (!_warnedRoadIds.has(roadIdStr)) {
    _warnedRoadIds.add(roadIdStr);
    if (typeof console !== 'undefined' && console.warn) {
      console.warn('[RoadAxis] road 无 lanes，以 centerline 居中（不偏移、零回归）', road && road.id);
    }
  }
  const cum = buildCumulative(spine);
  const { left, right } = edgesFromSpine(spine, fallbackHw);
  return { ok: true, fromLanes: false, spine: applyElevation(spine), cum, halfWidth: fallbackHw, leftEdge: left, rightEdge: right };
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
    return { ok: false, fromLanes: false, spine: [], cum: [], halfWidth: 0, leftEdge: [], rightEdge: [] };
  }
  const nodes = edge.nodes;
  if (!Array.isArray(nodes) || nodes.length < 2) {
    return { ok: false, fromLanes: false, spine: [], cum: [], halfWidth: 0, leftEdge: [], rightEdge: [] };
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
