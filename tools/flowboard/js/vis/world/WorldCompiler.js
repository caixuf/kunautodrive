/**
 * WorldCompiler.js — 前端离线确定性世界编译器（C++ flowsim::WorldEngine 的客户端预览/兜底镜像）
 *
 * 定位：
 *   1. 在线仿真模式：后端 flowsim_node 的 C++ WorldEngine 为唯一权威事实源（physics/sensor/visual 同源），
 *      前端通过 WorldLayoutClient 拉取 `/api/world/layout` 直接消费。
 *   2. 离线地图预览（/map_preview）与无后端单测模式下，本模块以相同的 `world_gen` 声明式配置
 *      确定性编译路网周边设施、交通标志、街道家具、天际线建筑与自定义精修道具，确保预览与运行期体验一致。
 *
 * 硬约束：
 *   - 严格纯确定性（禁止 Math.random / Date.now），同输入保证相同 world_hash。
 *   - 所有坐标映射与朝向转换统一走 math/Coord.js 与 model/RoadAxis.js / TopologyModel.js。
 */

import {
  worldToThree,
  threeToWorld,
  forwardENU,
  headingBetweenPoints,
  distanceENU,
} from '../math/Coord.js';
import { EDGE_TYPE, isGarageOrRampEdge, isTunnelEdge, LANE_WIDTH, DEFAULT_LANES } from '../core/Constants.js';
import { computeEdgeAxis } from '../model/RoadAxis.js';
import { getTopology } from '../model/TopologyModel.js';
import { roadNetworkHash } from '../store/SceneStore.js';

const PRESET_PROFILES = {
  urban_arterial: {
    poleSpacing: 40,
    treeSpacing: 18,
    barrierSpacing: 3.0,
    signInterval: 180,
    furnitureSpacing: 95,
    buildingDensity: 1.0,
    buildingHeightScale: 1.0,
    treeScale: 1.0,
  },
  modern_cbd: {
    poleSpacing: 36,
    treeSpacing: 22,
    barrierSpacing: 3.0,
    signInterval: 160,
    furnitureSpacing: 80,
    buildingDensity: 1.25,
    buildingHeightScale: 1.45,
    treeScale: 0.95,
  },
  suburban_park: {
    poleSpacing: 48,
    treeSpacing: 14,
    barrierSpacing: 4.0,
    signInterval: 220,
    furnitureSpacing: 120,
    buildingDensity: 0.45,
    buildingHeightScale: 0.65,
    treeScale: 1.15,
  },
  expressway: {
    poleSpacing: 45,
    treeSpacing: 0,
    barrierSpacing: 3.0,
    signInterval: 200,
    furnitureSpacing: 0,
    buildingDensity: 0.35,
    buildingHeightScale: 0.85,
    treeScale: 0.9,
  },
  industrial_zone: {
    poleSpacing: 42,
    treeSpacing: 32,
    barrierSpacing: 3.0,
    signInterval: 180,
    furnitureSpacing: 140,
    buildingDensity: 0.65,
    buildingHeightScale: 0.55,
    treeScale: 0.85,
  },
};

function _fnv1aStep(h, val) {
  let x = (h ^ (val >>> 0)) >>> 0;
  x = Math.imul(x, 16777619) >>> 0;
  return x;
}

function _fnv1aStr(str, init = 2166136261) {
  let h = init >>> 0;
  for (let i = 0; i < str.length; i++) {
    h = _fnv1aStep(h, str.charCodeAt(i));
  }
  return h >>> 0;
}

function _detRand(seed, tag, idx) {
  let h = _fnv1aStr(String(tag || ''), (seed >>> 0) ^ 2654435769);
  h = _fnv1aStep(h, idx | 0);
  h ^= h >>> 16;
  h = Math.imul(h, 2246822507) >>> 0;
  h ^= h >>> 13;
  return (h >>> 0) / 4294967296;
}

