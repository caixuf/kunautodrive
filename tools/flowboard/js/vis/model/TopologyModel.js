/**
 * TopologyModel.js — 路网拓扑模型（3D 层单一事实源，P0 2026-08-14）
 *
 * 背景：路口拓扑此前被 5 个 view 各自计算或干脆不算——
 *   RoadView detectJunctions（标线裁剪）、ConnectorView detectJunctions（渠化）、
 *   TreeView detectJunctions（树避让）、StreetlightView 不算（路灯立路口中间）、
 *   BarrierView 不算（护栏路口穿插成圈，用户 2026-08-14 报障）。
 * 同一拓扑事实出现第二份计算 = 违规（模块职责铁律）。本模块收敛：
 *
 *   getTopology(roadNetwork) → {
 *     centers, byId,              // detectJunctions 原始输出（路口中心 + edge 映射）
 *     armsOfJunction(ci),         // 路口 arm 列表（折线点缓存 + 半宽 + walk 锚点）
 *     junctionsOfEdge(edgeId),    // edge 两端所属路口 [{ci, end}]
 *     segmentsOutsideJunctions,   // spine 按路口圆拆段（RoadView 标线/路肩用）
 *     pointInJunction,            // 点是否落在本 edge 任一路口圆内（护栏裁剪用）
 *     nearJunction,               // 点距任一路口中心 < 半径+margin（路灯/树槽位避让）
 *   }
 *
 * 缓存：按 roadNetworkHash 单条目记忆（SceneDirector 只在 hash 变化时重建 view），
 * 同一重建周期内 5 个 view 共享一次拓扑计算。
 *
 * 坐标：centers/byId 与 JunctionDetect 约定一致（THREE 坐标系）。
 */

import { detectJunctions } from '../view/JunctionDetect.js';
import { worldToThree } from '../math/Coord.js';
import { roadNetworkHash } from '../store/SceneStore.js';
import { LANE_WIDTH, DEFAULT_LANES } from '../core/Constants.js';

/** 沿 edge 折线从路口端向外走，停在「到路口中心径向距离 ≥ exitRadius」的
 *  第一处，返回 {x, z, ux, uz}（THREE 坐标，(ux,uz)=远离路口的局部切线）。
 *  曲路修正（2026-08-14）：旧实现用「路口端最后一小段」直射线外推位置+朝向，
 *  弯道/隧道引道（如延安东路隧道，160 节点急弯）端段方向与路口外真实路向
 *  差达 72°，斑马线斜切路面。径向出圈语义（而非固定弧长）同时覆盖聚类
 *  中心偏移导致的端点不在圆上的情形（否则锚点可能落在路口圆内部）。 */
export function walkFromJunction(pts, fromEnd, cx, cz, exitRadius) {
  const n = pts.length;
  const step = fromEnd ? -1 : 1;
  let i = fromEnd ? n - 1 : 0;
  let px = pts[i].x, pz = pts[i].z;
  const rad = (x, z) => Math.hypot(x - cx, z - cz);
  while (true) {
    const j = i + step;
    if (j < 0 || j >= n) break;
    const dx = pts[j].x - px, dz = pts[j].z - pz;
    const l = Math.hypot(dx, dz);
    if (l < 1e-9) { i = j; continue; }
    const ux = dx / l, uz = dz / l;
    if (rad(px, pz) >= exitRadius) return { x: px, z: pz, ux, uz };   // 已在圆外：端点即锚点
    if (rad(pts[j].x, pts[j].z) >= exitRadius) {
      // 本段内有一次径向上穿（起点<半径、终点≥半径，单极小值二次型 → 二分安全）
      let lo = 0, hi = 1;
      for (let k = 0; k < 24; k++) {
        const mid = (lo + hi) / 2;
        if (rad(px + dx * mid, pz + dz * mid) >= exitRadius) hi = mid; else lo = mid;
      }
      return { x: px + dx * hi, z: pz + dz * hi, ux, uz };
    }
    px = pts[j].x; pz = pts[j].z; i = j;
  }
  // 整条路都在圆内（短 stub 全在路口里）：停在远端点，方向用末段
  const a = fromEnd ? pts[1] : pts[n - 2], b = fromEnd ? pts[0] : pts[n - 1];
  const dx = b.x - a.x, dz = b.z - a.z;
  const l = Math.hypot(dx, dz) || 1;
  return { x: px, z: pz, ux: dx / l, uz: dz / l };
}

