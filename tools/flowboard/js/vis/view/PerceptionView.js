/**
 * PerceptionView.js — 3D 感知标注与多模态预测光带覆盖层
 *
 * 车规级 SR（Surrounding Reality）视觉元素：
 * - 3D 角标框（8 个三轴短臂角标包围感知目标）
 * - 精细圆形 LiDAR 点云（真实点云高度渐变 / 目标受照面轮廓扫描点）
 * - 前保险杠传感器视场包络扇区 + 距离脉冲弧 + 贴地弱目标连线
 * - 多模态 Frenet 预测贴地 3D 渐变流光带（主模态 ~0.20m / 次模态 ~0.14m）
 *
 * 所有坐标/朝向/法线偏移均走 Coord.js，所有配色均取自 tokens.js (SCENE)。
 */

import {
  worldToThree,
  forwardENU,
  tangentToNormal,
  offsetAlongNormal,
  distanceENU,
} from '../math/Coord.js';
import { roadHeightAt } from '../math/RoadHeight.js';
import { SCENE } from '../theme/tokens.js';
import { VEHICLE_GROUND_Y } from './VehicleView.js';

// ── 角标框几何常量（每实体 8 个三轴 L 角标：4 顶角 + 4 底角） ──
const CORNER_LEN = 0.50;
const SEGMENTS_PER_CORNER = 3;
const VERTS_PER_SEGMENT = 2;
const COMPS_PER_VERT = 3;
const FLOATS_PER_CORNER = SEGMENTS_PER_CORNER * VERTS_PER_SEGMENT * COMPS_PER_VERT; // 18
const CORNERS = 8;
const FLOATS_PER_BOX = CORNERS * FLOATS_PER_CORNER; // 144
const VERTICES_PER_BOX = CORNERS * SEGMENTS_PER_CORNER * VERTS_PER_SEGMENT; // 48
const MAX_ENTITIES = 32;

// ── 传感器前视包络常量 ──
const SWEEP_SEGMENTS = 32;
const SWEEP_RADIUS = 36;
const BOUNDARY_RAY_LEN = 18;
const PULSE_CYCLE_FRAMES = 90;
const SWEEP_ABOVE_ROAD_M = 0.055;  // 相对路面(VEHICLE_GROUND_Y=0.10m)抬高至 0.155m，防止埋入沥青下方
const RAY_ABOVE_ROAD_M = 0.06;     // 贴地目标连线抬高至 0.16m

// ── LiDAR 点云常量（真实世界尺度 ~0.14m 柔和圆点） ──
const MAX_RADAR_POINTS = 2048;
const LIDAR_POINT_SIZE = 0.14;

// ── 多模态预测贴地光带常量 ──
const PRED_LIFT_M = 0.175;         // 高于全部道路标线(<=0.17m)，杜绝掠射角深度遮挡
const MIN_SECONDARY_PROB = 0.12;   // 过滤后端 5% 均匀底噪模态，仅在真实变道/切入/让行倾向时分叉显示次模态
const PRIMARY_WIDTH_START = 0.28;
const PRIMARY_WIDTH_END = 0.16;
const SECONDARY_WIDTH_START = 0.18;
const SECONDARY_WIDTH_END = 0.10;
const BEV_PRED_WIDTH_SCALE = 1.8;
const MAX_PRED_VERTS = 4096;
const MAX_PRED_INDICES = 12288;

const PREDICTION_COLORS = {
  lane_keep: SCENE.predLaneKeep,
  cut_in_left: SCENE.predCutIn,
  cut_in_right: SCENE.predCutIn,
  yield_decel: SCENE.predYield,
  default: SCENE.predDefault,
};

function _hexToRgb01(hex) {
  const n = Number(hex) || 0;
  return [
    ((n >> 16) & 255) / 255,
    ((n >> 8) & 255) / 255,
    (n & 255) / 255,
  ];
}

function _createLidarSpriteTexture() {
  if (typeof document === 'undefined') return null;
  const canvas = document.createElement('canvas');
  canvas.width = 64;
  canvas.height = 64;
  const ctx = canvas.getContext('2d');
  if (!ctx) return null;
  const grad = ctx.createRadialGradient(32, 32, 0, 32, 32, 30);
  grad.addColorStop(0.0, 'rgba(255, 255, 255, 1.0)');
  grad.addColorStop(0.45, 'rgba(235, 255, 252, 0.92)');
  grad.addColorStop(0.78, 'rgba(150, 245, 240, 0.30)');
  grad.addColorStop(1.0, 'rgba(0, 220, 210, 0.0)');
  ctx.clearRect(0, 0, 64, 64);
  ctx.fillStyle = grad;
  ctx.beginPath();
  ctx.arc(32, 32, 30, 0, Math.PI * 2);
  ctx.fill();
  return new THREE.CanvasTexture(canvas);
}

function _entityDims(ent) {
  const etype = ent.type || 'car';
  let defLen, defWid, defH;
  switch (etype) {
    case 'pedestrian':
      defLen = 0.55; defWid = 0.55; defH = 1.75; break;
    case 'bicycle': case 'cyclist': case 'motorcycle':
      defLen = 1.8; defWid = 0.65; defH = 1.6; break;
    case 'truck': case 'bus':
      defLen = 8.0; defWid = 2.5; defH = 3.6; break;
    case 'suv':
      defLen = 4.7; defWid = 2.0; defH = 1.82; break;
    case 'car': default:
      defLen = 4.6; defWid = 2.0; defH = 1.5; break;
  }
  return {
    halfL: (ent.length || defLen) * 0.5,
    halfW: (ent.width || defWid) * 0.5,
    vehH: ent.height || defH,
  };
}

function _isPerceivableObstacle(ent) {
  return !!ent && ent.type !== 'ego' && ent.type !== 'tl';
}

