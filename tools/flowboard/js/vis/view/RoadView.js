/**
 * RoadView.js — 路面 ribbon + 车道线 + 路肩 + 弯道/匝道支持
 *
 * P1 画质升级：
 *   - 沥青 PBR：程序化 CanvasTexture 生成 albedo + normal map（颗粒感）
 *   - 车道线：轻微 emissive 反光 + 虚线 bug 修复
 *   - 路肩/路缘石：道路两侧各一条浅色窄带
 *   - 城市道路：路缘石、人行道和草地绿化带分层，避免建筑直接落在沥青旁
 *
 * P2 弯道/匝道（2026-08）：
 *   - 边缘类型感知：road/ramp_curve/viaduct_highway/urban
 *   - 弯道：CatmullRomCurve3 多点采样，平滑法线
 *   - 匝道：单行道、窄路面、无黄色中心线、汇入区虚线标识
 *   - 汇入区：匝道汇入主路时渲染渐变虚线
 *
 * P3 匝道过渡（2026-08）：
 * - 平行贴合主路的入口/出口自动识别，生成加速/减速车道
 * - 梯形并道边界和导流 V 线全部沿局部曲线构造
 * - 普通端点转弯不触发，避免在城市路口错误绘制高速导流区
 *
 * 车道线手法移植自 docs/scene.html（materials + polygonOffset 防 z-fight），
 * 但几何由 road_network 数据驱动（沿采样中心线按横向偏移铺设）。
 * road_network 变化时重建，ego 位姿变化不重建。
 */

import { sampleEdgeNodes, edgeSampleCount as adaptiveEdgeSampleCount } from '../math/Curve.js';
import { mergeGeometries } from '../math/GeometryMerge.js';
import { LANE_WIDTH, DEFAULT_LANES, EDGE_TYPE } from '../core/Constants.js';
import { tangentToNormal, offsetAlongNormal, forwardENU } from '../math/Coord.js';
import { detectJunctions } from './JunctionDetect.js';

const ASPHALT_COLOR = 0x2a2a2a;
const SHOULDER_COLOR = 0x5a5a55;
const CURB_COLOR = 0x9a9a92;
const SIDEWALK_COLOR = 0x777b78;
const VERGE_COLOR = 0x355d35;
const LINE_WHITE = 0xcccccc;
const LINE_YELLOW = 0xffd700;
const RAMP_COLOR = 0x353535;    // 匝道路面比主路略浅

const LINE_W = 0.15;      // 车道线宽度 (m)
const EDGE_LINE_W = 0.20; // 路缘边线宽度 (m)，比车道线略宽以区分路边界
const EDGE_INSET = 0.25;  // 边线相对路缘内缩 (m)
const Y_ROAD = 0.10;      // 路面高度
const Y_MARK = 0.13;      // 车道线高度（略高于路面防 z-fight）
const Y_EDGE = 0.14;      // 路缘边线高度，略高于车道线确保远距离可见
const Y_SHOULDER = 0.08;  // 路肩高度（略低于路面）
const Y_SIDEWALK = 0.09;
const Y_CURB = 0.15;
const Y_VERGE = 0.03;
const SHOULDER_W = 0.6;   // 路肩宽度 (m)
const CURB_W = 0.18;
const SIDEWALK_W = 2.4;
const VERGE_W = 3.2;
const DASH = 3.0;         // 虚线段长 (m)
const GAP = 6.0;          // 虚线间隔 (m)

/* 弯道/匝道采样密度：主路多点曲线用 32，匝道用 24，直道 2 点用 24 */
const SAMPLES_CURVE = 32;
const SAMPLES_RAMP = 24;
const SAMPLES_STRAIGHT = 24;

/* 匝道渲染参数 */
const RAMP_LANE_W = 3.0;   // 匝道车道宽度略窄 (m)
const RAMP_SHOULDER_W = 0.4; // 匝道路肩略窄
const TAPER_DEFAULT_M = 90.0;
const TAPER_MIN_M = 35.0;
const TAPER_MAX_M = 160.0;
const RAMP_JOIN_DISTANCE_M = 16.0;

// ═══════════════════════════════════════════════════════════
// 程序化沥青纹理（CanvasTexture，零外部资源）
// ═══════════════════════════════════════════════════════════

let _asphaltTex = null;
let _asphaltNormal = null;