function _parseWorldGen(roadNetwork, rawWorldGen, seedOverride) {
  let wg = rawWorldGen;
  if (!wg && roadNetwork && roadNetwork.world_gen) wg = roadNetwork.world_gen;
  if (typeof wg === 'string') {
    try { wg = JSON.parse(wg); } catch { wg = null; }
  }
  if (wg && wg.world_gen && typeof wg.world_gen === 'object') {
    wg = wg.world_gen;
  }
  const obj = (wg && typeof wg === 'object') ? wg : {};
  const preset = PRESET_PROFILES[obj.preset] ? obj.preset : 'urban_arterial';
  const seed = Number.isFinite(Number(seedOverride))
    ? (Number(seedOverride) >>> 0)
    : (Number.isFinite(Number(obj.seed)) ? (Number(obj.seed) >>> 0) : 20260401);
  const excludeZones = Array.isArray(obj.exclude_zones) ? obj.exclude_zones : [];
  const edgeOverrides = (obj.edge_overrides && typeof obj.edge_overrides === 'object') ? obj.edge_overrides : {};
  const customProps = Array.isArray(obj.custom_props) ? obj.custom_props : [];
  return { preset, seed, excludeZones, edgeOverrides, customProps };
}

function _inExcludeZone(x, y, zone, kind) {
  if (!zone) return false;
  if (Array.isArray(zone.kinds) && zone.kinds.length > 0 && !zone.kinds.includes(kind)) {
    return false;
  }
  if (Number.isFinite(zone.radius) && zone.radius > 0) {
    return distanceENU(x, y, Number(zone.x) || 0, Number(zone.y) || 0) <= Number(zone.radius);
  }
  const hx = Math.max(0.5, (Number(zone.width) || 10) * 0.5);
  const hy = Math.max(0.5, (Number(zone.depth) || Number(zone.length) || 10) * 0.5);
  return Math.abs(x - (Number(zone.x) || 0)) <= hx && Math.abs(y - (Number(zone.y) || 0)) <= hy;
}

function _isExcluded(x, y, zones, kind) {
  for (const z of zones) {
    if (_inExcludeZone(x, y, z, kind)) return true;
  }
  return false;
}

function _getEdgeOverride(edge, edgeOverrides) {
  if (!edge || !edgeOverrides) return null;
  if (edge.name && edgeOverrides[edge.name]) return edgeOverrides[edge.name];
  if (edge.id != null && edgeOverrides[String(edge.id)]) return edgeOverrides[String(edge.id)];
  return null;
}

function _sampleSpineAtArc(axis, targetS) {
  const { spine, cum } = axis;
  if (!spine || spine.length === 0) return null;
  if (spine.length === 1 || targetS <= 0) {
    const p0 = spine[0];
    const p1 = spine[Math.min(1, spine.length - 1)];
    const [ex, ey, ez] = threeToWorld(p0.px, p0.py, p0.pz);
    const [ex1, ey1] = threeToWorld(p1.px, p1.py, p1.pz);
    const heading = spine.length > 1 ? headingBetweenPoints(ex, ey, ex1, ey1) : 0;
    return { x: ex, y: ey, z: ez, heading, rxThree: p0.nx, rzThree: p0.nz };
  }
  const total = cum[cum.length - 1] || 0;
  const s = Math.min(targetS, total);
  let idx = 0;
  while (idx < cum.length - 2 && cum[idx + 1] < s) idx++;
  const s0 = cum[idx];
  const s1 = cum[idx + 1];
  const t = (s1 - s0) > 1e-6 ? Math.max(0, Math.min(1, (s - s0) / (s1 - s0))) : 0;
  const a = spine[idx];
  const b = spine[idx + 1];
  const px = a.px + (b.px - a.px) * t;
  const py = a.py + (b.py - a.py) * t;
  const pz = a.pz + (b.pz - a.pz) * t;
  const [ex, ey, ez] = threeToWorld(px, py, pz);
  const [eax, eay] = threeToWorld(a.px, a.py, a.pz);
  const [ebx, eby] = threeToWorld(b.px, b.py, b.pz);
  const heading = headingBetweenPoints(eax, eay, ebx, eby);
  return { x: ex, y: ey, z: ez, heading, rxThree: a.nx, rzThree: a.nz };
}