function _build(rn) {
  const { centers, byId } = detectJunctions(rn);
  const edges = Array.isArray(rn.edges) ? rn.edges : [];
  const edgeMap = new Map(edges.map((e) => [String(e.id), e]));

  // edge 折线点（THREE XZ）每 edge 只转一次，arms/裁剪共用
  const _ptsCache = new Map();
  function edgePts(edgeId) {
    const id = String(edgeId);
    if (_ptsCache.has(id)) return _ptsCache.get(id);
    const edge = edgeMap.get(id);
    let pts = null;
    if (edge && Array.isArray(edge.nodes) && edge.nodes.length >= 2) {
      pts = edge.nodes.map((n) => {
        const arr = Array.isArray(n) ? n : [n.x || 0, n.y || 0, n.z || 0];
        const [x, , z] = worldToThree(Number(arr[0]) || 0, Number(arr[1]) || 0, Number(arr[2]) || 0);
        return { x, z };
      });
    }
    _ptsCache.set(id, pts);
    return pts;
  }

  // edge → 两端路口 [{ci, end:'start'|'end'}]
  const _junctionsOfEdgeCache = new Map();
  function junctionsOfEdge(edgeId) {
    const id = String(edgeId);
    if (_junctionsOfEdgeCache.has(id)) return _junctionsOfEdgeCache.get(id);
    const entry = byId.get(id);
    const out = [];
    if (entry) {
      if (entry.start != null) out.push({ ci: entry.start, end: 'start' });
      if (entry.end != null && entry.end !== entry.start) out.push({ ci: entry.end, end: 'end' });
    }
    _junctionsOfEdgeCache.set(id, out);
    return out;
  }

  // 路口 arm 列表（每路口只算一次）：{edgeId, fromEnd, pts, hw, roadW, edge}
  const _armsCache = new Map();
  function armsOfJunction(ci) {
    if (_armsCache.has(ci)) return _armsCache.get(ci);
    const arms = [];
    for (const [edgeId, entry] of byId) {
      if (entry.start !== ci && entry.end !== ci) continue;
      const edge = edgeMap.get(edgeId);
      const pts = edgePts(edgeId);
      if (!edge || !pts) continue;
      const fromEnd = entry.end === ci;
      const laneW = Number(edge.lane_width) || LANE_WIDTH;
      const lanesN = Number(edge.lanes) || DEFAULT_LANES;
      arms.push({ edgeId, fromEnd, pts, hw: (lanesN * laneW) / 2, roadW: lanesN * laneW + 0.6, edge });
    }
    _armsCache.set(ci, arms);
    return arms;
  }

  // 本 edge 路口圆列表 [{x, z, radius}]
  function junctionCirclesOfEdge(edgeId) {
    return junctionsOfEdge(edgeId).map((j) => centers[j.ci]).filter(Boolean);
  }

  /** spine 按「到本 edge 路口中心 > radius」拆成若干连续段（路口内不画
   *  标线/侧向元素），返回段数组（每段 ≥2 点）。无路口 → [spine]。 */
  function segmentsOutsideJunctions(edgeId, spine) {
    const js = junctionCirclesOfEdge(edgeId);
    if (!js.length) return [spine];
    const segs = [];
    let cur = [];
    for (const c of spine) {
      const outside = js.every((j) => Math.hypot(c.px - j.x, c.pz - j.z) > (j.radius || 0));
      if (outside) {
        cur.push(c);
      } else if (cur.length >= 2) {
        segs.push(cur); cur = [];
      } else {
        cur = [];
      }
    }
    if (cur.length >= 2) segs.push(cur);
    return segs;
  }

  /** 点是否落在本 edge 的任一路口圆内（护栏/家具裁剪用） */
  function pointInJunction(edgeId, x, z, margin = 0) {
    for (const j of junctionCirclesOfEdge(edgeId)) {
      if (Math.hypot(x - j.x, z - j.z) <= (j.radius || 0) + margin) return true;
    }
    return false;
  }

  /** 点距**任何**路口中心 < 半径+margin（路灯/树槽位避让，不带 edge 语境） */
  function nearJunction(x, z, margin = 0) {
    return centers.some((c) => Math.hypot(x - c.x, z - c.z) < (c.radius || 0) + margin);
  }

  return {
    centers, byId,
    armsOfJunction, junctionsOfEdge,
    segmentsOutsideJunctions, pointInJunction, nearJunction,
    edgePts,
  };
}

let _cachedHash = null;
let _cachedTopo = null;

/** 取 roadNetwork 的拓扑模型（按 roadNetworkHash 单条目缓存）。 */
export function getTopology(roadNetwork) {
  if (!roadNetwork || !Array.isArray(roadNetwork.edges) || !roadNetwork.edges.length) return null;
  const h = roadNetworkHash(roadNetwork);
  if (h !== _cachedHash) {
    _cachedTopo = _build(roadNetwork);
    _cachedHash = h;
  }
  return _cachedTopo;
}

/** 测试用：强制失效缓存 */
export function _invalidateTopologyCache() {
  _cachedHash = null;
  _cachedTopo = null;
}