function _buildAsphaltTextures() {
  if (_asphaltTex) return;
  // 在 Node.js 无头测试环境中，document 不可用，跳过纹理生成
  if (typeof document === 'undefined') return;

  const SIZE = 512;
  const canvas = document.createElement('canvas');
  canvas.width = SIZE; canvas.height = SIZE;
  const ctx = canvas.getContext('2d');

  // 基底：深灰沥青色
  ctx.fillStyle = '#2a2a2a';
  ctx.fillRect(0, 0, SIZE, SIZE);

  // 随机噪声颗粒（模拟沥青骨料）
  const imageData = ctx.getImageData(0, 0, SIZE, SIZE);
  const data = imageData.data;
  for (let i = 0; i < data.length; i += 4) {
    const noise = (Math.random() - 0.5) * 28;
    data[i]     = Math.max(0, Math.min(255, data[i] + noise));
    data[i + 1] = Math.max(0, Math.min(255, data[i + 1] + noise));
    data[i + 2] = Math.max(0, Math.min(255, data[i + 2] + noise));
  }
  ctx.putImageData(imageData, 0, 0);

  // 细纹裂缝（随机短线，模拟沥青路面微裂纹）
  ctx.strokeStyle = 'rgba(20,20,20,0.15)';
  ctx.lineWidth = 0.5;
  for (let i = 0; i < 80; i++) {
    const x = Math.random() * SIZE, y = Math.random() * SIZE;
    const len = 10 + Math.random() * 40;
    const angle = Math.random() * Math.PI;
    ctx.beginPath();
    ctx.moveTo(x, y);
    ctx.lineTo(x + Math.cos(angle) * len, y + Math.sin(angle) * len);
    ctx.stroke();
  }

  _asphaltTex = new THREE.CanvasTexture(canvas);
  _asphaltTex.wrapS = THREE.RepeatWrapping;
  _asphaltTex.wrapT = THREE.RepeatWrapping;
  _asphaltTex.repeat.set(8, 8);  // 8m 重复，匹配路面尺度
  _asphaltTex.colorSpace = THREE.SRGBColorSpace;

  // 法线贴图：从灰度高度图生成
  const normalCanvas = document.createElement('canvas');
  normalCanvas.width = SIZE; normalCanvas.height = SIZE;
  const nctx = normalCanvas.getContext('2d');
  nctx.drawImage(canvas, 0, 0);  // 复制噪声图
  const nImgData = nctx.getImageData(0, 0, SIZE, SIZE);
  const nd = nImgData.data;

  // Sobel 算子转法线
  const heightMap = new Float32Array(SIZE * SIZE);
  for (let i = 0; i < SIZE * SIZE; i++) {
    heightMap[i] = nd[i * 4] / 255;  // 灰度值
  }

  const out = new Uint8ClampedArray(SIZE * SIZE * 4);
  for (let y = 1; y < SIZE - 1; y++) {
    for (let x = 1; x < SIZE - 1; x++) {
      const idx = (y * SIZE + x);
      const tl = heightMap[(y - 1) * SIZE + (x - 1)];
      const t  = heightMap[(y - 1) * SIZE + x];
      const tr = heightMap[(y - 1) * SIZE + (x + 1)];
      const l  = heightMap[y * SIZE + (x - 1)];
      const r  = heightMap[y * SIZE + (x + 1)];
      const bl = heightMap[(y + 1) * SIZE + (x - 1)];
      const b  = heightMap[(y + 1) * SIZE + x];
      const br = heightMap[(y + 1) * SIZE + (x + 1)];

      const gx = (tr + 2 * r + br) - (tl + 2 * l + bl);
      const gy = (bl + 2 * b + br) - (tl + 2 * t + tr);
      const strength = 2.5;

      const nx = -gx * strength;
      const ny = -gy * strength;
      const nz = 1.0;
      const len = Math.sqrt(nx * nx + ny * ny + nz * nz);

      const pi = idx * 4;
      out[pi]     = Math.round(((nx / len) * 0.5 + 0.5) * 255);
      out[pi + 1] = Math.round(((ny / len) * 0.5 + 0.5) * 255);
      out[pi + 2] = Math.round(((nz / len) * 0.5 + 0.5) * 255);
      out[pi + 3] = 255;
    }
  }
  const normalImgData = new ImageData(out, SIZE, SIZE);
  nctx.putImageData(normalImgData, 0, 0);

  _asphaltNormal = new THREE.CanvasTexture(normalCanvas);
  _asphaltNormal.wrapS = THREE.RepeatWrapping;
  _asphaltNormal.wrapT = THREE.RepeatWrapping;
  _asphaltNormal.repeat.set(8, 8);
  _asphaltNormal.colorSpace = THREE.LinearSRGBColorSpace;
}