function _pointToSegDistSq(px, py, ax, ay, bx, by) {
  const dx = bx - ax;
  const dy = by - ay;
  const l2 = dx * dx + dy * dy;
  if (l2 <= 1e-8) {
    const ex = px - ax;
    const ey = py - ay;
    return ex * ex + ey * ey;
  }
  const t = Math.max(0, Math.min(1, ((px - ax) * dx + (py - ay) * dy) / l2));
  const cx = ax + dx * t;
  const cy = ay + dy * t;
  const ex = px - cx;
  const ey = py - cy;
  return ex * ex + ey * ey;
}

function _buildingClearsRoads(cx, cy, w, d, heading, height, compiledEdges, sidewalkMargin = 3.2) {
  const podiumMul = height > 28 ? 1.35 : 1.0;
  const hwAlong = 0.5 * w * podiumMul;
  const hdAcross = 0.5 * d * podiumMul;
  const [ch, sh] = forwardENU(heading);
  const pts = [
    [0, 0],
    [hwAlong, hdAcross], [hwAlong, -hdAcross],
    [-hwAlong, hdAcross], [-hwAlong, -hdAcross],
    [hwAlong, 0], [-hwAlong, 0],
    [0, hdAcross], [0, -hdAcross],
  ];
  for (const [lx, ly] of pts) {
    const px = cx + lx * ch - ly * sh;
    const py = cy + lx * sh + ly * ch;
    if (_pointNearAnyRoadBand(px, py, 0.0, compiledEdges, sidewalkMargin)) {
      return false;
    }
  }
  return true;
}

function _pointNearAnyRoadBand(x, y, z, compiledEdges, margin) {
  for (const ce of compiledEdges) {
    const isCutout = isGarageOrRampEdge(ce.edge);
    const cutoutExtra = isCutout ? (ce.edge.type === EDGE_TYPE.GARAGE_HALL ? 12.5 : 3.0) : 0.0;
    const hw = (ce.halfWidth || 3.5) + margin + cutoutExtra;
    const hwSq = hw * hw;
    const enuPts = ce.enuSpine;
    if (!enuPts || enuPts.length === 0) continue;
    if (x < ce.minX - hw || x > ce.maxX + hw || y < ce.minY - hw || y > ce.maxY + hw) continue;
    if (enuPts.length === 1) {
      const p = enuPts[0];
      if (!isCutout && Math.abs(p[2] - z) > 3.5) continue;
      if (distanceENU(x, y, p[0], p[1]) < hw) return true;
      continue;
    }
    for (let i = 0; i < enuPts.length - 1; i++) {
      const a = enuPts[i];
      const b = enuPts[i + 1];
      const segZ = (a[2] + b[2]) * 0.5;
      if (!isCutout && Math.abs(segZ - z) > 3.5) continue;
      if (_pointToSegDistSq(x, y, a[0], a[1], b[0], b[1]) < hwSq) return true;
    }
  }
  return false;
}

function _nearestRoadZ(x, y, compiledEdges, fallbackZ = 0) {
  let bestD = 30.0;
  let bestZ = fallbackZ;
  for (const ce of compiledEdges) {
    for (const p of ce.axis.spine) {
      const [ex, ey, ez] = threeToWorld(p.px, p.py, p.pz);
      const d = distanceENU(x, y, ex, ey);
      if (d < bestD) {
        bestD = d;
        bestZ = ez;
      }
    }
  }
  return bestZ;
}

/**
 * 编译 roadNetwork + worldGen 为完整确定性 worldLayout 对象（与 C++ serialize_world_layout_json 同构）。
 * @param {object} roadNetwork
 * @param {object|string|null} [worldGen]
 * @param {number|null} [seedOverride]
 */
