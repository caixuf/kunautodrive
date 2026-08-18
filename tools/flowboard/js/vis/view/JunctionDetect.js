/**
 * JunctionDetect.js — 从 road_network 检测交叉口中心
 *
 * 用途：让 RoadView 在接近交叉口处截断路面/标线，让 ConnectorView 在
 * 交叉口中心铺路口路面 + 斑马线，避免十字路口四条路 ribbon + 路缘石 +
 * 人行道 + 绿化带互相重叠的"毛边"。
 *
 * 数据源优先级（自底向上）：
 *   1. road_network.junctions[]（scene_pub 数据层预计算，单一事实源）
 *      → 直接消费，无需几何再猜一次
 *   2. 无数据层 junction 时（旧场景/直链路网）→ 端点几何聚类兜底
 *      （同一条 road 段的端点精确落在交叉口中心，按距离聚类即可得到）
 *
 * 坐标约定：与 sampleEdgeNodes 一致输出 THREE 坐标（x, y_up, z_north），
 * ENU→THREE 翻转统一走 Coord.worldToThree（门禁强制，禁止手写裸 -y）。
 */

import { worldToThree } from '../math/Coord.js';

const CLUSTER_RADIUS_M = 15;    // 端点聚簇半径：> 该距离视为不同交叉口
const EXTRA_URBAN_M = 3.6;      // 城市路额外截断量（路缘+人行道+余量）
const EXTRA_BASE_M = 1.0;       // 非城市路额外截断量
const REACH_MARGIN_M = 0.5;

/* 大地图的 junction 数量可达上万。路口匹配只需检查附近格子，不能让每个
 * endpoint 都扫描整个 centers 数组（42k roads × 12k junctions 会卡死主线程）。 */
function spatialGrid(centers, cellSize = CLUSTER_RADIUS_M) {
  const grid = new Map();
  const key = (x, z) => `${Math.floor(x / cellSize)},${Math.floor(z / cellSize)}`;
  for (let i = 0; i < centers.length; i++) {
    const c = centers[i];
    const k = key(c.x, c.z);
    let bucket = grid.get(k);
    if (!bucket) { bucket = []; grid.set(k, bucket); }
    bucket.push(i);
  }
  return { grid, key, cellSize };
}

function nearbyCenterIndices(index, x, z, radius) {
  const range = Math.ceil(radius / index.cellSize) + 1;
  const cx = Math.floor(x / index.cellSize);
  const cz = Math.floor(z / index.cellSize);
  const out = [];
  for (let dx = -range; dx <= range; dx++) {
    for (let dz = -range; dz <= range; dz++) {
      const bucket = index.grid.get(`${cx + dx},${cz + dz}`);
      if (bucket) out.push(...bucket);
    }
  }
  return out;
}

/** node → THREE 坐标（ENU→THREE 翻转统一走 worldToThree）。
 *  node 形如 [x_ENU, y_North, z_Up]（或 {x,y,z} 对象）。 */
function nodePoint(node) {
  if (!node) return null;
  if (Array.isArray(node)) {
    const [x, y, z] = worldToThree(node[0] || 0, node[1] || 0, node[2] || 0);
    return { x, yUp: y, z };
  }
  if (typeof node === 'object') {
    const [x, y, z] = worldToThree(node.x || 0, node.y || 0, node.z || 0);
    return { x, yUp: y, z };
  }
  return null;
}

function dist2(a, b) {
  return Math.hypot(a.x - b.x, a.z - b.z);
}

/* 高度层：立体交通里同一 XY 会有地面/高架/隧道多条路。端点聚簇必须带高度层，
 * 否则高架桥面与桥下地面路被当成同一路口 → 斑马线/停止线/铺装跨层重叠。
 * osm2kmap 写桥高 = 6m×layer，故按 6m 步长分桶；隧道固定 -4m。 */
const LEVEL_TOL_M = 0.5;
const LEVEL_STEP_M = 6.0;
function levelOf(py) {
  if (!Number.isFinite(py) || Math.abs(py) <= LEVEL_TOL_M) return 0;
  return Math.round(py / LEVEL_STEP_M);
}

/** 把 edge 的起点/终点匹配到最近的路口中心（<= CLUSTER_RADIUS_M），
 *  构建 byId: Map<edgeId, {start:中心索引|null, end:中心索引|null}>。 */