export function createRoadView(scene) {
  let roadGroup = new THREE.Group();
  scene.add(roadGroup);
  let built = false;
  let stats = { rampTransitions: 0 };
  let _junctionCenters = [];    // 当前路网的交叉口中心 [{x,z,radius}]
  let _junctionByEdge = new Map(); // edgeId -> {start:idx|null, end:idx|null}

  // 确保纹理已生成
  _buildAsphaltTextures();

  // ── 几何辅助 ──

  /** 从中心线样点（含法线）+ 半宽构建朝上的 ribbon 几何体。
   *  centers: [{px,py,pz,nx,nz}]，法线在 XZ 平面。yOff: 抬高量。 */
  function ribbonGeo(centers, halfW, yOff) {
    if (centers.length < 2) return null;
    const positions = [], indices = [], uvs = [];
    for (let k = 0; k < centers.length; k++) {
      const c = centers[k];
      positions.push(c.px + c.nx * halfW, c.py + yOff, c.pz + c.nz * halfW);
      positions.push(c.px - c.nx * halfW, c.py + yOff, c.pz - c.nz * halfW);
      uvs.push(0, k); uvs.push(1, k);
    }
    const vertCount = positions.length / 3;
    for (let i = 0; i < vertCount - 2; i += 2) {
      indices.push(i, i + 2, i + 1);
      indices.push(i + 1, i + 2, i + 3);
    }
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
    geo.setAttribute('uv', new THREE.Float32BufferAttribute(uvs, 2));
    geo.setIndex(indices);
    geo.computeVertexNormals();
    return geo;
  }

  /** 把中心线整体横向偏移 d（沿各点法线方向）。
   *  2026-08-14 曲率钳制：急弯处（局部曲率半径 R < |d|）偏移折线会回折自相交
   *  → 车道线/边线"打结"（OSM S 弯实测）。按每点局部曲率半径把偏移量钳到
   *  R - 余量，内侧偏移折线不再自相交。 */
  function offsetSpine(spine, d) {
    return spine.map((c, i) => {
      let off = d;
      if (i > 0 && i < spine.length - 1) {
        const a = spine[i - 1], b = spine[i + 1];
        const v1x = c.px - a.px, v1z = c.pz - a.pz;
        const v2x = b.px - c.px, v2z = b.pz - c.pz;
        const l1 = Math.hypot(v1x, v1z), l2 = Math.hypot(v2x, v2z);
        if (l1 > 1e-6 && l2 > 1e-6) {
          const cross = (v1x * v2z - v1z * v2x) / (l1 * l2);
          const dot = (v1x * v2x + v1z * v2z) / (l1 * l2);
          const ang = Math.atan2(cross, dot);   // exempt: 曲率钳制用有向转角，非坐标/朝向计算
          if (Math.abs(ang) > 1e-4) {
            const chord = (l1 + l2) * 0.5;
            const R = chord / (2 * Math.sin(Math.abs(ang) / 2));  // exempt: 局部曲率半径几何，非偏移手算
            // 对称钳制：内侧防自相交（打结），外侧急弯略收窄（可接受）。
            // 留 0.3m 余量防贴死。
            const maxOff = Math.max(0.3, R - 0.3);
            off = Math.max(-maxOff, Math.min(d, maxOff));
          }
        }
      }
      const [opx, , opz] = offsetAlongNormal(c.px, c.pz, c.nx, c.nz, off);
      return { px: opx, py: c.py, pz: opz, nx: c.nx, nz: c.nz };
    });
  }

  /** 构建 spine 的累计弧长数组 */
  function buildCumulative(spine) {
    const cum = [0];
    for (let i = 1; i < spine.length; i++) {
      cum.push(cum[i - 1] + Math.hypot(spine[i].px - spine[i - 1].px, spine[i].pz - spine[i - 1].pz));
    }
    return cum;
  }

  /** 沿 spine 按弧长比例插值样点 */
  function sampleSpineAt(spine, cum, s) {
    const total = cum[cum.length - 1];
    if (s <= 0) return spine[0];
    if (s >= total) return spine[spine.length - 1];
    let i = 1; while (i < cum.length && cum[i] < s) i++;
    const t = (s - cum[i - 1]) / ((cum[i] - cum[i - 1]) || 1);
    const a = spine[i - 1], b = spine[i];
    return {
      px: a.px + (b.px - a.px) * t, py: a.py + (b.py - a.py) * t,
      pz: a.pz + (b.pz - a.pz) * t, nx: a.nx + (b.nx - a.nx) * t,
      nz: a.nz + (b.nz - a.nz) * t,
    };
  }

  /** 实线：沿偏移中心线铺一条连续窄 ribbon */
  function solidLine(spine, d) {
    return ribbonGeo(offsetSpine(spine, d), LINE_W / 2, Y_MARK);
  }

  /** 路缘边线（实线，比车道线略宽略高，远距离可见） */
  function edgeLine(spine, d) {
    return ribbonGeo(offsetSpine(spine, d), EDGE_LINE_W / 2, Y_EDGE);
  }

  /** 虚线：沿偏移中心线按弧长 march，每 (DASH+GAP) 铺一段。
   *  2026-08 P1：整条虚线的所有段**合并成一个 BufferGeometry**（每段一个 quad，
   *  共享一个 geo）——旧实现每段各建一个 BufferGeometry，OSM 491 路 × 多虚线 ×
   *  每路几十段 ≈ 数万 geo + 数万次 computeVertexNormals，RoadView.build 占
   *  SceneDirector.update 87%（~3s）。合批后每条虚线 1 个 geo。 */
  function dashedLine(spine, d) {
    const centers = offsetSpine(spine, d);
    const cum = buildCumulative(centers);
    const total = cum[cum.length - 1];
    const positions = [], indices = [], uvs = [];
    const hw = LINE_W / 2;
    let quads = 0;
    for (let s = 0; s < total; s += DASH + GAP) {
      const end = Math.min(s + DASH, total);
      if (end - s < 0.1) continue;
      const a = sampleSpineAt(centers, cum, s);
      const b = sampleSpineAt(centers, cum, end);
      const nx = a.nx, nz = a.nz;   // 用起点的法线（段很短，法线变化可忽略）
      const base = positions.length / 3;
      positions.push(a.px + nx * hw, a.py + Y_MARK, a.pz + nz * hw);
      positions.push(a.px - nx * hw, a.py + Y_MARK, a.pz - nz * hw);
      positions.push(b.px + nx * hw, b.py + Y_MARK, b.pz + nz * hw);
      positions.push(b.px - nx * hw, b.py + Y_MARK, b.pz - nz * hw);
      uvs.push(0, 0, 1, 0, 0, 1, 1, 1);
      indices.push(base, base + 2, base + 1, base + 1, base + 2, base + 3);
      quads++;
    }
    if (quads === 0) return [];
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
    geo.setAttribute('uv', new THREE.Float32BufferAttribute(uvs, 2));
    geo.setIndex(indices);
    geo.computeVertexNormals();
    return [geo];
  }

  /** 在指定弧长范围 [rangeStart, rangeEnd) 内生成虚线（同样合批成单 geo） */
  function dashedLineInRange(spine, d, rangeStart, rangeEnd) {
    const centers = offsetSpine(spine, d);
    const cum = buildCumulative(centers);
    const total = cum[cum.length - 1];
    const rs = Math.max(0, rangeStart);
    const re = Math.min(total, rangeEnd);
    if (rs >= re) return [];

    const positions = [], indices = [], uvs = [];
    const hw = LINE_W / 2;
    const pattern = DASH + GAP;
    let quads = 0;
    let s = Math.floor(rs / pattern) * pattern;
    for (; s < re; s += pattern) {
      const end = Math.min(s + DASH, re);
      if (end - s < 0.1 || end <= rs) continue;
      const a = sampleSpineAt(centers, cum, Math.max(s, rs));
      const b = sampleSpineAt(centers, cum, end);
      const nx = a.nx, nz = a.nz;
      const base = positions.length / 3;
      positions.push(a.px + nx * hw, a.py + Y_MARK, a.pz + nz * hw);
      positions.push(a.px - nx * hw, a.py + Y_MARK, a.pz - nz * hw);
      positions.push(b.px + nx * hw, b.py + Y_MARK, b.pz + nz * hw);
      positions.push(b.px - nx * hw, b.py + Y_MARK, b.pz - nz * hw);
      uvs.push(0, 0, 1, 0, 0, 1, 1, 1);
      indices.push(base, base + 2, base + 1, base + 1, base + 2, base + 3);
      quads++;
    }
    if (quads === 0) return [];
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
    geo.setAttribute('uv', new THREE.Float32BufferAttribute(uvs, 2));
    geo.setIndex(indices);
    geo.computeVertexNormals();
    return [geo];
  }

  /** 车道从主路边缘向外展宽/收窄的梯形路面。 */
  function taperLaneGeo(spine, cum, startS, endS, side, edgeOffset, startWidth, endWidth) {
    const length = endS - startS;
    if (length < 1 || spine.length < 2) return null;
    const positions = [], indices = [], uvs = [];
    const count = Math.max(2, Math.ceil(length / 4) + 1);
    for (let i = 0; i < count; i++) {
      const t = i / (count - 1);
      const c = sampleSpineAt(spine, cum, startS + length * t);
      const width = startWidth + (endWidth - startWidth) * t;
      const inner = side * edgeOffset;
      const outer = side * (edgeOffset + Math.max(0.03, width));
      positions.push(
        c.px + c.nx * inner, c.py + Y_ROAD + 0.002, c.pz + c.nz * inner,
        c.px + c.nx * outer, c.py + Y_ROAD + 0.002, c.pz + c.nz * outer,
      );
      uvs.push(0, t, 1, t);
    }
    for (let i = 0; i < count - 1; i++) {
      const base = i * 2;
      indices.push(base, base + 2, base + 1, base + 1, base + 2, base + 3);
    }
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
    geo.setAttribute('uv', new THREE.Float32BufferAttribute(uvs, 2));
    geo.setIndex(indices);
    geo.computeVertexNormals();
    return geo;
  }

  function taperBoundaryGeo(spine, cum, startS, endS, side, edgeOffset, startWidth, endWidth) {
    const length = endS - startS;
    if (length < 1) return null;
    const count = Math.max(2, Math.ceil(length / 4) + 1);
    const centers = [];
    for (let i = 0; i < count; i++) {
      const t = i / (count - 1);
      const c = sampleSpineAt(spine, cum, startS + length * t);
      const width = startWidth + (endWidth - startWidth) * t;
      centers.push({
        px: c.px + c.nx * side * (edgeOffset + width),
        py: c.py,
        pz: c.pz + c.nz * side * (edgeOffset + width),
        nx: c.nx, nz: c.nz,
      });
    }
    return ribbonGeo(centers, LINE_W * 0.5, Y_MARK);
  }

  /** 在导流区画 V 形斜纹。它们与主路局部切线绑定，弯道上不会错位。 */
  function addGoreChevrons(geos, spine, cum, startS, endS, side, edgeOffset,
      startWidth, endWidth) {
    const length = endS - startS;
    const count = Math.min(7, Math.floor(length / 18));
    for (let i = 1; i <= count; i++) {
      const t = i / (count + 1);
      const width = startWidth + (endWidth - startWidth) * t;
      if (width < 1.0) continue;
      const c = sampleSpineAt(spine, cum, startS + length * t);
      const lateral = side * (edgeOffset + width * 0.55);
      const apex = { px: c.px + c.nx * lateral, py: c.py, pz: c.pz + c.nz * lateral, nx: c.nx, nz: c.nz };
      const back = 2.0;
      const spread = Math.min(1.25, width * 0.32);
      const left = {
        px: apex.px - c.nz * back + c.nx * side * spread,
        py: c.py, pz: apex.pz + c.nx * back + c.nz * side * spread,
        nx: c.nx, nz: c.nz,
      };
      const right = {
        px: apex.px - c.nz * back - c.nx * side * spread,
        py: c.py, pz: apex.pz + c.nx * back - c.nz * side * spread,
        nx: c.nx, nz: c.nz,
      };
      const leftGeo = ribbonGeo([left, apex], 0.11, Y_MARK);
      const rightGeo = ribbonGeo([right, apex], 0.11, Y_MARK);
      if (leftGeo) geos.push(leftGeo);
      if (rightGeo) geos.push(rightGeo);
    }
  }

  /** 从 edge nodes 构建 spine（中心线样点 + XZ 法线） */
  function buildSpine(points) {
    const spine = [];
    for (let i = 0; i < points.length; i += 3) {
      const px = points[i], py = points[i + 1], pz = points[i + 2];
      let tx = 1, tz = 0;
      if (i + 6 < points.length) { tx = points[i + 3] - px; tz = points[i + 5] - pz; }
      else if (i >= 3) { tx = px - points[i - 3]; tz = pz - points[i - 2]; }
      const [nx, nz] = tangentToNormal(tx, tz);
      spine.push({ px, py, pz, nx, nz });
    }
    return spine;
  }

  /** 确定 edge 的采样密度 */
  function edgeSampleCount(nodes, edgeType) {
    if (edgeType === EDGE_TYPE.RAMP_CURVE) return SAMPLES_RAMP;
    if (nodes.length > 2) return Math.max(SAMPLES_CURVE, adaptiveEdgeSampleCount(nodes));
    return SAMPLES_STRAIGHT;
  }

  // ── 匝道渲染 ──

  /** 渲染匝道：窄路面、无黄色中心线、白虚线边线 */
  function buildRamp(edge, nodes) {
    const points = sampleEdgeNodes(nodes, SAMPLES_RAMP);
    const spine = buildSpine(points);
    if (spine.length < 2) return {
      roadGeos: [], shoulderGeos: [], curbGeos: [], sidewalkGeos: [],
      vergeGeos: [], whiteGeos: [],
    };

    const laneWidth = edge.lane_width || RAMP_LANE_W;
    const rampLanes = Math.max(1, edge.lanes || 1);
    const hw = (rampLanes * laneWidth) / 2;

    // 匝道路面（略浅色）
    const road = ribbonGeo(spine, hw, Y_ROAD);

    // 路肩
    const shoulderW = RAMP_SHOULDER_W;
    const shoulderL = ribbonGeo(spine.map(c => ({
      px: c.px + c.nx * (hw + shoulderW * 0.5), py: c.py, pz: c.pz + c.nz * (hw + shoulderW * 0.5),
      nx: c.nx, nz: c.nz,
    })), shoulderW * 0.5, Y_SHOULDER);
    const shoulderR = ribbonGeo(spine.map(c => ({
      px: c.px - c.nx * (hw + shoulderW * 0.5), py: c.py, pz: c.pz - c.nz * (hw + shoulderW * 0.5),
      nx: c.nx, nz: c.nz,
    })), shoulderW * 0.5, Y_SHOULDER);

    const whiteGeos = [];
    // 匝道两侧路缘边线（白实线，与静态 invariant「外沿=实线」一致）
    const rampEdgeL = edgeLine(spine, hw - EDGE_INSET);
    if (rampEdgeL) whiteGeos.push(rampEdgeL);
    const rampEdgeR = edgeLine(spine, -(hw - EDGE_INSET));
    if (rampEdgeR) whiteGeos.push(rampEdgeR);

    // 多车道匝道内部车道分隔（白虚线）
    if (rampLanes > 1) {
      for (let k = 1; k < rampLanes; k++) {
        const d = -hw + k * laneWidth;
        for (const g of dashedLine(spine, d)) whiteGeos.push(g);
      }
    }

    return {
      roadGeos: [road],
      shoulderGeos: [shoulderL, shoulderR].filter(Boolean),
      curbGeos: [], sidewalkGeos: [], vergeGeos: [],
      whiteGeos, spine, cum: buildCumulative(spine),
    };
  }

  /** 在主路内部点识别与匝道平行的连接。端点交汇多为普通转弯，不能误画加速道。 */
  function findRampConnection(rampEdge, rampSpine, rampCum, main) {
    if (rampSpine.length < 2 || main.spine.length < 4) return null;
    const rampTotal = rampCum[rampCum.length - 1];
    const candidates = [
      { endpoint: 'start', point: rampSpine[0], adjacent: sampleSpineAt(rampSpine, rampCum, Math.min(10, rampTotal)) },
      { endpoint: 'end', point: rampSpine[rampSpine.length - 1], adjacent: sampleSpineAt(rampSpine, rampCum, Math.max(0, rampTotal - 10)) },
    ];
    let best = null;
    for (const candidate of candidates) {
      for (let i = 1; i < main.spine.length - 1; i++) {
        const c = main.spine[i];
        const distance = Math.hypot(c.px - candidate.point.px, c.pz - candidate.point.pz);
        if (!best || distance < best.distance) best = { ...candidate, distance, index: i, mainPoint: c };
      }
    }
    if (!best || best.distance > RAMP_JOIN_DISTANCE_M) return null;
    const mainTotal = main.cum[main.cum.length - 1];
    const joinS = main.cum[best.index];
    if (joinS < TAPER_MIN_M || joinS > mainTotal - TAPER_MIN_M) return null;

    const rampForward = best.endpoint === 'end'
      ? { x: best.point.px - best.adjacent.px, z: best.point.pz - best.adjacent.pz }
      : { x: best.adjacent.px - best.point.px, z: best.adjacent.pz - best.point.pz };
    const next = main.spine[Math.min(best.index + 1, main.spine.length - 1)];
    const prev = main.spine[Math.max(best.index - 1, 0)];
    const mainForward = { x: next.px - prev.px, z: next.pz - prev.pz };
    const rampLength = Math.hypot(rampForward.x, rampForward.z) || 1;
    const mainLength = Math.hypot(mainForward.x, mainForward.z) || 1;
    const alignment = (rampForward.x * mainForward.x + rampForward.z * mainForward.z) /
      (rampLength * mainLength);
    if (alignment < 0.65) return null;

    const explicitRole = String(rampEdge.ramp_role || rampEdge.layout || '').toLowerCase();
    const role = explicitRole === 'diverge' || explicitRole === 'exit' ? 'diverge'
      : explicitRole === 'merge' || explicitRole === 'entry' ? 'merge'
      : best.endpoint === 'end' ? 'merge' : 'diverge';
    const sideVector = {
      x: best.adjacent.px - best.mainPoint.px,
      z: best.adjacent.pz - best.mainPoint.pz,
    };
    const side = sideVector.x * best.mainPoint.nx + sideVector.z * best.mainPoint.nz < 0 ? -1 : 1;
    return { role, side, joinS };
  }

  /** 构建加速/减速车道、渐变并道线与导流 V 线。 */
  function buildRampTransition(rampEdge, rampSpine, rampCum, main) {
    const connection = findRampConnection(rampEdge, rampSpine, rampCum, main);
    if (!connection) return { roadGeos: [], whiteGeos: [] };
    const mainTotal = main.cum[main.cum.length - 1];
    const requested = Number(rampEdge.taper_length_m ?? rampEdge.accel_length_m ??
      rampEdge.decel_length_m ?? TAPER_DEFAULT_M);
    const taperLength = Math.max(TAPER_MIN_M, Math.min(TAPER_MAX_M,
      Number.isFinite(requested) ? requested : TAPER_DEFAULT_M));
    const startS = connection.role === 'merge'
      ? Math.max(0, connection.joinS - taperLength) : connection.joinS;
    const endS = connection.role === 'merge'
      ? connection.joinS : Math.min(mainTotal, connection.joinS + taperLength);
    if (endS - startS < TAPER_MIN_M) return { roadGeos: [], whiteGeos: [] };

    const laneWidth = Number(main.edge.lane_width) || LANE_WIDTH;
    const edgeOffset = (Number(main.edge.lanes) || DEFAULT_LANES) * laneWidth * 0.5;
    const startWidth = connection.role === 'merge' ? laneWidth : 0;
    const endWidth = connection.role === 'merge' ? 0 : laneWidth;
    const road = taperLaneGeo(main.spine, main.cum, startS, endS, connection.side,
      edgeOffset, startWidth, endWidth);
    const boundary = taperBoundaryGeo(main.spine, main.cum, startS, endS, connection.side,
      edgeOffset, startWidth, endWidth);
    const whiteGeos = boundary ? [boundary] : [];
    addGoreChevrons(whiteGeos, main.spine, main.cum, startS, endS, connection.side,
      edgeOffset, startWidth, endWidth);
    return { roadGeos: road ? [road] : [], whiteGeos };
  }

  /** 从 edge nodes 构建主路路面 + 车道线 + 路肩 */
  function buildStandardRoad(edge, nodes) {
    const sampleCount = edgeSampleCount(nodes, edge.type);
    const points = sampleEdgeNodes(nodes, sampleCount);
    /* 2026-08-14 路口一劳永逸：路面 ribbon 永不截断（贯穿路口，相邻路自然
     * 重叠覆盖，沥青色一致看不出缝）；标线/路肩/人行道/绿化带在路口边界停
     * （简单距离过滤：spine 点到最近 junction 中心 > radius 才画），不再依赖
     * 精确弧段裁剪——截断半径与 patch 对不齐的"烂路口"和裁剪退化导致的
     * 打结都消失。 */
    const spine = buildSpine(points);
    if (spine.length < 2) {
      return {
        roadGeos: [], shoulderGeos: [], curbGeos: [], sidewalkGeos: [],
        vergeGeos: [], whiteGeos: [], yellowGeos: [], spine, cum: [],
      };
    }
    const markSpine = filterSpineOutsideJunctions(spine, edge.id);

    const lanes = edge.lanes || DEFAULT_LANES;
    const laneWidth = edge.lane_width || LANE_WIDTH;
    const hw = (lanes * laneWidth) / 2;

    // 路面（不截断，贯穿路口）
    const road = ribbonGeo(spine, hw, Y_ROAD);

    // 路肩（路口边界停，逐段）
    const shoulderGeos = [];
    for (const seg of markSpine) {
      const sL = ribbonGeo(offsetSpine(seg, hw + SHOULDER_W * 0.5), SHOULDER_W * 0.5, Y_SHOULDER);
      const sR = ribbonGeo(offsetSpine(seg, -(hw + SHOULDER_W * 0.5)), SHOULDER_W * 0.5, Y_SHOULDER);
      if (sL) shoulderGeos.push(sL);
      if (sR) shoulderGeos.push(sR);
    }

    const whiteGeos = [];
    const yellowGeos = [];

    // 路缘边线（白实线）：路缘内缩。外沿=实线（真路约定 + 静态 invariant
    // 「外沿=实线」一致）；虚线只用于同向车道分隔。边线比车道线宽（0.20m vs
    // 0.15m）且略高（0.14m vs 0.13m），远距离可见，与车道分隔虚线视觉区分明显。
    for (const seg of markSpine) {
      const edgeL = edgeLine(seg, hw - EDGE_INSET);
      if (edgeL) whiteGeos.push(edgeL);
      const edgeR = edgeLine(seg, -(hw - EDGE_INSET));
      if (edgeR) whiteGeos.push(edgeR);
    }

    // 标线按 GB 5768 语义生成（与 extract_city_map._markings / json_to_xodr
    // 一致，单一真相）：对向分隔=双黄实线；同向车道分隔=白色虚线；外缘白实线
    // 由上方 edgeLine 负责。中心线位置取 k===nPerSide（forward 车道数），对奇数
    // 车道数（如 2+1 不对称）同样能正确落在两向分界，不再因 lanes%2!=0 丢中心线。
    const oneway = edge.oneway === true;
    const nPerSide = oneway ? lanes : Math.max(1, Math.floor(lanes / 2));
    for (const seg of markSpine) {
      for (let k = 1; k < lanes; k++) {
        const d = -hw + k * laneWidth;
        const isCenter = !oneway && k === nPerSide;
        if (isCenter) {
          const CENTER_GAP = 0.12;
          const left = solidLine(seg, d - CENTER_GAP);
          const right = solidLine(seg, d + CENTER_GAP);
          if (left) yellowGeos.push(left);
          if (right) yellowGeos.push(right);
        } else {
          for (const g of dashedLine(seg, d)) whiteGeos.push(g);
        }
      }
    }

    const curbGeos = [];
    const sidewalkGeos = [];
    const vergeGeos = [];
    const isUrban = edge.type === EDGE_TYPE.URBAN ||
      String(edge.name || '').toLowerCase().includes('urban');

    if (isUrban) {
      for (const seg of markSpine) {
        for (const side of [-1, 1]) {
          const curb = ribbonGeo(offsetSpine(seg, side * (hw + SHOULDER_W + CURB_W * 0.5)),
            CURB_W * 0.5, Y_CURB);
          const sidewalk = ribbonGeo(offsetSpine(seg,
            side * (hw + SHOULDER_W + CURB_W + SIDEWALK_W * 0.5)),
          SIDEWALK_W * 0.5, Y_SIDEWALK);
          const verge = ribbonGeo(offsetSpine(seg,
            side * (hw + SHOULDER_W + CURB_W + SIDEWALK_W + VERGE_W * 0.5)),
          VERGE_W * 0.5, Y_VERGE);
          if (curb) curbGeos.push(curb);
          if (sidewalk) sidewalkGeos.push(sidewalk);
          if (verge) vergeGeos.push(verge);
        }
      }
    }

    return {
      roadGeos: [road], shoulderGeos, curbGeos, sidewalkGeos, vergeGeos,
      whiteGeos, yellowGeos, spine, cum: buildCumulative(spine),
    };
  }

  /** 路口边界停的简单距离过滤：把 spine 按「到最近 junction 中心 > radius」
   *  拆成若干连续段（路口内不画标线/侧向元素），返回段数组（每段 ≥2 点）。
   *  路面不截断（贯穿路口），只有标线/侧向元素用这些段，避免中心线连桥穿路口。 */
  function filterSpineOutsideJunctions(spine, edgeId) {
    const entry = _junctionByEdge.get(String(edgeId));
    if (!entry) return [spine];
    const js = [];
    if (entry.start != null) js.push(_junctionCenters[entry.start]);
    if (entry.end != null) js.push(_junctionCenters[entry.end]);
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
    return segs.length ? segs : [];
  }

  /** 从 edge nodes 构建（兼容三种 edge 格式） */
  function parseNodes(edge) {
    let nodes = edge.nodes;
    if (!nodes || nodes.length < 2) {
      const len = edge.length_m || 100;
      const h = edge.heading || 0;
      const sx = edge.start_x || 0, sz = edge.start_z || 0;
      const [fex, fey] = forwardENU(h);
      nodes = [[sx, sz, 0], [sx + fex * len, sz + fey * len, 0]];
    } else if (nodes[0] && typeof nodes[0] === 'object' && !Array.isArray(nodes[0])) {
      nodes = nodes.map(n => [n.x || 0, n.y || 0, n.z || 0]);
    }
    return nodes;
  }

  /** 检测 edge 是否为匝道。
   *  2026-08 P1：**不要用 oneway 判匝道**——oneway 是单行道（城市主干道普遍），
   *  不是匝道。旧判断含 `edge.oneway === true`，导致 OSM 330/491 条单行道被
   *  误判为匝道：① 主路画成匝道样式（无双黄中心线/城市路肩）；② build() 第二遍
   *  匝道循环 ramp×main 全组合跑 findRampConnection（330×161 次 O(n) 点扫）→
   *  RoadView.build ~3s 卡顿首帧。匝道只由 type=ramp_curve 或 name 含 ramp 判定。 */
  function isRampEdge(edge) {
    return edge.type === EDGE_TYPE.RAMP_CURVE
      || (edge.name && edge.name.indexOf('ramp') !== -1);
  }

  /** 从 road_network 构建全部路面 + 车道线 + 路肩 */
  function build(roadNetwork) {
    while (roadGroup.children.length) {
      const c = roadGroup.children[0];
      roadGroup.remove(c);
      if (c.geometry) c.geometry.dispose();
      if (c.material) c.material.dispose();
    }
    built = false;
    stats = { rampTransitions: 0, doubleYellowCenterlines: 0 };

    if (!roadNetwork || !roadNetwork.edges || roadNetwork.edges.length === 0) return;

    // 交叉口检测：先算一次，供所有 edge 收口使用
    const junctions = detectJunctions(roadNetwork);
    _junctionCenters = junctions.centers;
    _junctionByEdge = junctions.byId;

    const roadGeos = [];
    const shoulderGeos = [];
    const curbGeos = [];
    const sidewalkGeos = [];
    const vergeGeos = [];
    const whiteLineGeos = [];
    const yellowLineGeos = [];
    const rampGeos = [];

    // 收集所有主路 edge 的 spine（用于汇入区检测）
    const mainRoadSpines = [];

    // 第一遍：主路渲染
    for (const edge of roadNetwork.edges) {
      if (isRampEdge(edge)) continue;
      const nodes = parseNodes(edge);
      const result = buildStandardRoad(edge, nodes);
      for (const g of result.roadGeos) roadGeos.push(g);
      for (const g of result.shoulderGeos) shoulderGeos.push(g);
      for (const g of result.curbGeos) curbGeos.push(g);
      for (const g of result.sidewalkGeos) sidewalkGeos.push(g);
      for (const g of result.vergeGeos) vergeGeos.push(g);
      for (const g of result.whiteGeos) whiteLineGeos.push(g);
      for (const g of result.yellowGeos) yellowLineGeos.push(g);
      if (result.yellowGeos.length === 2) stats.doubleYellowCenterlines++;
      if (result.spine.length >= 2) {
        mainRoadSpines.push({ edge, spine: result.spine, cum: result.cum });
      }
    }

    // 第二遍：匝道渲染
    for (const edge of roadNetwork.edges) {
      if (!isRampEdge(edge)) continue;
      const nodes = parseNodes(edge);
      const result = buildRamp(edge, nodes);
      for (const g of result.roadGeos) rampGeos.push(g);
      for (const g of result.shoulderGeos) shoulderGeos.push(g);
      for (const g of result.whiteGeos) whiteLineGeos.push(g);

      // 在几何上真正贴合主路的平行匝道上，补齐加速/减速车道、
      // 渐变并道线和导流 V 线；普通端点转弯不会误触发。
      if (result.spine && result.spine.length >= 2) {
        for (const ms of mainRoadSpines) {
          const transition = buildRampTransition(edge, result.spine, result.cum, ms);
          for (const g of transition.roadGeos) roadGeos.push(g);
          if (transition.roadGeos.length) stats.rampTransitions++;
          for (const g of transition.whiteGeos) {
            whiteLineGeos.push(g);
          }
        }
      }
    }

    // ── 合并 + 上材质 ──

    // 主路路面：沥青 PBR
    if (roadGeos.length) {
      const mat = new THREE.MeshStandardMaterial({
        color: ASPHALT_COLOR,
        map: _asphaltTex,
        normalMap: _asphaltNormal,
        normalScale: new THREE.Vector2(0.4, 0.4),
        roughness: 0.88,
        metalness: 0.02,
        side: THREE.DoubleSide,
      });
      const mesh = new THREE.Mesh(mergeGeometries(roadGeos), mat);
      mesh.receiveShadow = true;
      roadGroup.add(mesh);
    }

    // 匝道路面（略浅色）
    if (rampGeos.length) {
      const mat = new THREE.MeshStandardMaterial({
        color: RAMP_COLOR,
        map: _asphaltTex,
        normalMap: _asphaltNormal,
        normalScale: new THREE.Vector2(0.4, 0.4),
        roughness: 0.85,
        metalness: 0.02,
        side: THREE.DoubleSide,
      });
      const mesh = new THREE.Mesh(mergeGeometries(rampGeos), mat);
      mesh.receiveShadow = true;
      roadGroup.add(mesh);
    }

    // 路肩
    if (shoulderGeos.length) {
      const mat = new THREE.MeshStandardMaterial({
        color: SHOULDER_COLOR,
        roughness: 0.85,
        metalness: 0.05,
        side: THREE.DoubleSide,
      });
      const mesh = new THREE.Mesh(mergeGeometries(shoulderGeos), mat);
      mesh.receiveShadow = true;
      roadGroup.add(mesh);
    }

    // 城市路缘石、人行道和绿化带分别合批，静态路网不随帧更新。
    const addSurface = function(geometries, color, roughness) {
      if (!geometries.length) return;
      const mat = new THREE.MeshStandardMaterial({
        color, roughness, metalness: 0, side: THREE.DoubleSide,
      });
      const mesh = new THREE.Mesh(mergeGeometries(geometries), mat);
      mesh.receiveShadow = true;
      roadGroup.add(mesh);
    };
    addSurface(curbGeos, CURB_COLOR, 0.78);
    addSurface(sidewalkGeos, SIDEWALK_COLOR, 0.92);
    addSurface(vergeGeos, VERGE_COLOR, 1.0);

    // 白色车道线
    if (whiteLineGeos.length) {
      const mat = new THREE.MeshStandardMaterial({
        color: LINE_WHITE,
        roughness: 0.6,
        metalness: 0.05,
        side: THREE.DoubleSide,
        polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -2,
      });
      roadGroup.add(new THREE.Mesh(mergeGeometries(whiteLineGeos), mat));
    }

    // 黄色中心线
    if (yellowLineGeos.length) {
      const mat = new THREE.MeshStandardMaterial({
        color: LINE_YELLOW,
        roughness: 0.6,
        metalness: 0.05,
        side: THREE.DoubleSide,
        polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -2,
      });
      roadGroup.add(new THREE.Mesh(mergeGeometries(yellowLineGeos), mat));
    }

    built = true;
  }

  function getRoadGroup() { return roadGroup; }
  function isBuilt() { return built; }
  function getStats() { return { ...stats }; }

  return { build, getRoadGroup, isBuilt, getStats };
}