export function createPerceptionView(scene) {
  // ── 1. 3D 角标框 ──
  const boxPositions = new Float32Array(MAX_ENTITIES * FLOATS_PER_BOX);
  const boxGeo = new THREE.BufferGeometry();
  boxGeo.setAttribute('position', new THREE.BufferAttribute(boxPositions, 3));
  boxGeo.setDrawRange(0, 0);
  const boxMat = new THREE.LineBasicMaterial({
    color: SCENE.percBracket,
    transparent: true,
    opacity: 0.86,
    depthTest: true,
    depthWrite: false,
  });
  const boxMesh = new THREE.LineSegments(boxGeo, boxMat);
  boxMesh.renderOrder = 22;
  boxMesh.frustumCulled = false;
  scene.add(boxMesh);

  // ── 2. 贴地感知目标关联线（从自车外廓地面出发，低不透明度，绝不穿自车后挡风玻璃） ──
  const rayPositions = new Float32Array(MAX_ENTITIES * 2 * 3);
  const rayColors = new Float32Array(MAX_ENTITIES * 2 * 3);
  const rayGeo = new THREE.BufferGeometry();
  rayGeo.setAttribute('position', new THREE.BufferAttribute(rayPositions, 3));
  rayGeo.setAttribute('color', new THREE.BufferAttribute(rayColors, 3));
  rayGeo.setDrawRange(0, 0);
  const rayMat = new THREE.LineBasicMaterial({
    vertexColors: true,
    transparent: true,
    opacity: 0.22,
    depthTest: true,
    depthWrite: false,
  });
  const rayMesh = new THREE.LineSegments(rayGeo, rayMat);
  rayMesh.renderOrder = 21;
  rayMesh.frustumCulled = false;
  scene.add(rayMesh);

  // ── 3. 前保险杠传感器视场扇形（带索引三角扇 + 径向顶点色衰减至 0） ──
  const sweepVertCount = SWEEP_SEGMENTS + 2;
  const sweepPositions = new Float32Array(sweepVertCount * 3);
  const sweepColors = new Float32Array(sweepVertCount * 3);
  const sweepIndices = new Uint16Array(SWEEP_SEGMENTS * 3);
  for (let i = 0; i < SWEEP_SEGMENTS; i++) {
    sweepIndices[i * 3 + 0] = 0;
    sweepIndices[i * 3 + 1] = i + 1;
    sweepIndices[i * 3 + 2] = i + 2;
  }
  const sweepGeo = new THREE.BufferGeometry();
  sweepGeo.setAttribute('position', new THREE.BufferAttribute(sweepPositions, 3));
  sweepGeo.setAttribute('color', new THREE.BufferAttribute(sweepColors, 3));
  sweepGeo.setIndex(new THREE.BufferAttribute(sweepIndices, 1));
  const sweepMat = new THREE.MeshBasicMaterial({
    vertexColors: true,
    transparent: true,
    opacity: 0.20,
    side: THREE.DoubleSide,
    depthTest: true,
    depthWrite: false,
    blending: THREE.AdditiveBlending,
    polygonOffset: true,
    polygonOffsetFactor: -3,
    polygonOffsetUnits: -6,
  });
  const sweepMesh = new THREE.Mesh(sweepGeo, sweepMat);
  sweepMesh.renderOrder = 20;
  sweepMesh.frustumCulled = false;
  scene.add(sweepMesh);

  // 前视视场左右短边界线 (2 段) + 向前传播的距离脉冲弧 (SWEEP_SEGMENTS 段)
  const sweepEdgeSegs = 2 + SWEEP_SEGMENTS;
  const sweepEdgePositions = new Float32Array(sweepEdgeSegs * 2 * 3);
  const sweepEdgeColors = new Float32Array(sweepEdgeSegs * 2 * 3);
  const sweepEdgeGeo = new THREE.BufferGeometry();
  sweepEdgeGeo.setAttribute('position', new THREE.BufferAttribute(sweepEdgePositions, 3));
  sweepEdgeGeo.setAttribute('color', new THREE.BufferAttribute(sweepEdgeColors, 3));
  const sweepEdgeMat = new THREE.LineBasicMaterial({
    vertexColors: true,
    transparent: true,
    opacity: 0.22,
    depthTest: true,
    depthWrite: false,
  });
  const sweepEdgeMesh = new THREE.LineSegments(sweepEdgeGeo, sweepEdgeMat);
  sweepEdgeMesh.renderOrder = 21;
  sweepEdgeMesh.frustumCulled = false;
  scene.add(sweepEdgeMesh);

  // ── 4. 3D LiDAR 点云（柔和圆形精灵 + 真实厘米级尺度） ──
  const pointPositions = new Float32Array(MAX_RADAR_POINTS * 3);
  const pointColors = new Float32Array(MAX_RADAR_POINTS * 3);
  const pointGeo = new THREE.BufferGeometry();
  pointGeo.setAttribute('position', new THREE.BufferAttribute(pointPositions, 3));
  pointGeo.setAttribute('color', new THREE.BufferAttribute(pointColors, 3));
  pointGeo.setDrawRange(0, 0);

  const pointSpriteTex = _createLidarSpriteTexture();
  const pointMat = new THREE.PointsMaterial({
    size: LIDAR_POINT_SIZE,
    sizeAttenuation: true,
    map: pointSpriteTex,
    alphaTest: 0.04,
    vertexColors: true,
    transparent: true,
    opacity: 0.90,
    depthTest: true,
    depthWrite: false,
  });
  const pointMesh = new THREE.Points(pointGeo, pointMat);
  pointMesh.renderOrder = 24;
  pointMesh.frustumCulled = false;
  scene.add(pointMesh);

  // ── 5. 多模态预测贴地渐变 3D 光带（单 Mesh 批处理，零逐帧分配） ──
  const predGroup = new THREE.Group();
  predGroup.name = 'predictionTrajectories';
  scene.add(predGroup);

  const predPositions = new Float32Array(MAX_PRED_VERTS * 3);
  const predColors = new Float32Array(MAX_PRED_VERTS * 4);
  const predIndices = new Uint16Array(MAX_PRED_INDICES);
  const predGeo = new THREE.BufferGeometry();
  predGeo.setAttribute('position', new THREE.BufferAttribute(predPositions, 3));
  predGeo.setAttribute('color', new THREE.BufferAttribute(predColors, 4));
  predGeo.setIndex(new THREE.BufferAttribute(predIndices, 1));
  predGeo.setDrawRange(0, 0);

  const predMat = new THREE.MeshBasicMaterial({
    vertexColors: true,
    transparent: true,
    depthTest: true,
    depthWrite: false,
    side: THREE.DoubleSide,
    polygonOffset: true,
    polygonOffsetFactor: -4,
    polygonOffsetUnits: -8,
  });
  const predMesh = new THREE.Mesh(predGeo, predMat);
  predMesh.renderOrder = 25;
  predMesh.frustumCulled = false;
  predGroup.add(predMesh);

  function _clearPredictions() {
    predGeo.setDrawRange(0, 0);
  }

  function _findMatchedEntity(pred, store) {
    const entities = store.entities || [];
    const objId = pred.object_id ?? pred.id;
    if (objId !== undefined && objId !== null) {
      for (let i = 0; i < entities.length; i++) {
        const cand = entities[i];
        if (_isPerceivableObstacle(cand) && cand.id === objId) return cand;
      }
    }
    const pwx = Number(pred.wx);
    const pwy = Number(pred.wy);
    if (Number.isFinite(pwx) && Number.isFinite(pwy)) {
      let best = null;
      let bestDist = 6.0;
      for (let i = 0; i < entities.length; i++) {
        const cand = entities[i];
        if (!_isPerceivableObstacle(cand)) continue;
        const d = distanceENU(pwx, pwy, cand.x || 0, cand.y || 0);
        if (d <= bestDist) {
          bestDist = d;
          best = cand;
        }
      }
      return best;
    }
    return null;
  }

  function _extractBranchWorldPoints(pred, br, ent, store) {
    if (Array.isArray(br.points) && br.points.length >= 2) {
      return { pts: br.points, hasElevation: true };
    }
    if (Array.isArray(br.world_waypoints) && br.world_waypoints.length >= 2) {
      const wps = br.world_waypoints;
      const firstWp0 = wps[0];
      if (!Array.isArray(firstWp0) || firstWp0.length < 2) return { pts: null, hasElevation: false };

      const pwx = Number(pred.wx);
      const pwy = Number(pred.wy);
      // 防御后端局部 Frenet 参考线截断夹紧幻影：若首预测点距目标真实坐标超过 16m，说明目标超出局部 Frenet 域被截断到自车附近，予以剔除
      if (Number.isFinite(pwx) && Number.isFinite(pwy)) {
        const jumpDist = distanceENU(pwx, pwy, Number(firstWp0[0]) || 0, Number(firstWp0[1]) || 0);
        if (jumpDist > 16.0) return { pts: null, hasElevation: false };
      }

      const out = [];
      let startIdx = 0;
      if (ent && Number.isFinite(ent.x) && Number.isFinite(ent.y)) {
        const [efx, efy] = forwardENU(ent.heading || 0);
        const frontOff = (ent.length || 4.6) * 0.35;
        const ax = ent.x + efx * frontOff;
        const ay = ent.y + efy * frontOff;
        while (startIdx < wps.length - 1) {
          const wx0 = Number(wps[startIdx]?.[0]);
          const wy0 = Number(wps[startIdx]?.[1]);
          if (!Number.isFinite(wx0) || !Number.isFinite(wy0)) { startIdx++; continue; }
          if ((wx0 - ax) * efx + (wy0 - ay) * efy >= 0.35) break;
          startIdx++;
        }
        const firstWp = wps[startIdx];
        if (firstWp && distanceENU(ax, ay, Number(firstWp[0]) || 0, Number(firstWp[1]) || 0) <= 12.0) {
          out.push([ax, ay]);
        }
      } else if (Number.isFinite(pwx) && Number.isFinite(pwy)) {
        if (distanceENU(pwx, pwy, Number(firstWp0[0]) || 0, Number(firstWp0[1]) || 0) <= 12.0) {
          out.push([pwx, pwy]);
        }
      }

      for (let k = startIdx; k < wps.length; k++) {
        const wp = wps[k];
        if (Array.isArray(wp) && wp.length >= 2) {
          out.push([Number(wp[0]), Number(wp[1])]);
        }
      }
      return { pts: out, hasElevation: false };
    }
    if (Array.isArray(br.waypoints) && br.waypoints.length >= 2 && store.ego) {
      const ego = store.ego;
      const [fx, fy] = forwardENU(ego.heading || 0);
      const out = [];
      for (let k = 0; k < br.waypoints.length; k++) {
        const wp = br.waypoints[k];
        if (!Array.isArray(wp) || wp.length < 2) continue;
        const bx = Number(wp[0]);
        const by = Number(wp[1]);
        if (!Number.isFinite(bx) || !Number.isFinite(by)) continue;
        out.push([ego.x + fx * bx - fy * by, ego.y + fy * bx + fx * by]);
      }
      return { pts: out, hasElevation: false };
    }
    return { pts: null, hasElevation: false };
  }

  function _sampleBranchPoints(rawPts, store, hasElevation = false) {
    const raw3d = [];
    for (let i = 0; i < rawPts.length; i++) {
      const pt = rawPts[i];
      if (!pt) continue;
      const px = Number(pt[0]);
      const py = Number(pt[1]);
      if (!Number.isFinite(px) || !Number.isFinite(py)) continue;
      const baseZ = (hasElevation && pt.length >= 3 && Number.isFinite(Number(pt[2])))
        ? Number(pt[2])
        : roadHeightAt(store, px, py);
      const [tx, ty, tz] = worldToThree(px, py, baseZ + PRED_LIFT_M);
      raw3d.push(new THREE.Vector3(tx, ty, tz));
    }
    if (raw3d.length < 2) return [];
    if (raw3d[0].distanceTo(raw3d[raw3d.length - 1]) < 0.5) return [];

    if (raw3d.length === 2) {
      const a = raw3d[0], b = raw3d[1];
      const steps = 10;
      const out = [];
      for (let s = 0; s <= steps; s++) {
        const t = s / steps;
        out.push({
          x: a.x + (b.x - a.x) * t,
          y: a.y + (b.y - a.y) * t,
          z: a.z + (b.z - a.z) * t,
        });
      }
      return out;
    }

    const subdiv = Math.max(12, Math.min(28, (raw3d.length - 1) * 6));
    const curve = new THREE.CatmullRomCurve3(raw3d, false, 'centripetal', 0.5);
    return curve.getPoints(subdiv);
  }

  function _syncPredictions(store) {
    const showPred = !store.layerVisibility || store.layerVisibility.predictions !== false;
    if (!showPred || !Array.isArray(store.predictions) || store.predictions.length === 0) {
      predGroup.visible = false;
      _clearPredictions();
      return;
    }
    predGroup.visible = true;

    const wScale = store.isBev ? BEV_PRED_WIDTH_SCALE : 1.0;
    let vCursor = 0;
    let iCursor = 0;

    for (let p = 0; p < store.predictions.length; p++) {
      const pred = store.predictions[p];
      if (!pred) continue;
      const branches = Array.isArray(pred.branches)
        ? pred.branches
        : (Array.isArray(pred.trajectories) ? pred.trajectories : null);
      if (!branches || branches.length === 0) continue;

      let maxProb = 0;
      for (let b = 0; b < branches.length; b++) {
        const pr = Number(branches[b]?.probability ?? branches[b]?.prob) || 0;
        if (pr > maxProb) maxProb = pr;
      }

      const matchedEnt = _findMatchedEntity(pred, store);
      if (!matchedEnt && store.ego && Number.isFinite(Number(pred.wx)) && Number.isFinite(Number(pred.wy))) {
        if (distanceENU(Number(pred.wx), Number(pred.wy), store.ego.x || 0, store.ego.y || 0) < 3.5) {
          continue;
        }
      }
      let isOpposing = false;
      if (matchedEnt && store.ego) {
        const [efx, efy] = forwardENU(store.ego.heading || 0);
        const [tfx, tfy] = forwardENU(matchedEnt.heading || 0);
        isOpposing = (efx * tfx + efy * tfy) < -0.35;
      }

      for (let b = 0; b < branches.length; b++) {
        const br = branches[b];
        if (!br) continue;

        const mode = br.mode || br.intent || 'lane_keep';
        const rawProb = Number(br.probability ?? br.prob);
        const probVal = Number.isFinite(rawProb) ? rawProb : 0.5;
        const isPrimary = probVal >= maxProb - 1e-3;
        if (!isPrimary && probVal < MIN_SECONDARY_PROB) continue;
        if (!isPrimary && isOpposing && String(mode).startsWith('cut_in')) continue;

        const { pts: rawPts, hasElevation } = _extractBranchWorldPoints(pred, br, matchedEnt, store);
        if (!rawPts || rawPts.length < 2) continue;

        const pts = _sampleBranchPoints(rawPts, store, hasElevation);
        const n = pts.length;
        if (n < 2) continue;
        if (vCursor + n * 2 > MAX_PRED_VERTS || iCursor + (n - 1) * 6 > MAX_PRED_INDICES) break;

        const prob = Math.max(0.1, Math.min(1.0, probVal));
        const hex = PREDICTION_COLORS[mode] || PREDICTION_COLORS.default;
        const [baseR, baseG, baseB] = _hexToRgb01(hex);

        const wStart = (isPrimary ? PRIMARY_WIDTH_START : SECONDARY_WIDTH_START) * wScale;
        const wEnd = (isPrimary ? PRIMARY_WIDTH_END : SECONDARY_WIDTH_END) * wScale;
        const alphaStart = isPrimary ? (0.66 + 0.28 * prob) : (0.38 + 0.28 * prob);
        const alphaEnd = isPrimary ? 0.08 : 0.03;

        const baseVert = vCursor;
        for (let i = 0; i < n; i++) {
          const cur = pts[i];
          const prev = pts[Math.max(0, i - 1)];
          const next = pts[Math.min(n - 1, i + 1)];
          const dx = next.x - prev.x;
          const dz = next.z - prev.z;
          const [nx, nz] = tangentToNormal(dx, dz);

          const u = i / (n - 1);
          let width = wStart + (wEnd - wStart) * u;
          // 末端收尖为平滑导向箭头尖端
          if (u > 0.86) {
            const tipU = (u - 0.86) / 0.14;
            width = tipU < 0.28
              ? wEnd * (1.0 + 0.55 * (tipU / 0.28))
              : wEnd * 1.55 * (1.0 - (tipU - 0.28) / 0.72);
          }
          const halfW = Math.max(0.004, width * 0.5);

          const [lx, , lz] = offsetAlongNormal(cur.x, cur.z, nx, nz, -halfW);
          const [rx, , rz] = offsetAlongNormal(cur.x, cur.z, nx, nz, halfW);

          // 起点略提亮，沿远端地平线平滑衰减 alpha
          const glowBoost = (1.0 - u) * 0.22;
          const cr = Math.min(1.0, baseR + glowBoost);
          const cg = Math.min(1.0, baseG + glowBoost);
          const cb = Math.min(1.0, baseB + glowBoost);
          const alpha = alphaStart * (1.0 - u * 0.90) + alphaEnd * (u * 0.90);

          const vp = vCursor * 3;
          const vc = vCursor * 4;
          predPositions[vp + 0] = lx;
          predPositions[vp + 1] = cur.y;
          predPositions[vp + 2] = lz;
          predColors[vc + 0] = cr;
          predColors[vc + 1] = cg;
          predColors[vc + 2] = cb;
          predColors[vc + 3] = alpha;

          predPositions[vp + 3] = rx;
          predPositions[vp + 4] = cur.y;
          predPositions[vp + 5] = rz;
          predColors[vc + 4] = cr;
          predColors[vc + 5] = cg;
          predColors[vc + 6] = cb;
          predColors[vc + 7] = alpha;

          vCursor += 2;
        }

        for (let i = 0; i < n - 1; i++) {
          const a = baseVert + i * 2;
          const bIdx = a + 1;
          const c = a + 2;
          const d = a + 3;
          predIndices[iCursor++] = a;
          predIndices[iCursor++] = bIdx;
          predIndices[iCursor++] = c;
          predIndices[iCursor++] = bIdx;
          predIndices[iCursor++] = d;
          predIndices[iCursor++] = c;
        }
      }
    }

    predGeo.attributes.position.needsUpdate = true;
    predGeo.attributes.color.needsUpdate = true;
    predGeo.index.needsUpdate = true;
    predGeo.setDrawRange(0, iCursor);
  }

  function _updatePointCloud(ego, entities, store) {
    let pIdx = 0;
    const egoZ = ego.z || 0;

    const pushThreePt = (tx, ty, tz, r, g, b) => {
      if (pIdx >= MAX_RADAR_POINTS) return;
      const base = pIdx * 3;
      pointPositions[base + 0] = tx;
      pointPositions[base + 1] = ty;
      pointPositions[base + 2] = tz;
      pointColors[base + 0] = r;
      pointColors[base + 1] = g;
      pointColors[base + 2] = b;
      pIdx++;
    };

    if (Array.isArray(store.pointCloud) && store.pointCloud.length > 0) {
      for (let i = 0; i < store.pointCloud.length; i++) {
        if (pIdx >= MAX_RADAR_POINTS) break;
        const pt = store.pointCloud[i];
        const wx = pt.x ?? pt[0] ?? 0;
        const wy = pt.y ?? pt[1] ?? 0;
        const wz = pt.z ?? pt[2] ?? 0;
        const [tx, ty, tz] = worldToThree(wx, wy, wz);
        const relZ = Math.max(0, wz - egoZ);

        // 连续高度梯度：地面深青 → 车身青绿 → 高处柔和暖琥珀（杜绝亮黄方块）
        let r, g, b;
        if (relZ < 0.22) {
          r = 0.08; g = 0.42; b = 0.54;
        } else if (relZ < 1.35) {
          const t = (relZ - 0.22) / 1.13;
          r = 0.02 + 0.06 * t;
          g = 0.80 + 0.14 * t;
          b = 0.92 - 0.20 * t;
        } else {
          const t = Math.min(1.0, (relZ - 1.35) / 1.85);
          r = 0.08 + 0.82 * t;
          g = 0.94 - 0.22 * t;
          b = 0.72 - 0.40 * t;
        }
        pushThreePt(tx, ty, tz, r, g, b);
      }
    } else {
      // 无原始点云流时：在感知目标朝向自车的保险杠与侧翼表面合成精细双线束 LiDAR 扫描轮廓点
      const [ego3X, , ego3Z] = worldToThree(ego.x, ego.y, egoZ);
      const bumperRatios = [-0.86, -0.57, -0.29, 0.0, 0.29, 0.57, 0.86];
      const flankRatios = [-0.72, -0.36, 0.0, 0.36, 0.72];
      const cabinRatios = [-0.54, -0.18, 0.18, 0.54];

      const maxScanEntities = Math.min(entities.length, 24);
      for (let i = 0; i < maxScanEntities; i++) {
        const ent = entities[i];
        if (!_isPerceivableObstacle(ent)) continue;
        const dist = distanceENU(ego.x, ego.y, ent.x || 0, ent.y || 0);
        if (dist > 75 || dist < 1.8) continue;

        const { halfL, halfW, vehH } = _entityDims(ent);
        const entGroundZ = (ent.z != null) ? ent.z : roadHeightAt(store, ent.x || 0, ent.y || 0);
        const [cx, cy, cz] = worldToThree(ent.x || 0, ent.y || 0, entGroundZ + VEHICLE_GROUND_Y);

        const [efx, efy] = forwardENU(ent.heading || 0);
        const [fwdX, , fwdZ] = worldToThree(efx, efy, 0);
        const [normX, normZ] = tangentToNormal(fwdX, fwdZ);

        const toEgoX = ego3X - cx;
        const toEgoZ = ego3Z - cz;
        const faceLong = (toEgoX * fwdX + toEgoZ * fwdZ) >= 0 ? 1 : -1;
        const faceLat = (toEgoX * normX + toEgoZ * normZ) >= 0 ? 1 : -1;

        const fade = Math.max(0.42, 1.0 - dist / 90);
        const yBand1 = cy + vehH * 0.28;
        const yBand2 = cy + vehH * 0.54;
        const yBand3 = cy + vehH * 0.78;

        // 受照前/后保险杠双层水平激光线束
        const bumperCenterX = cx + faceLong * halfL * fwdX;
        const bumperCenterZ = cz + faceLong * halfL * fwdZ;
        for (let k = 0; k < bumperRatios.length; k++) {
          const lat = bumperRatios[k] * halfW;
          const [px, , pz] = offsetAlongNormal(bumperCenterX, bumperCenterZ, normX, normZ, lat);
          pushThreePt(px, yBand1, pz, 0.02 * fade, 0.84 * fade, 0.86 * fade);
          pushThreePt(px, yBand2, pz, 0.06 * fade, 0.94 * fade, 0.78 * fade);
        }

        // 受照侧翼双层水平激光线束（仅在存在侧向视角夹角时可见）
        const flankDot = Math.abs(toEgoX * normX + toEgoZ * normZ) / (dist || 1);
        if (flankDot > 0.06) {
          for (let k = 0; k < flankRatios.length; k++) {
            const lon = flankRatios[k] * halfL;
            const baseX = cx + lon * fwdX;
            const baseZ = cz + lon * fwdZ;
            const [px, , pz] = offsetAlongNormal(baseX, baseZ, normX, normZ, faceLat * halfW);
            pushThreePt(px, yBand1, pz, 0.02 * fade, 0.80 * fade, 0.84 * fade);
            pushThreePt(px, yBand2, pz, 0.06 * fade, 0.92 * fade, 0.76 * fade);
          }
        }

        // 舱体肩线/风挡受照微弱回波点
        const cabinCenterX = cx + faceLong * (halfL * 0.62) * fwdX;
        const cabinCenterZ = cz + faceLong * (halfL * 0.62) * fwdZ;
        for (let k = 0; k < cabinRatios.length; k++) {
          const lat = cabinRatios[k] * (halfW * 0.82);
          const [px, , pz] = offsetAlongNormal(cabinCenterX, cabinCenterZ, normX, normZ, lat);
          pushThreePt(px, yBand3, pz, 0.10 * fade, 0.95 * fade, 0.84 * fade);
        }
      }
    }

    pointGeo.attributes.position.needsUpdate = true;
    pointGeo.attributes.color.needsUpdate = true;
    pointGeo.setDrawRange(0, pIdx);
  }

  let _frame = 0;

  function update(store, _now) {
    _frame++;
    const ego = store.ego;
    const showPerc = !store.layerVisibility || store.layerVisibility.perception !== false;

    // 1. 同步多模态预测贴地光带
    _syncPredictions(store);

    if (!ego || !showPerc) {
      boxMesh.visible = false;
      rayMesh.visible = false;
      sweepMesh.visible = false;
      sweepEdgeMesh.visible = false;
      pointMesh.visible = false;
      pointGeo.setDrawRange(0, 0);
      return;
    }

    boxMesh.visible = true;
    rayMesh.visible = true;
    sweepMesh.visible = true;
    sweepEdgeMesh.visible = true;
    pointMesh.visible = true;

    const isNight = !!(store.env && (store.env.isNight || store.env.lighting === 'night'));
    sweepMat.opacity = isNight ? 0.14 : 0.22;

    const entities = store.entities || [];
    const count = Math.min(entities.length, MAX_ENTITIES);

    // 2. 更新 3D 感知角标框
    _updateBrackets(ego, entities, count, store, boxPositions);
    boxGeo.attributes.position.needsUpdate = true;
    boxGeo.setDrawRange(0, count * VERTICES_PER_BOX);

    // 3. 更新贴地传感器目标关联线
    _updateRays(ego, entities, count, store, rayPositions, rayColors);
    rayGeo.attributes.position.needsUpdate = true;
    rayGeo.attributes.color.needsUpdate = true;
    rayGeo.setDrawRange(0, count * 2);

    // 4. 更新前保险杠传感器视场包络与脉冲弧
    _updateSweep(ego, store, _frame, sweepPositions, sweepColors, sweepEdgePositions, sweepEdgeColors);
    sweepGeo.attributes.position.needsUpdate = true;
    sweepGeo.attributes.color.needsUpdate = true;
    sweepEdgeGeo.attributes.position.needsUpdate = true;
    sweepEdgeGeo.attributes.color.needsUpdate = true;

    // 5. 更新精细 LiDAR 点云
    _updatePointCloud(ego, entities, store);
  }

  function clear() {
    boxGeo.setDrawRange(0, 0);
    rayGeo.setDrawRange(0, 0);
    pointGeo.setDrawRange(0, 0);
    _clearPredictions();
    boxMesh.visible = false;
    rayMesh.visible = false;
    sweepMesh.visible = false;
    sweepEdgeMesh.visible = false;
    pointMesh.visible = false;
    predGroup.visible = false;
  }

  function dispose() {
    clear();
    scene.remove(boxMesh);
    scene.remove(rayMesh);
    scene.remove(sweepMesh);
    scene.remove(sweepEdgeMesh);
    scene.remove(pointMesh);
    scene.remove(predGroup);
    boxGeo.dispose();
    boxMat.dispose();
    rayGeo.dispose();
    rayMat.dispose();
    sweepGeo.dispose();
    sweepMat.dispose();
    sweepEdgeGeo.dispose();
    sweepEdgeMat.dispose();
    pointGeo.dispose();
    pointMat.dispose();
    pointSpriteTex?.dispose?.();
    predGeo.dispose();
    predMat.dispose();
  }

  return { update, clear, dispose };
}