function matchEdgesToCenters(edges, centers) {
  const byId = new Map();
  const index = spatialGrid(centers);
  for (const edge of edges) {
    if (!edge || !Array.isArray(edge.nodes) || edge.nodes.length < 2) continue;
    const id = String(edge?.id ?? '');
    const nodes = edge.nodes.map((n) =>
      Array.isArray(n) ? n : [n.x || 0, n.y || 0, n.z || 0]);
    const start = nodePoint(nodes[0]);
    const end = nodePoint(nodes[nodes.length - 1]);
    let sIdx = null, eIdx = null, sBest = Infinity, eBest = Infinity;
    const sLv = start ? levelOf(start.yUp) : 0;
    const eLv = end ? levelOf(end.yUp) : 0;
    const candidates = new Set([
      ...nearbyCenterIndices(index, start?.x || 0, start?.z || 0, CLUSTER_RADIUS_M),
      ...nearbyCenterIndices(index, end?.x || 0, end?.z || 0, CLUSTER_RADIUS_M),
    ]);
    for (const i of candidates) {
      const c = centers[i];
      if (c.level != null && c.level !== sLv && c.level !== eLv) continue;   // 高度层不符
      const dS = start ? dist2(start, c) : Infinity;
      const dE = end ? dist2(end, c) : Infinity;
      if (dS < sBest) { sBest = dS; sIdx = i; }
      if (dE < eBest) { eBest = dE; eIdx = i; }
    }
    byId.set(id, {
      start: sIdx >= 0 && sBest <= CLUSTER_RADIUS_M ? sIdx : null,
      end: eIdx >= 0 && eBest <= CLUSTER_RADIUS_M ? eIdx : null,
    });
  }
  return byId;
}

/** 数据层路口（scene_pub junctions[]）：直接消费中心 + radius，不做几何聚类。
 *  每个 junction: {id, x, y, z, radius, n, roads:[road_id...]}（ENU 坐标）。 */
function junctionsFromData(roadNetwork, dataJunctions) {
  const centers = dataJunctions.map((j) => {
    let x = Number(j.x), y = Number(j.y), z = Number(j.z);
    /* osm2kmap 的 fork 契约携带 shape 而不是 x/y/z。优先用 polygon 质心，
     * 否则这些 junction 会错误地全部落在世界原点。 */
    if (![x, y, z].some(Number.isFinite) && Array.isArray(j.shape) && j.shape.length) {
      let sx = 0, sy = 0, sz = 0, count = 0;
      for (const p of j.shape) {
        if (!Array.isArray(p) || p.length < 2) continue;
        sx += Number(p[0]) || 0;
        sy += Number(p[1]) || 0;
        sz += Number(p[2]) || 0;
        count++;
      }
      if (count) { x = sx / count; y = sy / count; z = sz / count; }
    }
    const [tx, , tz] = worldToThree(Number.isFinite(x) ? x : 0,
      Number.isFinite(y) ? y : 0, Number.isFinite(z) ? z : 0);
    const py = Number.isFinite(z) ? z : 0;
    let radius = Number(j.radius);
    if (!Number.isFinite(radius) && Array.isArray(j.shape) && j.shape.length) {
      radius = 0;
      for (const p of j.shape) {
        if (Array.isArray(p) && p.length >= 2) {
          radius = Math.max(radius, Math.hypot((Number(p[0]) || 0) - x,
            (Number(p[1]) || 0) - y));
        }
      }
    }
    return { x: tx, z: tz, py, level: levelOf(py),
      radius: Number.isFinite(radius) ? Math.max(8, radius) : 8 };
  });
  const edges = Array.isArray(roadNetwork?.edges) ? roadNetwork.edges : [];
  const byId = matchEdgesToCenters(edges, centers);
  return { centers, byId };
}

/** 检测 road_network 中的所有交叉口中心。
 *  @returns {{centers:Array<{x,z,radius}>, byId:Map<string,{start:number|null,end:number|null}>}}
 *   centers 为交叉口中心列表（radius = 需截断半径，含路肩/人行道余量）；
 *   byId 把每个 edge.id 映射到其起点/终点所属的中心索引（无则 null）。 */
