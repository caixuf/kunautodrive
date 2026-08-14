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

/** 把 edge 的起点/终点匹配到最近的路口中心（<= CLUSTER_RADIUS_M），
 *  构建 byId: Map<edgeId, {start:中心索引|null, end:中心索引|null}>。 */
function matchEdgesToCenters(edges, centers) {
  const byId = new Map();
  for (const edge of edges) {
    if (!edge || !Array.isArray(edge.nodes) || edge.nodes.length < 2) continue;
    const id = String(edge?.id ?? '');
    const nodes = edge.nodes.map((n) =>
      Array.isArray(n) ? n : [n.x || 0, n.y || 0, n.z || 0]);
    const start = nodePoint(nodes[0]);
    const end = nodePoint(nodes[nodes.length - 1]);
    let sIdx = null, eIdx = null, sBest = Infinity, eBest = Infinity;
    for (let i = 0; i < centers.length; i++) {
      const dS = start ? dist2(start, centers[i]) : Infinity;
      const dE = end ? dist2(end, centers[i]) : Infinity;
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
    const [x, yUp, z] = worldToThree(Number(j.x) || 0, Number(j.y) || 0, Number(j.z) || 0);
    return { x, z, radius: Number(j.radius) || 8 };
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
  const dataJ = Array.isArray(roadNetwork?.junctions) ? roadNetwork.junctions : [];
  if (dataJ.length) return junctionsFromData(roadNetwork, dataJ);

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
    if (start) endpoints.push({ ...start, edgeId: id, isStart: true, reach });
    if (end) endpoints.push({ ...end, edgeId: id, isStart: false, reach });
  }

  // ── 第二遍：贪心聚类（遍历序即可，网格端点天然成簇）──
  const centerIndexByEndpoint = [];   // 每个 endpoint → center 索引
  for (const ep of endpoints) {
    let best = -1, bestDist = Infinity;
    for (let i = 0; i < centers.length; i++) {
      const d = dist2(ep, centers[i]);
      if (d < bestDist) { bestDist = d; best = i; }
    }
    if (best >= 0 && bestDist <= CLUSTER_RADIUS_M) {
      centerIndexByEndpoint.push(best);
      // 更新质心（轻量：取当前成员平均）
      const c = centers[best];
      c.n = (c.n || 0) + 1;
      c.x = (c.x * (c.n - 1) + ep.x) / c.n;
      c.z = (c.z * (c.n - 1) + ep.z) / c.n;
      c.radius = Math.max(c.radius || 0, ep.reach);
    } else {
      centers.push({ x: ep.x, z: ep.z, n: 1, radius: ep.reach });
      centerIndexByEndpoint.push(centers.length - 1);
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
      validCenters.push({ x: centers[i].x, z: centers[i].z, radius: centers[i].radius || 0 });
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