// ── 内部几何更新函数 ──

function _updateBrackets(ego, entities, count, store, positions) {
  for (let i = 0; i < count; i++) {
    const ent = entities[i];
    const bi = i * FLOATS_PER_BOX;
    if (!_isPerceivableObstacle(ent)) {
      for (let j = 0; j < FLOATS_PER_BOX; j++) positions[bi + j] = 0;
      continue;
    }

    const dist = distanceENU(ego.x, ego.y, ent.x || 0, ent.y || 0);
    if (dist > 95 || dist < 1.8) {
      for (let j = 0; j < FLOATS_PER_BOX; j++) positions[bi + j] = 0;
      continue;
    }

    const { halfL: rawHalfL, halfW: rawHalfW, vehH } = _entityDims(ent);
    const halfL = rawHalfL + 0.08;
    const halfW = rawHalfW + 0.06;

    const minDim = Math.min(halfL * 2, halfW * 2);
    const cl = Math.max(0.10, Math.min(CORNER_LEN, minDim * 0.24));

    const entZ = (ent.z != null) ? ent.z : roadHeightAt(store, ent.x || 0, ent.y || 0);
    const [cx, cyRaw, cz] = worldToThree(ent.x || 0, ent.y || 0, entZ + VEHICLE_GROUND_Y);
    const bottomY = cyRaw + 0.04;
    const roofY = cyRaw + vehH + 0.04;

    const [efx, efy] = forwardENU(ent.heading || 0);
    const [fwdX, , fwdZ] = worldToThree(efx, efy, 0);
    const [normX, normZ] = tangentToNormal(fwdX, fwdZ);

    // 4 个角点：[前/后偏移 (沿 fwd), 右/左偏移 (沿右侧法线 norm)]
    const cornerOffsets = [
      [ halfL, -halfW], // front-left
      [ halfL,  halfW], // front-right
      [-halfL,  halfW], // back-right
      [-halfL, -halfW], // back-left
    ];

    let idx = 0;
    const writeCorners = (planeY, isTop) => {
      const vDir = isTop ? -1 : 1;
      for (let c = 0; c < cornerOffsets.length; c++) {
        const [foff, roff] = cornerOffsets[c];
        const longX = cx + foff * fwdX;
        const longZ = cz + foff * fwdZ;
        const [cx3, , cz3] = offsetAlongNormal(longX, longZ, normX, normZ, roff);

        const inFwd = -Math.sign(foff);
        const inNorm = -Math.sign(roff);
        const [latEndX, , latEndZ] = offsetAlongNormal(cx3, cz3, normX, normZ, inNorm * cl);

        // 1) 垂直短臂
        positions[bi + idx++] = cx3; positions[bi + idx++] = planeY;            positions[bi + idx++] = cz3;
        positions[bi + idx++] = cx3; positions[bi + idx++] = planeY + vDir * cl; positions[bi + idx++] = cz3;

        // 2) 纵向向内短臂
        positions[bi + idx++] = cx3;                   positions[bi + idx++] = planeY; positions[bi + idx++] = cz3;
        positions[bi + idx++] = cx3 + inFwd * cl * fwdX; positions[bi + idx++] = planeY; positions[bi + idx++] = cz3 + inFwd * cl * fwdZ;

        // 3) 横向向内短臂
        positions[bi + idx++] = cx3;     positions[bi + idx++] = planeY; positions[bi + idx++] = cz3;
        positions[bi + idx++] = latEndX; positions[bi + idx++] = planeY; positions[bi + idx++] = latEndZ;
      }
    };

    writeCorners(roofY, true);
    writeCorners(bottomY, false);
  }
}