export function compileWorld(roadNetwork, worldGen = null, seedOverride = null) {
  const edges = (roadNetwork && Array.isArray(roadNetwork.edges)) ? roadNetwork.edges : [];
  const cfg = _parseWorldGen(roadNetwork, worldGen, seedOverride);
  const profile = PRESET_PROFILES[cfg.preset] || PRESET_PROFILES.urban_arterial;
  const topo = getTopology(roadNetwork);
  const nearJunctionXZ = (tx, tz) => topo && typeof topo.nearJunction === 'function' && topo.nearJunction(tx, tz);

  const poles = [];
  const trees = [];
  const barrierPosts = [];
  const barrierSpans = [];
  const piers = [];
  const signs = [];
  const furniture = [];
  const buildings = [];
  const props = [];

  const compiledEdges = [];
  for (let ei = 0; ei < edges.length; ei++) {
    const edge = edges[ei];
    if (!edge || !Array.isArray(edge.nodes) || edge.nodes.length < 2) continue;
    const axis = computeEdgeAxis(edge, roadNetwork && roadNetwork.lane_data);
    if (!axis || !axis.ok || axis.spine.length < 2) continue;
    const hw = axis.halfWidth || ((edge.lanes || DEFAULT_LANES) * (edge.lane_width || LANE_WIDTH) * 0.5);
    const totalLen = axis.cum[axis.cum.length - 1] || 0;
    const enuSpine = [];
    let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
    for (const p of axis.spine) {
      const pt = threeToWorld(p.px, p.py, p.pz);
      enuSpine.push(pt);
      if (pt[0] < minX) minX = pt[0];
      if (pt[0] > maxX) maxX = pt[0];
      if (pt[1] < minY) minY = pt[1];
      if (pt[1] > maxY) maxY = pt[1];
    }
    compiledEdges.push({
      edge,
      axis,
      enuSpine,
      minX, maxX, minY, maxY,
      halfWidth: hw,
      totalLen,
      override: _getEdgeOverride(edge, cfg.edgeOverrides),
    });
  }

  let seq = 0;
  for (let ei = 0; ei < compiledEdges.length; ei++) {
    const ce = compiledEdges[ei];
    const { edge, axis, halfWidth, totalLen, override } = ce;
    if (isGarageOrRampEdge(edge) || isTunnelEdge(edge)) continue;
    if (edge.type === EDGE_TYPE.INTERSECTION) continue;

    const isViaduct = edge.type === EDGE_TYPE.VIADUCT_HIGHWAY || edge.type === EDGE_TYPE.RAMP_CURVE;
    const allowPoles = !(override && override.poles === false);
    const allowTrees = !(override && override.trees === false) && !isViaduct && profile.treeSpacing > 0;
    const allowSigns = !(override && override.signs === false);
    const allowFurniture = !(override && override.furniture === false) && !isViaduct && profile.furnitureSpacing > 0;

    // 1. Poles
    if (allowPoles && totalLen > 12) {
      const poleStep = profile.poleSpacing;
      const offsetD = halfWidth + 1.15;
      for (let s = 14; s <= totalLen - 12; s += poleStep) {
        const sp = _sampleSpineAtArc(axis, s);
        if (!sp) continue;
        const [fx, fy] = forwardENU(sp.heading);
        const lx = -fy, ly = fx;
        for (const side of [-1, 1]) {
          const px = sp.x + lx * side * offsetD;
          const py = sp.y + ly * side * offsetD;
          const [tx, , tz] = worldToThree(px, py, sp.z);
          if (nearJunctionXZ(tx, tz)) continue;
          if (_isExcluded(px, py, cfg.excludeZones, 'pole')) continue;
          if (_pointNearAnyRoadBand(px, py, sp.z, compiledEdges, 0.25)) continue;
          poles.push({
            x: px,
            y: py,
            z: sp.z,
            nx: lx * side,
            ny: ly * side,
            heading: sp.heading,
            side: side > 0 ? 1 : -1,
          });
        }
      }
    }

    // 2. Trees
    if (allowTrees && totalLen > 16) {
      const treeStep = profile.treeSpacing;
      const offsetD = halfWidth + 3.2;
      for (let s = 12; s <= totalLen - 12; s += treeStep) {
        const sp = _sampleSpineAtArc(axis, s);
        if (!sp) continue;
        const [fx, fy] = forwardENU(sp.heading);
        const lx = -fy, ly = fx;
        for (const side of [-1, 1]) {
          const jitter = (_detRand(cfg.seed, 'tree_j', seq) - 0.5) * 0.6;
          const px = sp.x + lx * side * (offsetD + jitter);
          const py = sp.y + ly * side * (offsetD + jitter);
          const [tx, , tz] = worldToThree(px, py, sp.z);
          if (nearJunctionXZ(tx, tz)) continue;
          if (_isExcluded(px, py, cfg.excludeZones, 'tree')) continue;
          if (_pointNearAnyRoadBand(px, py, sp.z, compiledEdges, 1.35)) continue;
          const v = _detRand(cfg.seed, 'tree_v', seq++);
          trees.push({
            x: px,
            y: py,
            z: sp.z,
            variant: Math.round(v * 1000) / 1000,
            height_scale: Math.round((0.85 + 0.35 * v) * profile.treeScale * 1000) / 1000,
            crown_scale: Math.round((0.9 + 0.25 * _detRand(cfg.seed, 'tree_c', seq)) * 1000) / 1000,
            species: v > 0.72 ? 'pine' : 'broadleaf',
          });
        }
      }
    }

    // 3. Barriers (for viaducts or elevated decks)
    if (isViaduct && totalLen > 6) {
      const bStep = profile.barrierSpacing;
      const bOff = halfWidth + 0.45;
      for (const side of [-1, 1]) {
        let prev = null;
        for (let s = 3; s <= totalLen - 3; s += bStep) {
          const sp = _sampleSpineAtArc(axis, s);
          if (!sp) continue;
          const [fx, fy] = forwardENU(sp.heading);
          const lx = -fy, ly = fx;
          const px = sp.x + lx * side * bOff;
          const py = sp.y + ly * side * bOff;
          if (_isExcluded(px, py, cfg.excludeZones, 'barrier')) { prev = null; continue; }
          barrierPosts.push({ x: px, y: py, z: sp.z, heading: sp.heading });
          if (prev) {
            const len = distanceENU(prev.x, prev.y, px, py);
            if (len > 0.4 && len <= bStep * 2.2) {
              barrierSpans.push({
                x: (prev.x + px) * 0.5,
                y: (prev.y + py) * 0.5,
                z: (prev.z + sp.z) * 0.5,
                len,
                heading: headingBetweenPoints(prev.x, prev.y, px, py),
              });
            }
          }
          prev = { x: px, y: py, z: sp.z };
        }
      }
    }

    // 4. Elevated Piers (every 30m where elevation >= 2.0m; wide decks use 2-col portal, skip perpendicular crossings)
    if (totalLen > 20) {
      for (let s = 18; s <= totalLen - 18; s += 30) {
        const sp = _sampleSpineAtArc(axis, s);
        if (!sp || sp.z < 2.0) continue;
        const [fx, fy] = forwardENU(sp.heading);
        const lx = -fy, ly = fx;
        const wideDeck = halfWidth >= 5.5;
        let colOff = wideDeck ? halfWidth * 0.52 : 0.0;
        let columns = wideDeck ? 2 : 1;
        if (wideDeck) {
          const cxL = sp.x + lx * colOff, cyL = sp.y + ly * colOff;
          const cxR = sp.x - lx * colOff, cyR = sp.y - ly * colOff;
          if (_pointNearAnyRoadBand(cxL, cyL, 0.0, compiledEdges, 0.8) ||
              _pointNearAnyRoadBand(cxR, cyR, 0.0, compiledEdges, 0.8)) {
            colOff = halfWidth + 2.0;
            const wxL = sp.x + lx * colOff, wyL = sp.y + ly * colOff;
            const wxR = sp.x - lx * colOff, wyR = sp.y - ly * colOff;
            if (_pointNearAnyRoadBand(wxL, wyL, 0.0, compiledEdges, 0.8) ||
                _pointNearAnyRoadBand(wxR, wyR, 0.0, compiledEdges, 0.8)) {
              continue;
            }
          }
        } else if (_pointNearAnyRoadBand(sp.x, sp.y, 0.0, compiledEdges, 1.2)) {
          colOff = Math.max(halfWidth + 1.2, 5.2);
          const wxL = sp.x + lx * colOff, wyL = sp.y + ly * colOff;
          const wxR = sp.x - lx * colOff, wyR = sp.y - ly * colOff;
          if (_pointNearAnyRoadBand(wxL, wyL, 0.0, compiledEdges, 0.8) ||
              _pointNearAnyRoadBand(wxR, wyR, 0.0, compiledEdges, 0.8)) {
            continue;
          }
          columns = 2;
        }
        piers.push({
          x: sp.x,
          y: sp.y,
          z_ground: 0.0,
          z_top: sp.z - 0.35,
          heading: sp.heading,
          columns,
          col_offset: columns > 1 ? colOff : 0.0,
          radius: 0.55,
          cap_len: columns > 1 ? Math.max(halfWidth * 1.95, colOff * 2.15) : Math.max(2.2, halfWidth * 1.4),
          cap_w: 1.5,
          cap_h: 0.85,
        });
      }
    }

    // 5. Traffic Signs
    if (allowSigns && totalLen > 24) {
      const speedKph = Number(override && override.speed_limit_kph) ||
        (edge.type === EDGE_TYPE.HIGHWAY || isViaduct ? 80 : 60);
      const interval = profile.signInterval;
      for (let s = 18; s <= totalLen - 14; s += interval) {
        const sp = _sampleSpineAtArc(axis, s);
        if (!sp) continue;
        const [fx, fy] = forwardENU(sp.heading);
        const px = sp.x + fy * (halfWidth + 1.25);
        const py = sp.y - fx * (halfWidth + 1.25);
        const [tx, , tz] = worldToThree(px, py, sp.z);
        if (nearJunctionXZ(tx, tz)) continue;
        if (_isExcluded(px, py, cfg.excludeZones, 'sign')) continue;
        if (_pointNearAnyRoadBand(px, py, sp.z, compiledEdges, 0.35)) continue;
        signs.push({
          x: px,
          y: py,
          z: sp.z,
          heading: sp.heading,
          kind: s < 35 ? 'speed_limit' : (s > totalLen - 35 ? 'stop' : 'warning'),
          label: s < 35 ? String(speedKph) : (s > totalLen - 35 ? 'STOP' : '!'),
          speed_kph: speedKph,
        });
      }
    }

    // 6. Street Furniture (bus shelter, hydrant, bench, ev_charger)
    if (allowFurniture && totalLen > 45) {
      const fKinds = ['hydrant', 'bus_shelter', 'bench', 'ev_charger'];
      for (let s = 32; s <= totalLen - 25; s += profile.furnitureSpacing) {
        const sp = _sampleSpineAtArc(axis, s);
        if (!sp) continue;
        const [fx, fy] = forwardENU(sp.heading);
        const px = sp.x + fy * (halfWidth + 1.85);
        const py = sp.y - fx * (halfWidth + 1.85);
        const [tx, , tz] = worldToThree(px, py, sp.z);
        if (nearJunctionXZ(tx, tz)) continue;
        if (_isExcluded(px, py, cfg.excludeZones, 'furniture')) continue;
        if (_pointNearAnyRoadBand(px, py, sp.z, compiledEdges, 0.8)) continue;
        const kind = fKinds[seq % fKinds.length];
        seq++;
        furniture.push({
          x: px,
          y: py,
          z: sp.z,
          heading: sp.heading,
          length: kind === 'bus_shelter' ? 3.2 : 0.8,
          width: kind === 'bus_shelter' ? 1.2 : 0.7,
          height: kind === 'bus_shelter' ? 2.4 : 1.1,
          kind,
        });
      }
    }
  }

  // 7. Procedural Skyline Buildings (when roadNetwork lacks OSM buildings[])
  const hasOsmBuildings = !!(roadNetwork && Array.isArray(roadNetwork.buildings) && roadNetwork.buildings.length > 0);
  const buildingsSource = hasOsmBuildings ? 'osm' : 'world_engine';
  if (!hasOsmBuildings) {
    const spacing = Math.max(24, 34 / Math.max(0.35, profile.buildingDensity));
    for (let ei = 0; ei < compiledEdges.length; ei++) {
      const ce = compiledEdges[ei];
      const { edge, axis, halfWidth, totalLen, override } = ce;
      if (isGarageOrRampEdge(edge) || isTunnelEdge(edge)) continue;
      if (override && override.buildings === false) continue;
      for (let s = 26; s <= totalLen - 24; s += spacing) {
        const sp = _sampleSpineAtArc(axis, s);
        if (!sp || Math.abs(sp.z) > 1.5) continue;
        const [fx, fy] = forwardENU(sp.heading);
        const lx = -fy, ly = fx;
        for (const side of [-1, 1]) {
          const rW = _detRand(cfg.seed, 'bw', seq);
          const rD = _detRand(cfg.seed, 'bd', seq);
          const rH = _detRand(cfg.seed, 'bh', seq);
          const w = 16 + rW * 16;
          const d = 14 + rD * 14;
          const h = (16 + rH * 38) * profile.buildingHeightScale;
          const podiumMul = h > 28 ? 1.35 : 1.0;
          const setback = halfWidth + d * podiumMul * 0.5 + 8.5;
          const bx = sp.x + lx * side * setback;
          const by = sp.y + ly * side * setback;
          seq++;
          if (_isExcluded(bx, by, cfg.excludeZones, 'building')) continue;
          if (!_buildingClearsRoads(bx, by, w, d, sp.heading, h, compiledEdges, 3.2)) continue;
          const [tx, , tz] = worldToThree(bx, by, 0);
          if (nearJunctionXZ(tx, tz)) continue;
          buildings.push({
            x: bx,
            y: by,
            z: 0.0,
            width: Math.round(w * 100) / 100,
            depth: Math.round(d * 100) / 100,
            height: Math.round(h * 100) / 100,
            heading: sp.heading,
            layer: 0,
            seed: rH,
          });
        }
      }
    }
  }

  // 8. Custom Scenario Refinement Props (road_s & world + auto_z)
  for (let i = 0; i < cfg.customProps.length; i++) {
    const cp = cfg.customProps[i];
    if (!cp || typeof cp !== 'object') continue;
    let px = Number(cp.x) || 0;
    let py = Number(cp.y) || 0;
    let pz = Number(cp.z) || 0;
    let heading = Number(cp.heading) || 0;
    if (cp.placement === 'road_s') {
      const targetKey = String(cp.road != null ? cp.road : '');
      const matched = compiledEdges.find(ce =>
        (ce.edge.name != null && String(ce.edge.name) === targetKey) ||
        (ce.edge.id != null && String(ce.edge.id) === targetKey)
      );
      if (matched) {
        const sp = _sampleSpineAtArc(matched.axis, Math.max(0, Number(cp.s) || 0));
        if (sp) {
          const [fx, fy] = forwardENU(sp.heading);
          const dLat = Number(cp.d) || 0;
          px = sp.x - fy * dLat;
          py = sp.y + fx * dLat;
          pz = sp.z;
          heading = Number.isFinite(Number(cp.heading)) ? Number(cp.heading) : sp.heading;
        }
      }
    } else if (cp.auto_z) {
      pz = _nearestRoadZ(px, py, compiledEdges, pz);
    }
    props.push({
      id: String(cp.id || `prop_${i}`),
      kind: String(cp.kind || 'bollard'),
      label: String(cp.label || cp.title || ''),
      subtitle: String(cp.subtitle || ''),
      x: px,
      y: py,
      z: pz,
      heading,
      length: Number(cp.length) || 1.2,
      width: Number(cp.width) || 1.2,
      height: Number(cp.height) || 2.0,
      collider: Boolean(cp.collider),
    });
  }

  const sanity = {
    poles: poles.length,
    trees: trees.length,
    barrier_posts: barrierPosts.length,
    barrier_spans: barrierSpans.length,
    piers: piers.length,
    signs: signs.length,
    furniture: furniture.length,
    buildings: buildings.length,
    props: props.length,
    colliders: poles.length + trees.length + barrierSpans.length + props.filter(p => p.collider).length,
    remaining_asphalt_intrusions: 0,
    floating_count: 0,
  };

  const sig = _fnv1aStr(
    `${cfg.preset}|${cfg.seed}|${roadNetworkHash(roadNetwork)}|${poles.length}|${trees.length}|${buildings.length}|${props.length}`
  );
  const hashHex = sig.toString(16).padStart(8, '0') + _fnv1aStr(`${props.length}:${signs.length}`).toString(16).padStart(8, '0');

  return {
    schema: 1,
    authoritative: 'world_compiler_fallback',
    preset: cfg.preset,
    world_hash: hashHex,
    buildings_source: buildingsSource,
    sanity,
    poles,
    trees,
    barriers: { posts: barrierPosts, spans: barrierSpans },
    piers,
    signs,
    furniture,
    buildings,
    props,
  };
}

export default compileWorld;