export function detectJunctions(roadNetwork) {
  // ── 优先数据层 junction（scene_pub 单一事实源）──
  // 但数据层必须带可用坐标（x/y/z）；否则 junctionsFromData 会把全部中心
  // 塌缩到原点（worldToThree(0,0)）→ 路口铺装/斑马线/停止线/转向导流全算到
  // 世界原点、路口聚类+裁剪彻底失效（本仓库 osm_lujiazui_v2 的 junctions[]
  // 只带 id/type/incoming_road/connecting_roads，无坐标）。此时几何端点聚类
  // 兜底（基于 edge 真实几何）反而正确，故回退。
  const dataJ = Array.isArray(roadNetwork?.junctions) ? roadNetwork.junctions : [];
  const dataHasCoords = dataJ.length &&
    dataJ.some((j) => j.x != null || j.y != null || j.z != null ||
      (Array.isArray(j.shape) && j.shape.length > 0));
  if (dataHasCoords) return junctionsFromData(roadNetwork, dataJ);

  // ── 兜底：端点几何聚类 ──
  const centers = [];
  const byId = new Map();
  const edges = Array.isArray(roadNetwork?.edges) ? roadNetwork.edges : [];
  if (!edges.length) return { centers, byId };

  // ── 第一遍：收集所有端点 + 每个端点的"截断半径"（道路半宽 + 城市扩展）──
  const endpoints = [];   // {x, z, edgeId, isStart, reach}
  for (const edge of edges) {
    const id = String(edge?.id ?? '');
    if (!edge || !Array.isArray(edge.nodes) || edge.nodes.length < 2) continue;
    const nodes = edge.nodes.map((n) =>
      Array.isArray(n) ? n : [n.x || 0, n.y || 0, n.z || 0]);
    const lanes = Number(edge.lanes) || 2;
    const laneWidth = Number(edge.lane_width) || 3.5;
    const halfWidth = (lanes * laneWidth) / 2;
    const isUrban = edge.type === 'urban' ||
      String(edge.name || '').toLowerCase().includes('urban');
    const reach = halfWidth + (isUrban ? EXTRA_URBAN_M : EXTRA_BASE_M) + REACH_MARGIN_M;

    const start = nodePoint(nodes[0]);
    const end = nodePoint(nodes[nodes.length - 1]);
    if (start) endpoints.push({ ...start, level: levelOf(start.yUp), edgeId: id, isStart: true, reach });
    if (end) endpoints.push({ ...end, level: levelOf(end.yUp), edgeId: id, isStart: false, reach });
  }

  // ── 第二遍：贪心聚类（遍历序即可，网格端点天然成簇）──
  const centerIndexByEndpoint = [];   // 每个 endpoint → center 索引
  const grid = spatialGrid(centers);
  for (const ep of endpoints) {
    let best = -1, bestDist = Infinity;
    const candidates = nearbyCenterIndices(grid, ep.x, ep.z, CLUSTER_RADIUS_M);
    for (const i of candidates) {
      const c = centers[i];
      if (c.level !== ep.level) continue;   // 高度层不符：高架/地面/隧道不混簇
      const d = dist2(ep, c);
      if (d < bestDist) { bestDist = d; best = i; }
    }
    if (best >= 0 && bestDist <= CLUSTER_RADIUS_M) {
      centerIndexByEndpoint.push(best);
      // 更新质心（轻量：取当前成员平均）
      const c = centers[best];
      c.n = (c.n || 0) + 1;
      c.x = (c.x * (c.n - 1) + ep.x) / c.n;
      c.z = (c.z * (c.n - 1) + ep.z) / c.n;
      c.py = (c.py * (c.n - 1) + (ep.yUp || 0)) / c.n;
      c.radius = Math.max(c.radius || 0, ep.reach);
    } else {
      const index = centers.length;
      centers.push({ x: ep.x, z: ep.z, py: ep.yUp || 0, level: ep.level, n: 1, radius: ep.reach });
      const k = grid.key(ep.x, ep.z);
      let bucket = grid.grid.get(k);
      if (!bucket) { bucket = []; grid.grid.set(k, bucket); }
      bucket.push(index);
      centerIndexByEndpoint.push(index);
    }
  }

  // ── 第三遍：构建 edge → {start,end} 中心索引映射（先按旧索引）──
  for (let k = 0; k < endpoints.length; k++) {
    const ep = endpoints[k];
    const entry = byId.get(ep.edgeId) || { start: null, end: null };
    if (ep.isStart) entry.start = centerIndexByEndpoint[k];
    else entry.end = centerIndexByEndpoint[k];
    byId.set(ep.edgeId, entry);
  }

  // ── 第四遍：只保留真正交叉口（>=3 个端点汇聚），并重建 byId 映射 ──
  // 2 个端点的簇是地图边界端点或两条路的直连，不是交叉口，不生成路口
  // 路面（否则地图边界会出现孤零零的路口方块）。
  const validIdx = new Map();   // 旧 center 索引 → 新索引
  const validCenters = [];
  for (let i = 0; i < centers.length; i++) {
    if ((centers[i].n || 0) >= 3) {
      validIdx.set(i, validCenters.length);
      validCenters.push({
        x: centers[i].x, z: centers[i].z,
        py: centers[i].py || 0, level: centers[i].level || 0,
        radius: centers[i].radius || 0,
      });
    }
  }
  for (const [edgeId, entry] of byId) {
    const mapped = { start: null, end: null };
    if (entry.start != null && validIdx.has(entry.start)) mapped.start = validIdx.get(entry.start);
    if (entry.end != null && validIdx.has(entry.end)) mapped.end = validIdx.get(entry.end);
    byId.set(edgeId, mapped);
  }

  return { centers: validCenters, byId };
}