function _updateRays(ego, entities, count, store, positions, colors) {
  const egoGroundZ = (ego.z != null) ? ego.z : roadHeightAt(store, ego.x, ego.y);
  const [ex, ey, ez] = worldToThree(ego.x, ego.y, egoGroundZ + VEHICLE_GROUND_Y + RAY_ABOVE_ROAD_M);
  const [efx, efy] = forwardENU(ego.heading || 0);
  const [egoFwdX, , egoFwdZ] = worldToThree(efx, efy, 0);
  const egoHalfL = (ego.length || 4.6) * 0.5;
  const [fovR, fovG, fovB] = _hexToRgb01(SCENE.percFov);
  const [brkR, brkG, brkB] = _hexToRgb01(SCENE.percBracket);

  for (let i = 0; i < count; i++) {
    const ent = entities[i];
    const baseIdx = i * 6;
    if (!_isPerceivableObstacle(ent)) {
      for (let j = 0; j < 6; j++) { positions[baseIdx + j] = 0; colors[baseIdx + j] = 0; }
      continue;
    }

    const dist = distanceENU(ego.x, ego.y, ent.x || 0, ent.y || 0);
    const entGroundZ = (ent.z != null) ? ent.z : roadHeightAt(store, ent.x || 0, ent.y || 0);
    const [tx, ty, tz] = worldToThree(ent.x || 0, ent.y || 0, entGroundZ + VEHICLE_GROUND_Y + RAY_ABOVE_ROAD_M);

    const dirX = (tx - ex) / (dist || 1);
    const dirZ = (tz - ez) / (dist || 1);
    const forwardDot = dirX * egoFwdX + dirZ * egoFwdZ;

    // 仅对自车前向/侧前方视场内 (forwardDot >= -0.10) 且距离适中的目标绘制贴地弱连线
    if (dist > 75 || dist < 4.5 || forwardDot < -0.10) {
      for (let j = 0; j < 6; j++) { positions[baseIdx + j] = 0; colors[baseIdx + j] = 0; }
      continue;
    }

    const { halfL: targetHalfL } = _entityDims(ent);
    const startOff = egoHalfL + 0.55;
    const endOff = Math.max(startOff, dist - targetHalfL - 0.35);
    const atten = Math.max(0.28, 1.0 - dist / 85);

    positions[baseIdx + 0] = ex + dirX * startOff;
    positions[baseIdx + 1] = ey;
    positions[baseIdx + 2] = ez + dirZ * startOff;
    colors[baseIdx + 0] = fovR * 0.82 * atten;
    colors[baseIdx + 1] = fovG * 0.82 * atten;
    colors[baseIdx + 2] = fovB * 0.82 * atten;

    positions[baseIdx + 3] = ex + dirX * endOff;
    positions[baseIdx + 4] = ty;
    positions[baseIdx + 5] = ez + dirZ * endOff;
    colors[baseIdx + 3] = brkR * atten;
    colors[baseIdx + 4] = brkG * atten;
    colors[baseIdx + 5] = brkB * atten;
  }
}

function _updateSweep(ego, store, frame, positions, colors, edgePositions, edgeColors) {
  const heading = ego.heading || 0;
  const halfFov = Math.PI * 55 / 180;
  const sweepStart = heading - halfFov;
  const sweepEnd = heading + halfFov;

  const egoHalfL = (ego.length || 4.6) * 0.5;
  const [efx, efy] = forwardENU(heading);
  const bumperEnuX = ego.x + efx * egoHalfL;
  const bumperEnuY = ego.y + efy * egoHalfL;
  const egoGroundZ = (ego.z != null) ? ego.z : roadHeightAt(store, bumperEnuX, bumperEnuY);
  const sweepSurfaceZ = egoGroundZ + VEHICLE_GROUND_Y + SWEEP_ABOVE_ROAD_M;
  const [bx, by, bz] = worldToThree(bumperEnuX, bumperEnuY, sweepSurfaceZ);

  const [fovR, fovG, fovB] = _hexToRgb01(SCENE.percFov);

  // 扇形中心顶点（前保险杠正中央，径向亮度最高，向外缘平滑衰减至 0）
  positions[0] = bx;
  positions[1] = by;
  positions[2] = bz;
  colors[0] = fovR;
  colors[1] = fovG;
  colors[2] = fovB;

  for (let i = 0; i <= SWEEP_SEGMENTS; i++) {
    const t = sweepStart + (sweepEnd - sweepStart) * (i / SWEEP_SEGMENTS);
    const [fx, fy] = forwardENU(t);
    const [px, py, pz] = worldToThree(
      bumperEnuX + fx * SWEEP_RADIUS,
      bumperEnuY + fy * SWEEP_RADIUS,
      sweepSurfaceZ
    );
    const vi = (i + 1) * 3;
    positions[vi + 0] = px;
    positions[vi + 1] = py;
    positions[vi + 2] = pz;
    colors[vi + 0] = 0;
    colors[vi + 1] = 0;
    colors[vi + 2] = 0;
  }

  // 左右短视场边界线（长 18m，远端顶点色衰减至 0）
  const [fxL, fyL] = forwardENU(sweepEnd);
  const [lx, ly, lz] = worldToThree(bumperEnuX + fxL * BOUNDARY_RAY_LEN, bumperEnuY + fyL * BOUNDARY_RAY_LEN, sweepSurfaceZ);
  const [fxR, fyR] = forwardENU(sweepStart);
  const [rx, ry, rz] = worldToThree(bumperEnuX + fxR * BOUNDARY_RAY_LEN, bumperEnuY + fyR * BOUNDARY_RAY_LEN, sweepSurfaceZ);

  edgePositions[0] = bx; edgePositions[1] = by; edgePositions[2] = bz;
  edgeColors[0] = fovR;  edgeColors[1] = fovG;  edgeColors[2] = fovB;
  edgePositions[3] = lx; edgePositions[4] = ly; edgePositions[5] = lz;
  edgeColors[3] = 0;     edgeColors[4] = 0;     edgeColors[5] = 0;

  edgePositions[6] = bx;  edgePositions[7] = by;  edgePositions[8] = bz;
  edgeColors[6] = fovR;   edgeColors[7] = fovG;   edgeColors[8] = fovB;
  edgePositions[9] = rx;  edgePositions[10] = ry; edgePositions[11] = rz;
  edgeColors[9] = 0;      edgeColors[10] = 0;     edgeColors[11] = 0;

  // 向前推进的柔和雷达距离脉冲弧（4m → 34m 循环扩散并自然淡出）
  const pulsePhase = (frame % PULSE_CYCLE_FRAMES) / PULSE_CYCLE_FRAMES;
  const pulseRadius = 4.0 + pulsePhase * 30.0;
  const pulseFade = (1.0 - pulsePhase * 0.88) * (0.50 + 0.50 * Math.min(1.0, pulsePhase * 4.0));

  for (let i = 0; i < SWEEP_SEGMENTS; i++) {
    const u0 = i / SWEEP_SEGMENTS;
    const u1 = (i + 1) / SWEEP_SEGMENTS;
    const t0 = sweepStart + (sweepEnd - sweepStart) * u0;
    const t1 = sweepStart + (sweepEnd - sweepStart) * u1;
    const [f0x, f0y] = forwardENU(t0);
    const [f1x, f1y] = forwardENU(t1);
    const [p0x, p0y, p0z] = worldToThree(bumperEnuX + f0x * pulseRadius, bumperEnuY + f0y * pulseRadius, sweepSurfaceZ);
    const [p1x, p1y, p1z] = worldToThree(bumperEnuX + f1x * pulseRadius, bumperEnuY + f1y * pulseRadius, sweepSurfaceZ);

    // 两侧边缘羽化，中间最清晰
    const edgeFeather0 = 1.0 - Math.abs(2.0 * u0 - 1.0) * 0.85;
    const edgeFeather1 = 1.0 - Math.abs(2.0 * u1 - 1.0) * 0.85;
    const a0 = pulseFade * edgeFeather0;
    const a1 = pulseFade * edgeFeather1;

    const base = (2 + i) * 6;
    edgePositions[base + 0] = p0x; edgePositions[base + 1] = p0y; edgePositions[base + 2] = p0z;
    edgeColors[base + 0] = fovR * a0; edgeColors[base + 1] = fovG * a0; edgeColors[base + 2] = fovB * a0;

    edgePositions[base + 3] = p1x; edgePositions[base + 4] = p1y; edgePositions[base + 5] = p1z;
    edgeColors[base + 3] = fovR * a1; edgeColors[base + 4] = fovG * a1; edgeColors[base + 5] = fovB * a1;
  }
}
