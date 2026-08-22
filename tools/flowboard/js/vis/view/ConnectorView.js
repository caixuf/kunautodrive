/**
 * ConnectorView.js — 路段拼接连接件
 *
 * 解决多段 edge 直接拼接的视觉断层：
 *   1. LaneTaper    — 车道数/宽度变化的锥形过渡
 *   2. JunctionCap  — 路口两端的路口区域补齐
 *   3. RampMerge    — 匝道汇入主路的喇叭口导流线
 *   4. ViaductPier  — 高架段桥墩支撑
 *   5. BarrierEndCap— 护栏端头防撞桶
 *
 * 全部从 road_network.edges 元数据自动派生，零配置。
 * 连接件 mesh 加到 connectorGroup，road_network 变化时整体重建。
 */

import { getBox, getCylinder, getStdMaterial } from '../core/AssetFactory.js';
import { LANE_WIDTH, DEFAULT_LANES, isTunnelEdge } from '../core/Constants.js';
import { worldToThree, forwardENU, directionToRotationY } from '../math/Coord.js';
import { getTopology, walkFromJunction } from '../model/TopologyModel.js';
import { mergeGeometries } from '../math/GeometryMerge.js';
import { SCENE } from '../theme/tokens.js';

/* 配色全部来自 theme/tokens.js（P3 设计 token 单一事实源）。
 * TAPER/JUNCTION 必须与 RoadView 路面同色（无缝），所以共享 SCENE.asphalt。 */
const TAPER_COLOR  = SCENE.asphalt;   // 锥形过渡路面（同沥青色）
const JUNCTION_COLOR = SCENE.asphalt; // 路口区域与路面同色（2026-08-14 无缝，不再像补丁块）
const CROSSWALK_COLOR = SCENE.crosswalk; // 斑马线白
const MERGE_LINE_COLOR = SCENE.guideLine; // 导流线白
const ARROW_COLOR      = SCENE.lineWhite; // 地面导向箭头与菱形白
const PIER_COLOR   = SCENE.pier;      // 桥墩灰
const BARREL_RED   = SCENE.barrelRed;   // 防撞桶红
const BARREL_WHITE = SCENE.barrelWhite;   // 防撞桶白
const CROSSWALK_STRIPE_W = 0.5;  // 条带宽（跨道路方向）
const CROSSWALK_GAP = 0.5;       // 条带间距（跨道路方向）
const CROSSWALK_LENGTH = 2.5;    // 条纹沿道路长度（GB 5768.3 交叉口 ≥2m）
const STOP_LINE_W = 0.45;        // 停止线宽（垂直于行车的横条）
const PATCH_Y = 0.105;           // 路口铺装略高于路面防 z-fight
const CROSS_Y = 0.16;            // 斑马线高度
const STOP_Y = 0.17;             // 停止线高度（比斑马线再高一点防 z-fight）
// 断头判定容差：两条 road 相接的接缝（连续连接/弯道转折/路口汇聚）端点几乎
// 重合（OSM 段首尾精确同点），只有无任何邻接端点的孤端才是真断头。1.5m 既
// 能覆盖 OSM 聚类 gap，又不会把相邻道路端点（远大于 1.5m）误判成接缝。
const DEAD_END_TOL_M = 1.5;

export function createConnectorView(scene) {
  const group = new THREE.Group();
  scene.add(group);
  let built = false;

  /** 清空连接件 */
  function clear() {
    while (group.children.length) {
      const c = group.children[0];
      group.remove(c);
      if (c.geometry) c.geometry.dispose();
    }
    built = false;
  }

  /** 主构建入口：扫描 edges，自动派生连接件 */
  function build(roadNetwork) {
    clear();
    if (!roadNetwork || !roadNetwork.edges) return;

    const edges = roadNetwork.edges;
    /* fork/merge 转向记录：优先 map_junctions（map.json 原样透传），
     * 其次 junctions（map_compiler 生成）。两者都是 fork 契约格式。 */
    const junctions = roadNetwork.map_junctions || roadNetwork.junctions || [];

    // 拓扑（单一事实源）：路口中心 / edge↔路口映射，供 LaneTaper 邻接与
    // JunctionPatch 共用。须先于 1. LaneTaper 计算（旧实现依赖 id 连续，现用拓扑）。
    const topo = getTopology(roadNetwork);
    const centers = topo ? topo.centers : [];
    const byId = topo ? topo.byId : new Map();

    // ── 1. LaneTaper：相邻 edge 车道数/宽度不同 ──
    // 邻接判定用拓扑（edge 末端与另一 edge 始端落在同一路口中心），不再依赖
    // id 连续（id 连续只是巧合，真实过渡多在路口/弯道处 id 不连续，旧实现漏判）。
    const edgeMap = new Map(edges.map((e) => [String(e.id), e]));
    const endEdges = new Map();   // centerIdx -> [在此结束的 edgeId]
    if (topo) {
      for (const e of edges) {
        const entry = topo.byId.get(String(e.id));
        if (entry && entry.end != null) {
          if (!endEdges.has(entry.end)) endEdges.set(entry.end, []);
          endEdges.get(entry.end).push(String(e.id));
        }
      }
    }
    for (const e of edges) {
      const entry = topo ? topo.byId.get(String(e.id)) : null;
      if (!entry || entry.start == null) continue;
      const a = e;
      const widthA = (a.lanes || DEFAULT_LANES) * (a.lane_width || LANE_WIDTH);
      for (const bId of (endEdges.get(entry.start) || [])) {
        if (bId === String(a.id)) continue;
        const b = edgeMap.get(bId);
        if (!b) continue;
        const widthB = (b.lanes || DEFAULT_LANES) * (b.lane_width || LANE_WIDTH);
        if (Math.abs(widthA - widthB) < 0.1) continue;
        _buildLaneTaper(a, b, widthA, widthB, topo, entry.start);
      }
    }

    // ── 2. JunctionCap 已废弃（2026-08-14）：路面 ribbon 不再截断，intersection
    //  类型 edge 的路面由 RoadView 贯穿渲染覆盖；旧 cap 硬编码 +X 方向与数据驱动
    //  的 _buildJunctionPatch 重叠渲染且方向错，删除。 ──

    // ── 2b. 交叉口路面补齐：在每个检测到的路口中心铺一块**真实路口多边形**
    //  沥青路面（比路宽大一点，覆盖四向收口后的缺口）+ 四个方向斑马线。
    //  laneData 供 connectingRoads 解析内部 connector → 真实 edge 的转向路径。 ──
    _buildJunctionPatch(topo, centers, junctions, roadNetwork.lane_data || null);

    // ── 2c. 转向连接曲线已删除（2026-08-15，P1 渠化替代）：旧实现按 fork turn 画
    //  彩色 ribbon，但 incoming_road 是 base 路名、byId 键是分段 edge id，键型
    //  不匹配导致其从未渲染过任何东西（死代码）；正确的数据版 = 2b 内的
    //  转向引导虚线（connectingRoads 前缀匹配 + 二次贝塞尔导流线）。 ──

    // ── 3. RampMerge：ramp_curve 类型 + junctions 里有 merge ──
    const mergeJunctions = junctions.filter(j => j.type === 'merge');
    for (const edge of edges) {
      if (edge.type !== 'ramp_curve') continue;
      // 查找该 ramp 对应的 merge junction
      const mj = mergeJunctions.find(j => j.incoming_road === edge.id);
      if (mj) {
        const targetEdge = edges.find(e => e.id === mj.target_road);
        if (targetEdge) _buildRampMerge(edge, targetEdge);
      }
    }

    // ── 4. ViaductPier：高架（bridge/z>=1.5）edge 自适应多柱桥墩与盖梁收集 ──
    const pierInstances = [];
    const capInstances = [];
    for (const edge of edges) {
      const nodes = Array.isArray(edge.nodes) ? edge.nodes : [];
      const maxZ = nodes.reduce((m, n) => Math.max(m, Array.isArray(n) ? (n[2] || 0) : (n.z || 0)), 0);
      if (!edge.bridge && maxZ < 1.5) continue;
      _collectViaductPiers(edge, pierInstances, capInstances);
    }
    if (pierInstances.length > 0) {
      const pierGeo = getCylinder(0.6, 0.75, 1);
      const pierMat = getStdMaterial(PIER_COLOR, 0.9, 0.0);
      const pierMesh = new THREE.InstancedMesh(pierGeo, pierMat, pierInstances.length);
      const dummy = new THREE.Object3D();
      pierInstances.forEach((p, i) => {
        dummy.position.set(p.tx, p.h / 2, p.tz);
        dummy.scale.set(p.radius || 1, p.h, p.radius || 1);
        dummy.updateMatrix();
        pierMesh.setMatrixAt(i, dummy.matrix);
      });
      pierMesh.instanceMatrix.needsUpdate = true;
      pierMesh.frustumCulled = true;
      group.add(pierMesh);
    }
    if (capInstances.length > 0) {
      const capGeo = getBox(1, 1, 1);
      const capMat = getStdMaterial(PIER_COLOR, 0.92, 0.0);
      const capMesh = new THREE.InstancedMesh(capGeo, capMat, capInstances.length);
      const dummyCap = new THREE.Object3D();
      capInstances.forEach((c, i) => {
        dummyCap.position.set(c.tx, c.ty, c.tz);
        dummyCap.rotation.y = c.rotY;
        dummyCap.scale.set(c.len, c.h, c.w);
        dummyCap.updateMatrix();
        capMesh.setMatrixAt(i, dummyCap.matrix);
      });
      capMesh.instanceMatrix.needsUpdate = true;
      capMesh.frustumCulled = true;
      group.add(capMesh);
    }

    // ── 4b. BridgeRamp：桥隧端点坡道渐变过渡（消除 6m 悬崖） ──
    const rampGeos = [];
    for (const edge of edges) {
      const nodes = Array.isArray(edge.nodes) ? edge.nodes : [];
      const maxZ = nodes.reduce((m, n) => Math.max(m, Array.isArray(n) ? (n[2] || 0) : (n.z || 0)), 0);
      if (!edge.bridge && !isTunnelEdge(edge) && maxZ < 1.5) continue;
      _collectBridgeRamps(edge, rampGeos);
    }
    if (rampGeos.length > 0) {
      const mergedRampGeo = mergeGeometries(rampGeos);
      if (mergedRampGeo) {
        const rampMat = getStdMaterial(TAPER_COLOR, 0.92, 0.0);
        const rampMesh = new THREE.Mesh(mergedRampGeo, rampMat);
        rampMesh.frustumCulled = true;
        group.add(rampMesh);
      }
      for (const g of rampGeos) g.dispose();
    }

    // ── 5. BarrierEndCap：只在**真断头端点**（孤端）放防撞桶 ──
    const endpointLocs = [];
    for (const e of edges) {
      if (!Array.isArray(e.nodes) || e.nodes.length < 2) continue;
      const ns = e.nodes.map((n) =>
        Array.isArray(n) ? n : [n.x || 0, n.y || 0, n.z || 0]);
      const [sx, , sz] = worldToThree(ns[0][0] || 0, ns[0][1] || 0, ns[0][2] || 0);
      const [ex, , ez] = worldToThree(
        ns[ns.length - 1][0] || 0, ns[ns.length - 1][1] || 0, ns[ns.length - 1][2] || 0);
      endpointLocs.push({ x: sx, z: sz, id: String(e.id) });
      endpointLocs.push({ x: ex, z: ez, id: String(e.id) });
    }
    const EP_CELL = 8;
    const endpointGrid = new Map();
    for (const ep of endpointLocs) {
      const k = `${Math.floor(ep.x / EP_CELL)},${Math.floor(ep.z / EP_CELL)}`;
      let bucket = endpointGrid.get(k);
      if (!bucket) { bucket = []; endpointGrid.set(k, bucket); }
      bucket.push(ep);
    }
    const barrelLocs = [];
    for (const edge of edges) {
      if (edge.type === 'ramp_curve' || edge.type === 'intersection') continue;
      _collectBarrierEndCaps(edge, byId, endpointLocs, endpointGrid, EP_CELL, barrelLocs);
    }
    if (barrelLocs.length > 0) {
      const redGeo = getCylinder(0.3, 0.35, 0.4);
      const redMat = getStdMaterial(BARREL_RED, 0.6, 0.1);
      const redMesh = new THREE.InstancedMesh(redGeo, redMat, barrelLocs.length);
      const dummyRed = new THREE.Object3D();
      barrelLocs.forEach((loc, i) => {
        dummyRed.position.set(loc.x, 0.3, loc.z);
        dummyRed.updateMatrix();
        redMesh.setMatrixAt(i, dummyRed.matrix);
      });
      redMesh.instanceMatrix.needsUpdate = true;
      redMesh.frustumCulled = true;
      group.add(redMesh);

      const whiteGeo = getCylinder(0.3, 0.3, 0.3);
      const whiteMat = getStdMaterial(BARREL_WHITE, 0.6, 0.1);
      const whiteMesh = new THREE.InstancedMesh(whiteGeo, whiteMat, barrelLocs.length);
      const dummyWhite = new THREE.Object3D();
      barrelLocs.forEach((loc, i) => {
        dummyWhite.position.set(loc.x, 0.75, loc.z);
        dummyWhite.updateMatrix();
        whiteMesh.setMatrixAt(i, dummyWhite.matrix);
      });
      whiteMesh.instanceMatrix.needsUpdate = true;
      whiteMesh.frustumCulled = true;
      group.add(whiteMesh);
    }

    built = true;
  }

  /** 1. LaneTaper：锥形过渡路面
   *  在 edgeA→edgeB 的接缝（拓扑路口中心）生成一个梯形（宽 widthA → widthB），
   *  长 5m，沿"进入→离开"的贯穿方向铺设（弯道/路口也贴方向，不假设沿 +X 直伸）。 */
  function _buildLaneTaper(edgeA, edgeB, widthA, widthB, topo, centerIdx) {
    // 接缝位置：优先路口中心（拓扑），否则 edgeA 末端
    let cx, cz, dirx, dirz;
    const ca = topo && topo.centers && topo.centers[centerIdx];
    if (ca) {
      cx = ca.x; cz = ca.z;
      // 贯穿方向 = 进入中心方向（a 末段切线）+ 离开中心方向（b 首段切线）平均
      const ta = _edgeDir(edgeA, false);
      const tb = _edgeDir(edgeB, true);
      dirx = ta.x + tb.x; dirz = ta.z + tb.z;
      const l = Math.hypot(dirx, dirz) || 1; dirx /= l; dirz /= l;
    } else {
      const pos = _getEdgeEnd(edgeA);
      if (!pos) return;
      cx = pos.x; cz = pos.z; dirx = 1; dirz = 0;
    }

    const taperLen = 5;
    const hA = widthA / 2, hB = widthB / 2;
    const nx = -dirz, nz = dirx;   // 沿行驶方向的左法线（横向偏移）
    const back = -taperLen / 2, front = taperLen / 2;
    // 梯形顶点：front（下游 edgeB 侧）宽 widthB，back（上游 edgeA 侧）宽 widthA
    const positions = [
      cx + dirx * back + nx * hA, 0.11, cz + dirz * back + nz * hA,   // 左后
      cx + dirx * back - nx * hA, 0.11, cz + dirz * back - nz * hA,   // 右后
      cx + dirx * front + nx * hB, 0.11, cz + dirz * front + nz * hB, // 左前
      cx + dirx * front - nx * hB, 0.11, cz + dirz * front - nz * hB, // 右前
    ];
    const indices = [0, 1, 2, 1, 3, 2];
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
    geo.setIndex(indices);
    geo.computeVertexNormals();
    const mat = getStdMaterial(TAPER_COLOR, 0.92, 0.0);
    const mesh = new THREE.Mesh(geo, mat);
    mesh.receiveShadow = true;
    group.add(mesh);
  }

  /** edge 在末端(end=false) 或首端(start=true) 的局部切线方向（THREE XZ）。
   *  ENU→THREE：THREE.x=ENU.x, THREE.z=-ENU.y。 */
  function _edgeDir(edge, fromStart) {
    const nodes = (edge.nodes || []).map((n) =>
      Array.isArray(n) ? [n[0] || 0, n[1] || 0, n[2] || 0]
        : [n.x || 0, n.y || 0, n.z || 0]);
    if (nodes.length < 2) return { x: 1, z: 0 };
    const a = fromStart ? nodes[0] : nodes[nodes.length - 2];
    const b = fromStart ? nodes[1] : nodes[nodes.length - 1];
    const dx = (b[0] - a[0]) || 0;
    const dz = -((b[1] - a[1]) || 0);
    const l = Math.hypot(dx, dz) || 1;
    return { x: dx / l, z: dz / l };
  }

  /** 2b. 交叉口路面补齐：在每个检测到的路口中心铺一块**真实路口多边形**。
   *  拓扑（centers/arms/折线点）来自 TopologyModel 单一事实源；
   *  标线用 walkFromJunction 径向出圈取位置与局部切线（曲路贴弯）；
   *  polygon 圆角化（Chaikin 2 次切角）；转向导流线用 junctionData 的 turn。 */
  /* fork 索引（消 connectingRoads 的 O(J×F)）：郑东 12027 fork × 11763 路口
   * 全遍历 = 1.4 亿次 fromIndices。预建 Map<key, [forkIdx]>，
   * key ∈ {incoming, incoming+'_', altId, altId+'_'}；查询时按 arm.edgeId
   * 的下划线前缀族查候选，只对候选跑 fromIndices（前缀/fromEnd 语义不变）。 */
  const buildForkIndex = (junctionDataArr) => {
    const idx = new Map();
    junctionDataArr.forEach((f, fi) => {
      if (!f || f.type !== 'fork') return;
      const id = String(f.incoming_road || '');
      if (!id) return;
      const isRev = id.startsWith('road_r');
      const altId = isRev ? id.replace('road_r', 'road_') : id;
      for (const key of [id, id + '_', altId, altId + '_']) {
        let arr = idx.get(key);
        if (!arr) { arr = []; idx.set(key, arr); }
        arr.push(fi);
      }
    });
    return idx;
  };
  /** arm.edgeId 的下划线前缀族 + altId（road_r→road_）前缀族 */
  const edgePrefixKeys = (edgeId) => {
    const keys = new Set();
    const add = (s) => {
      if (!s) return;
      const parts = s.split('_');
      let cur = '';
      for (let i = 0; i < parts.length; i++) {
        cur = cur ? cur + '_' + parts[i] : parts[i];
        keys.add(cur);
      }
    };
    add(edgeId);
    if (edgeId && edgeId.startsWith('road_r')) add(edgeId.replace('road_r', 'road_'));
    return [...keys];
  };

  function _buildJunctionPatch(topo, centers, junctionData, laneData) {
    if (!topo || !centers.length) return;

    const forkIndex = buildForkIndex(junctionData);

    // 路口多边形（合并单 mesh）+ 斑马线/停止线/导流线/导向箭头 InstancedMesh
    const polyPositions = [], polyIndices = [];
    const crossInstances = [];    // {x, z, rotY, len, w}
    const stopInstances = [];     // {x, z, rotY, len, w}
    const guideInstances = [];    // {x, z, rotY, len, w} 转向引导虚线
    const arrowInstances = [];    // {x, z, rotY, len, w} 地面导向箭头与人行预告菱形

    for (let ci = 0; ci < centers.length; ci++) {
      const c = centers[ci];
      const baseY = c.py || 0;   // 高架/隧道路口的标线随路面高程，不再钉在地面
      const arms = topo.armsOfJunction(ci);
      /* 段边界 vs 真实交叉口（郑东 OSM 大地图）：
       *  - 2 臂且两臂路口侧端点重合（<1.5m）→ SUMO 分段段边界，路面 ribbon
       *    贯穿已连续，画 patch 反而把路"切"成段段盖板 → 跳过
       *  - 2 臂但端点有 gap（≥1.5m）→ 同路两段没接上，需 patch 补空隙
       *  - ≥3 臂 → 真实交叉口，总是 patch */
      const isSegBoundary = arms.length === 2;
      if (isSegBoundary) {
        const p0 = arms[0].pts[arms[0].fromEnd ? arms[0].pts.length - 1 : 0];
        const p1 = arms[1].pts[arms[1].fromEnd ? arms[1].pts.length - 1 : 0];
        if (Math.hypot(p0.x - p1.x, p0.z - p1.z) < 1.5) continue;
      }
      if (arms.length < 2) continue;
      {
        const pts = [];
        for (const a of arms) {
          const w = walkFromJunction(a.pts, a.fromEnd, c.x, c.z, c.radius);
          const pxn = -w.uz, pzn = w.ux;
          const bx = w.x + pxn * a.hw;
          const bz = w.z + pzn * a.hw;
          const tx = w.x - pxn * a.hw;
          const tz = w.z - pzn * a.hw;
          pts.push({ x: bx, z: bz, ang: Math.atan2(bz - c.z, bx - c.x) }); // exempt: 排序用，非 heading 计算
          pts.push({ x: tx, z: tz, ang: Math.atan2(tz - c.z, tx - c.x) }); // exempt: 排序用，非 heading 计算
        }
        pts.sort((a, b) => a.ang - b.ang);
        /* 圆角化：Chaikin 2 次迭代切角，路口铺装从硬八边形变成圆角矩形
         * （向高德路口第一眼观感对齐），凸多边形保持扇形三角化成立。 */
        let smooth = pts;
        for (let iter = 0; iter < 2; iter++) {
          const next = [];
          for (let i = 0; i < smooth.length; i++) {
            const p = smooth[i], q = smooth[(i + 1) % smooth.length];
            next.push({ x: p.x * 0.75 + q.x * 0.25, z: p.z * 0.75 + q.z * 0.25 });
            next.push({ x: p.x * 0.25 + q.x * 0.75, z: p.z * 0.25 + q.z * 0.75 });
          }
          smooth = next;
        }
        const base = polyPositions.length / 3;
        for (const p of smooth) polyPositions.push(p.x, baseY + PATCH_Y, p.z);
        for (let i = 1; i < smooth.length - 1; i++) polyIndices.push(base, base + i, base + i + 1);
      }

      // ── 斑马线 + 停止线 + 导向箭头 + 转向导流线（只给 ≥3 臂真实交叉口）──
      // 2 臂 gap 段边界只是补空隙，不是交叉口，不画行人/停止线/箭头。
      if (arms.length >= 3) {
      const conns = connectingRoads(ci, arms, junctionData, laneData, forkIndex);
      const incomingIdx = new Set();
      for (const conn of conns) {
        if (conn.fromIdx != null && conn.toIdx != null) incomingIdx.add(conn.fromIdx);
      }
      for (let ai = 0; ai < arms.length; ai++) {
        const a = arms[ai];
        if (isTunnelEdge(a.edge)) continue;
        const wc = walkFromJunction(a.pts, a.fromEnd, c.x, c.z, c.radius + 2.0);
        const rotY = directionToRotationY(wc.ux, wc.uz);
        const step = CROSSWALK_STRIPE_W + CROSSWALK_GAP;
        const stripeCount = Math.max(2, Math.round(a.roadW / step));
        const nx = -wc.uz, nz = wc.ux;
        for (let i = 0; i < stripeCount; i++) {
          const across = (i - (stripeCount - 1) / 2) * step;
          crossInstances.push({ x: wc.x + nx * across, z: wc.z + nz * across, y: baseY + CROSS_Y, rotY, len: CROSSWALK_LENGTH, w: CROSSWALK_STRIPE_W });
        }
        if (incomingIdx.size && !incomingIdx.has(ai)) continue;   // 非来车 arm：无停止线与来车标线
        const ws = walkFromJunction(a.pts, a.fromEnd, c.x, c.z, c.radius + 2.0 + CROSSWALK_LENGTH + 1.5);
        const halfW = a.roadW * 0.25;
        stopInstances.push({ x: ws.x + ws.uz * halfW, z: ws.z - ws.ux * halfW, y: baseY + STOP_Y, rotY: directionToRotationY(ws.ux, ws.uz) + Math.PI / 2, len: a.roadW * 0.5, w: STOP_LINE_W });

        // ── 地面导向箭头（GB 5768.3 5.14）与人行横道预告菱形（5.16）──
        const isOneWay = a.edge && a.edge.oneway === true;
        const totalLanes = Math.max(1, Number(a.edge && a.edge.lanes) || 2);
        const apprLanes = isOneWay ? totalLanes : Math.max(1, Math.floor(totalLanes / 2));
        const laneW = Number(a.edge && a.edge.lane_width) || (a.roadW / totalLanes);

        for (const dist of [16.0, 32.0]) {
          const wa = walkFromJunction(a.pts, a.fromEnd, c.x, c.z, c.radius + 2.0 + CROSSWALK_LENGTH + dist);
          const ux = -wa.ux, uz = -wa.uz; // 车道来车前向单位向量
          const rx = uz, rz = -ux;        // 来车方向右侧法线
          for (let k = 0; k < apprLanes; k++) {
            const lOffset = isOneWay
              ? (k + 0.5 - apprLanes * 0.5) * laneW
              : (k + 0.5) * laneW;
            const lx = wa.x + rx * lOffset;
            const lz = wa.z + rz * lOffset;
            let turnType = 'straight';
            if (apprLanes === 1) turnType = 'straight_left';
            else if (apprLanes === 2) turnType = (k === 0) ? 'straight_left' : 'straight_right';
            else turnType = (k === 0) ? 'left' : (k === apprLanes - 1) ? 'right' : 'straight';
            _drawGroundArrow(lx, baseY + STOP_Y, lz, ux, uz, turnType, arrowInstances);
          }
        }
        // 人行横道预告菱形（距斑马线 42m）
        const wd = walkFromJunction(a.pts, a.fromEnd, c.x, c.z, c.radius + 2.0 + CROSSWALK_LENGTH + 42.0);
        const uxd = -wd.ux, uzd = -wd.uz;
        const rxd = uzd, rzd = -uxd;
        for (let k = 0; k < apprLanes; k++) {
          const lOffset = isOneWay
            ? (k + 0.5 - apprLanes * 0.5) * laneW
            : (k + 0.5) * laneW;
          _drawDiamondMarking(wd.x + rxd * lOffset, baseY + STOP_Y, wd.z + rzd * lOffset, uxd, uzd, arrowInstances);
        }
      }
      // 转向引导线：本路口每个 fork junction 的 incoming→connecting 路径
      for (const conn of conns) {
        if (conn.fromIdx == null || conn.toIdx == null) continue;
        const fa = arms[conn.fromIdx], ta = arms[conn.toIdx];
        const p0 = walkFromJunction(fa.pts, fa.fromEnd, c.x, c.z, c.radius + 2.5);
        const p1 = walkFromJunction(ta.pts, ta.fromEnd, c.x, c.z, c.radius + 2.5);
        _drawTurnGuide(p0, p1, conn.turn, baseY + STOP_Y, guideInstances);
      }
      }
    }

    // 路口多边形：合并单 mesh
    if (polyPositions.length >= 9) {
      const geo = new THREE.BufferGeometry();
      geo.setAttribute('position', new THREE.Float32BufferAttribute(polyPositions, 3));
      geo.setIndex(polyIndices);
      geo.computeVertexNormals();
      const mat = getStdMaterial(JUNCTION_COLOR, 0.9, 0.0);
      const mesh = new THREE.Mesh(geo, mat);
      mesh.receiveShadow = true;
      group.add(mesh);
    }

    if (crossInstances.length) {
      const geo = getBox(1, 0.02, 1);
      const mat = getStdMaterial(CROSSWALK_COLOR, 0.6, 0.0);
      const mesh = new THREE.InstancedMesh(geo, mat, crossInstances.length);
      const dummy = new THREE.Object3D();
      crossInstances.forEach((s, i) => {
        dummy.position.set(s.x, s.y, s.z);
        dummy.rotation.y = s.rotY;
        dummy.scale.set(s.len, 1, s.w);
        dummy.updateMatrix();
        mesh.setMatrixAt(i, dummy.matrix);
      });
      mesh.instanceMatrix.needsUpdate = true;
      mesh.receiveShadow = true;
      group.add(mesh);
    }

    if (stopInstances.length) {
      const geo = getBox(1, 0.02, 1);
      const mat = getStdMaterial(MERGE_LINE_COLOR, 0.6, 0.0);
      const mesh = new THREE.InstancedMesh(geo, mat, stopInstances.length);
      const dummy = new THREE.Object3D();
      stopInstances.forEach((s, i) => {
        dummy.position.set(s.x, s.y, s.z);
        dummy.rotation.y = s.rotY;
        dummy.scale.set(s.len, 1, s.w);
        dummy.updateMatrix();
        mesh.setMatrixAt(i, dummy.matrix);
      });
      mesh.instanceMatrix.needsUpdate = true;
      mesh.receiveShadow = true;
      group.add(mesh);
    }

    // 转向引导虚线（白，左转专用）：左转弯导流线，与停止线同高防 z-fight
    if (guideInstances.length) {
      const geo = getBox(1, 0.02, 1);
      const mat = getStdMaterial(MERGE_LINE_COLOR, 0.6, 0.0);
      const mesh = new THREE.InstancedMesh(geo, mat, guideInstances.length);
      const dummy = new THREE.Object3D();
      guideInstances.forEach((s, i) => {
        dummy.position.set(s.x, s.y, s.z);
        dummy.rotation.y = s.rotY;
        dummy.scale.set(s.len, 1, s.w);
        dummy.updateMatrix();
        mesh.setMatrixAt(i, dummy.matrix);
      });
      mesh.instanceMatrix.needsUpdate = true;
      mesh.receiveShadow = true;
      group.add(mesh);
    }

    // 地面导向箭头与人行预告菱形（白，GB 5768.3 标线标准）
    if (arrowInstances.length) {
      const geo = getBox(1, 0.02, 1);
      const mat = getStdMaterial(ARROW_COLOR, 0.6, 0.0);
      const mesh = new THREE.InstancedMesh(geo, mat, arrowInstances.length);
      const dummy = new THREE.Object3D();
      arrowInstances.forEach((s, i) => {
        dummy.position.set(s.x, s.y, s.z);
        dummy.rotation.y = s.rotY;
        dummy.scale.set(s.len, 1, s.w);
        dummy.updateMatrix();
        mesh.setMatrixAt(i, dummy.matrix);
      });
      mesh.instanceMatrix.needsUpdate = true;
      mesh.receiveShadow = true;
      mesh.frustumCulled = true;
      group.add(mesh);
    }

    /* SUMO connector 是导航拓扑，不是可见车道边界。它们的 centerline 若再
     * 画成白线，会与真实 lane marking、停止线、斑马线叠成白网；真实转向
     * 导流线统一由上面的 fork turn 数据生成。 */
  }

  /** 本路口的转向连接列表：fork 记录里 incoming_road 与 connecting_road 都有
   *  arm 在本路口的，才算本路口的转向路径。返回 [{fromIdx, toIdx, turn}]。
   *  incoming_road 是 base 路名（如 "东泰路"），arm edgeId 可能是其分段
   *  （"东泰路_47279978"）——来车匹配 = 精确相等 OR `base_` 前缀（下划线
   *  分隔防"东泰路北段"类误配）；一条双向路的所有分段 arm 都是来车端。
   *
   *  OSM 大地图（郑东等）：fork 的 connecting_roads 全部是 SUMO 内部 connector
   *  （road_j*），它们不在 edges/arms 列表中。直接 lookup armIdx.get(conn.id)
   *  必定 null → 全部跳过 → 转向导流线/停止线归属永远画不出来。
   *  修复：connector 走不通直查时，从 laneData 取其 lane successors →
   *  提取 base road id → 在 armIdx 里找目标 arm。
   *
   *  fork→路口 1:1 归属（2026-08-19 修复 fork 鬼连接）：
   *  SUMO 语义：fork 转向发生在 incoming_road.to_node = edge 末端。一条 road
   *  段两端各接一个路口、同时是两端 arm（fromEnd 一真一假），旧 prefix+altId
   *  无脑匹配让同一 fork 在两端各画一次——40–100m 外的鬼停止线/导流线。
   *  refined 匹配规则：
   *    exact/前缀（主匹配）→ fromEnd===true（转向在末端）
   *    altId 兜底（road_r→road_）→ fromEnd===false（forward 边的 to_node =
   *      reverse 边的 from_node，reverse 终点侧路口里 forward 边是起端） */
  function resolveConnectorTarget(connId, laneData, armIdx) {
    const lanes = laneData && laneData[connId];
    if (!Array.isArray(lanes)) return [];
    const found = [];
    for (const lane of lanes) {
      for (const s of (lane.successors || [])) {
        const base = String(s).split('.lane.')[0];
        const idx = armIdx.get(base);
        if (idx != null && !found.includes(idx)) found.push(idx);
      }
    }
    return found;
  }

  function connectingRoads(ci, arms, junctionData, laneData, forkIndex) {
    const out = [];
    if (!Array.isArray(junctionData) || !junctionData.length) return out;
    const armIdx = new Map();   // edgeId -> arms 下标
    arms.forEach((a, i) => { if (!armIdx.has(a.edgeId)) armIdx.set(a.edgeId, i); });
    // 匹配规则见函数头注释：主匹配 fromEnd===true；altId（road_r→road_）兜底要求 fromEnd===false。
    const fromIndices = (incoming) => {
      const id = String(incoming);
      const isReverse = id.startsWith('road_r');
      const altId = isReverse ? id.replace('road_r', 'road_') : id;
      const idPrefix = id + '_';
      const altPrefix = isReverse ? altId + '_' : null;
      const idxs = [];
      for (let i = 0; i < arms.length; i++) {
        const a = arms[i];
        if (a.edgeId === id) { if (a.fromEnd) idxs.push(i); continue; }
        if (a.edgeId.startsWith(idPrefix)) { if (a.fromEnd) idxs.push(i); continue; }
        if (altPrefix && (a.edgeId === altId || a.edgeId.startsWith(altPrefix))) {
          if (!a.fromEnd) idxs.push(i);
        }
      }
      return idxs;
    };
    // 候选 fork：由 arms 的 edgeId 前缀族查 forkIndex（消 O(J×F)）。索引查不到
    // 时（live 路径 arm.edgeId=数字串）回退全遍历——但该场景 fork 少，代价小。
    let candidates = junctionData;
    if (forkIndex && forkIndex.size && arms.length) {
      const candSet = new Set();
      for (const a of arms) {
        for (const key of edgePrefixKeys(a.edgeId)) {
          const arr = forkIndex.get(key);
          if (arr) for (const fi of arr) candSet.add(fi);
        }
      }
      if (candSet.size) candidates = [...candSet].sort((a, b) => a - b).map((fi) => junctionData[fi]);
    }
    for (const f of candidates) {
      if (!f || f.type !== 'fork') continue;
      const fromIdxs = fromIndices(f.incoming_road);
      if (!fromIdxs.length) continue;
      for (const conn of (f.connecting_roads || [])) {
        let toIdxs = armIdx.has(String(conn.id)) ? [armIdx.get(String(conn.id))] : [];
        if (!toIdxs.length) {
          toIdxs = resolveConnectorTarget(String(conn.id), laneData, armIdx);
        }
        for (const toIdx of toIdxs) {
          for (const fromIdx of fromIdxs) {
            if (toIdx === fromIdx) continue;
            out.push({ fromIdx, toIdx, turn: conn.turn || 'straight' });
          }
        }
      }
    }
    return out;
  }

  /** 左转弯引导虚线：从来车 arm 停止线锚点到目标 arm 锚点的二次贝塞尔，
   *  按 1m 实线 + 1m 间隔铺虚线段。控制点 = 两臂切线交点（平行退化为中点）。
   *  p0 的 walk 方向朝路口外，交通进入路口 = -u0；p1 朝路口外 = +u1（驶离）。 */
  function _drawTurnGuide(p0, p1, turn, y, out) {
    if (turn !== 'left') return;   // 只画左转导流（直行/右转不画，避免杂乱）
    const d0x = -p0.ux, d0z = -p0.uz;    // 入路口方向
    const d1x = p1.ux, d1z = p1.uz;      // 出路口方向
    /* 几何一致性过滤：数据说左转，实际转向也必须是左。incoming_road 前缀
     * 匹配会把同路 continuation 段也拉进候选（从该 arm 出发的同目标连接
     * 几何上是右转 = 幽灵导流），用转向符号剔除。
     * ENU 左转 = THREE XZ 有向角为负（y 轴翻转）。 */
    const cross = d0x * d1z - d0z * d1x;
    const dot = d0x * d1x + d0z * d1z;
    if (Math.atan2(cross, dot) > -0.3) return;   // exempt: 转向符号判定，非 heading 输出
    // 两切线交点：p0 + t·d0 = p1 + s·d1
    const det = d0x * (-d1z) - d0z * (-d1x);
    let ctrl;
    if (Math.abs(det) < 1e-6) {
      ctrl = { x: (p0.x + p1.x) / 2, z: (p0.z + p1.z) / 2 };
    } else {
      const t = ((p1.x - p0.x) * (-d1z) - (p1.z - p0.z) * (-d1x)) / det;
      ctrl = { x: p0.x + d0x * t, z: p0.z + d0z * t };
      // 交点退化保护：控制点距路口过远（>3 倍 arm 距）时退回中点
      const span = Math.hypot(p1.x - p0.x, p1.z - p0.z);
      if (Math.hypot(ctrl.x - p0.x, ctrl.z - p0.z) > span * 3) {
        ctrl = { x: (p0.x + p1.x) / 2, z: (p0.z + p1.z) / 2 };
      }
    }
    // 二次贝塞尔采样 + 按弧长铺 1m on / 1m off 虚线
    const N = 32, pts = [];
    for (let i = 0; i <= N; i++) {
      const t = i / N, u = 1 - t;
      pts.push({
        x: u * u * p0.x + 2 * u * t * ctrl.x + t * t * p1.x,
        z: u * u * p0.z + 2 * u * t * ctrl.z + t * t * p1.z,
      });
    }
    let acc = 0;
    for (let i = 1; i < pts.length; i++) {
      const dx = pts[i].x - pts[i - 1].x, dz = pts[i].z - pts[i - 1].z;
      const l = Math.hypot(dx, dz);
      if (l < 1e-9) continue;
      const segStart = acc, segEnd = acc + l;
      // 本段覆盖的 on 区间 [k*2, k*2+1)
      for (let s0 = Math.ceil(segStart / 2) * 2; s0 < segEnd; s0 += 2) {
        const s1 = Math.min(s0 + 1.0, segEnd);
        const t0 = (s0 - segStart) / l, t1 = (s1 - segStart) / l;
        const mx = (pts[i - 1].x + dx * t0 + pts[i - 1].x + dx * t1) / 2;
        const mz = (pts[i - 1].z + dz * t0 + pts[i - 1].z + dz * t1) / 2;
        out.push({ x: mx, z: mz, y, rotY: directionToRotationY(dx, dz), len: (s1 - s0), w: 0.15 });
      }
      acc = segEnd;
    }
  }

  /** 地面导向箭头生成（GB 5768.3 5.14）：直行、左转、右转、直行+左转、直行+右转 */
  function _drawGroundArrow(x, y, z, ux, uz, turnType, out) {
    // ux, uz: 沿车道行驶前向单位向量 (THREE x, z)
    // nx, nz: 车道左侧法向单位向量 (THREE x, z)
    const nx = -uz, nz = ux;
    const rotForward = directionToRotationY(ux, uz);

    // 1. 直行主干与箭头 (Straight stem + head)
    if (turnType === 'straight' || turnType === 'straight_left' || turnType === 'straight_right') {
      // 主干
      out.push({ x: x - ux * 0.4, y, z: z - uz * 0.4, rotY: rotForward, len: 2.6, w: 0.22 });
      // 左翼倒刺 (向左偏 28 度)
      const bux_l = ux * 0.88 + nx * 0.47, buz_l = uz * 0.88 + nz * 0.47;
      out.push({
        x: x + ux * 0.95 + nx * 0.28, y, z: z + uz * 0.95 + nz * 0.28,
        rotY: directionToRotationY(bux_l, buz_l), len: 1.1, w: 0.20,
      });
      // 右翼倒刺 (向右偏 28 度)
      const bux_r = ux * 0.88 - nx * 0.47, buz_r = uz * 0.88 - nz * 0.47;
      out.push({
        x: x + ux * 0.95 - nx * 0.28, y, z: z + uz * 0.95 - nz * 0.28,
        rotY: directionToRotationY(bux_r, buz_r), len: 1.1, w: 0.20,
      });
    }

    // 2. 左转弯曲主干与箭头 (Left hook + head)
    if (turnType === 'left' || turnType === 'straight_left' || turnType === 'turn') {
      const isCombo = turnType === 'straight_left';
      const ox = isCombo ? x + nx * 0.40 : x;
      const oz = isCombo ? z + nz * 0.40 : z;
      // 纵向直干段
      out.push({ x: ox - ux * 0.6, y, z: oz - uz * 0.6, rotY: rotForward, len: 1.8, w: 0.22 });
      // 左转弧线折线段 (向左 60 度)
      const lux = ux * 0.50 + nx * 0.866, luz = uz * 0.50 + nz * 0.866;
      out.push({
        x: ox + ux * 0.35 + nx * 0.55, y, z: oz + uz * 0.35 + nz * 0.55,
        rotY: directionToRotationY(lux, luz), len: 1.4, w: 0.22,
      });
      // 箭头头部 (纯向左 90度，带两侧倒刺)
      const tipX = ox + ux * 0.50 + nx * 1.15;
      const tipZ = oz + uz * 0.50 + nz * 1.15;
      out.push({ x: tipX - nx * 0.2, y, z: tipZ - nz * 0.2, rotY: directionToRotationY(nx, nz), len: 0.7, w: 0.22 });
      const tipBux1 = nx * 0.866 + ux * 0.5, tipBuz1 = nz * 0.866 + uz * 0.5;
      out.push({ x: tipX - nx * 0.15 + ux * 0.25, y, z: tipZ - nz * 0.15 + uz * 0.25, rotY: directionToRotationY(tipBux1, tipBuz1), len: 0.8, w: 0.20 });
      const tipBux2 = nx * 0.866 - ux * 0.5, tipBuz2 = nz * 0.866 - uz * 0.5;
      out.push({ x: tipX - nx * 0.15 - ux * 0.25, y, z: tipZ - nz * 0.15 - uz * 0.25, rotY: directionToRotationY(tipBux2, tipBuz2), len: 0.8, w: 0.20 });
    }

    // 3. 右转弯曲主干与箭头 (Right hook + head)
    if (turnType === 'right' || turnType === 'straight_right') {
      const isCombo = turnType === 'straight_right';
      const ox = isCombo ? x - nx * 0.40 : x;
      const oz = isCombo ? z - nz * 0.40 : z;
      // 纵向直干段
      out.push({ x: ox - ux * 0.6, y, z: oz - uz * 0.6, rotY: rotForward, len: 1.8, w: 0.22 });
      // 右转弧线折线段 (向右 60 度)
      const rux = ux * 0.50 - nx * 0.866, ruz = uz * 0.50 - nz * 0.866;
      out.push({
        x: ox + ux * 0.35 - nx * 0.55, y, z: oz + uz * 0.35 - nz * 0.55,
        rotY: directionToRotationY(rux, ruz), len: 1.4, w: 0.22,
      });
      // 箭头头部 (纯向右 90度，带两侧倒刺)
      const tipX = ox + ux * 0.50 - nx * 1.15;
      const tipZ = oz + uz * 0.50 - nz * 1.15;
      out.push({ x: tipX + nx * 0.2, y, z: tipZ + nz * 0.2, rotY: directionToRotationY(-nx, -nz), len: 0.7, w: 0.22 });
      const tipBux1 = -nx * 0.866 + ux * 0.5, tipBuz1 = -nz * 0.866 + uz * 0.5;
      out.push({ x: tipX + nx * 0.15 + ux * 0.25, y, z: tipZ + nz * 0.15 + uz * 0.25, rotY: directionToRotationY(tipBux1, tipBuz1), len: 0.8, w: 0.20 });
      const tipBux2 = -nx * 0.866 - ux * 0.5, tipBuz2 = -nz * 0.866 - uz * 0.5;
      out.push({ x: tipX + nx * 0.15 - ux * 0.25, y, z: tipZ + nz * 0.15 - uz * 0.25, rotY: directionToRotationY(tipBux2, tipBuz2), len: 0.8, w: 0.20 });
    }
  }

  /** 人行横道预告菱形标线（GB 5768.3 5.16） */
  function _drawDiamondMarking(x, y, z, ux, uz, out) {
    const nx = -uz, nz = ux;
    const halfL = 1.4, halfW = 0.55, w = 0.18;
    const len = Math.hypot(halfL, halfW);

    // 4 条边构成长轴 2.8m、短轴 1.1m 的标准菱形
    // 1. 前左边
    const e1x = -ux * halfL + nx * halfW, e1z = -uz * halfL + nz * halfW;
    out.push({
      x: x + ux * (halfL * 0.5) + nx * (halfW * 0.5), y,
      z: z + uz * (halfL * 0.5) + nz * (halfW * 0.5),
      rotY: directionToRotationY(e1x, e1z), len, w,
    });

    // 2. 前右边
    const e2x = -ux * halfL - nx * halfW, e2z = -uz * halfL - nz * halfW;
    out.push({
      x: x + ux * (halfL * 0.5) - nx * (halfW * 0.5), y,
      z: z + uz * (halfL * 0.5) - nz * (halfW * 0.5),
      rotY: directionToRotationY(e2x, e2z), len, w,
    });

    // 3. 后左边
    const e3x = ux * halfL + nx * halfW, e3z = uz * halfL + nz * halfW;
    out.push({
      x: x - ux * (halfL * 0.5) + nx * (halfW * 0.5), y,
      z: z - uz * (halfL * 0.5) + nz * (halfW * 0.5),
      rotY: directionToRotationY(e3x, e3z), len, w,
    });

    // 4. 后右边
    const e4x = ux * halfL - nx * halfW, e4z = uz * halfL - nz * halfW;
    out.push({
      x: x - ux * (halfL * 0.5) - nx * (halfW * 0.5), y,
      z: z - uz * (halfL * 0.5) - nz * (halfW * 0.5),
      rotY: directionToRotationY(e4x, e4z), len, w,
    });
  }

  /** 3. RampMerge：喇叭口导流线（三角形 + 白色斜线） */
  function _buildRampMerge(rampEdge, mainEdge) {
    const rampEnd = _getEdgeEnd(rampEdge);
    const mainStart = _getEdgeStart(mainEdge);
    if (!rampEnd || !mainStart) return;

    // 三角形导流带（连接 ramp 终点和主路边缘）
    const rampWidth = (rampEdge.lanes || 1) * (rampEdge.lane_width || 3.0);
    const mainWidth = (mainEdge.lanes || DEFAULT_LANES) * (mainEdge.lane_width || LANE_WIDTH);
    const positions = [
      rampEnd.x, 0.11, rampEnd.z + rampWidth/2,
      rampEnd.x, 0.11, rampEnd.z - rampWidth/2,
      mainStart.x + 10, 0.11, mainStart.z - mainWidth/2,
    ];
    const indices = [0, 1, 2];
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
    geo.setIndex(indices);
    geo.computeVertexNormals();
    const mat = getStdMaterial(TAPER_COLOR, 0.92, 0.0);
    const mesh = new THREE.Mesh(geo, mat);
    mesh.receiveShadow = true;
    group.add(mesh);

    // 导流斜线（5 条白色短线）
    const lineMat = getStdMaterial(MERGE_LINE_COLOR, 0.5, 0.0);
    for (let i = 0; i < 5; i++) {
      const t = i / 5;
      const lineGeo = getBox(0.15, 0.02, 1.5);
      const line = new THREE.Mesh(lineGeo, lineMat);
      line.position.set(rampEnd.x + t * 10, 0.16, rampEnd.z - rampWidth/2 + t * 1.5);
      line.rotation.y = Math.PI / 6;
      group.add(line);
    }
  }

  /** 4. ViaductPier：高架桥墩与盖梁收集（支持单柱/双柱自适应与真实盖梁） */
  function _collectViaductPiers(edge, pierInstances, capInstances) {
    const nodes = (edge.nodes || []).map((n) =>
      Array.isArray(n) ? [n[0] || 0, n[1] || 0, n[2] || 0]
        : [n.x || 0, n.y || 0, n.z || 0]);
    if (nodes.length < 2) return;

    const laneW = Number(edge.lane_width) || 3.5;
    const lanes = Math.max(1, Number(edge.lanes) || 2);
    const halfW = (lanes * laneW) / 2 + 0.6;

    const cum = [0];
    for (let i = 1; i < nodes.length; i++) {
      cum.push(cum[i - 1] + Math.hypot(
        (nodes[i][0] - nodes[i - 1][0]) || 0, (nodes[i][1] - nodes[i - 1][1]) || 0));
    }
    const total = cum[cum.length - 1] || (edge.length_m || 0);
    if (total <= 0) return;

    const sampleAt = (s) => {
      if (s <= 0) return { x: nodes[0][0], y: nodes[0][1], z: nodes[0][2] };
      if (s >= total) {
        const n = nodes[nodes.length - 1];
        return { x: n[0], y: n[1], z: n[2] };
      }
      for (let i = 1; i < cum.length; i++) {
        if (cum[i] >= s) {
          const t = (s - cum[i - 1]) / ((cum[i] - cum[i - 1]) || 1);
          return {
            x: nodes[i - 1][0] + (nodes[i][0] - nodes[i - 1][0]) * t,
            y: nodes[i - 1][1] + (nodes[i][1] - nodes[i - 1][1]) * t,
            z: nodes[i - 1][2] + (nodes[i][2] - nodes[i - 1][2]) * t,
          };
        }
      }
      const n = nodes[nodes.length - 1];
      return { x: n[0], y: n[1], z: n[2] };
    };

    let lastS = -1e9;
    for (let s = 12; s < total; s += 22) {
      const p = sampleAt(s);
      const h = p.z || 0;
      if (h < 0.8) continue;
      if (s - lastS < 20) continue;

      const pNext = sampleAt(Math.min(total, s + 1.0));
      const [tx, , tz] = worldToThree(p.x, p.y, h);
      const [tnx, , tnz] = worldToThree(pNext.x, pNext.y, pNext.z || h);
      const dx = tnx - tx, dz = tnz - tz;
      const dl = Math.hypot(dx, dz) || 1;
      const ux = dx / dl, uz = dz / dl;
      const nx = -uz, nz = ux;
      const rotY = directionToRotationY(ux, uz);

      if (halfW < 6.0) {
        // 窄路面/单向匝道：单圆柱墩 + 顶部梯形盖梁
        pierInstances.push({ tx, tz, h: h - 0.3, radius: 0.75 });
        if (capInstances) {
          capInstances.push({
            tx, ty: h - 0.35, tz,
            len: Math.max(1.8, halfW * 1.4), h: 0.65, w: 1.5,
            rotY,
          });
        }
      } else {
        // 宽幅主线高架：双柱门式墩 + 跨线连续重型盖梁
        const colOffset = halfW * 0.48;
        pierInstances.push({ tx: tx - nx * colOffset, tz: tz - nz * colOffset, h: h - 0.4, radius: 0.75 });
        pierInstances.push({ tx: tx + nx * colOffset, tz: tz + nz * colOffset, h: h - 0.4, radius: 0.75 });
        if (capInstances) {
          capInstances.push({
            tx, ty: h - 0.45, tz,
            len: halfW * 1.95, h: 0.85, w: 1.9,
            rotY,
          });
        }
      }
      lastS = s;
    }
  }

  /** 4b. BridgeRamp：桥隧端点坡道渐变过渡（消除 6m 悬崖）几何收集 */
  function _collectBridgeRamps(edge, rampGeos) {
    const nodes = (edge.nodes || []).map((n) =>
      Array.isArray(n) ? [n[0] || 0, n[1] || 0, n[2] || 0]
        : [n.x || 0, n.y || 0, n.z || 0]);
    if (nodes.length < 2) return;

    const laneW = Number(edge.lane_width) || 3.5;
    const lanes = Number(edge.lanes) || 2;
    const halfW = (lanes * laneW) / 2 + 0.6;
    const RAMP_LEN = 30;
    const THRESHOLD = 2.0;

    const h0 = nodes[0][2] || 0;
    const h1 = nodes[1][2] || 0;
    const hEnd = nodes[nodes.length - 1][2] || 0;
    const hPrev = nodes[nodes.length - 2][2] || 0;

    if (Math.abs(h1 - h0) > THRESHOLD) {
      const geo = _createRampSegmentGeo(nodes, 0, 1, halfW, h0, RAMP_LEN);
      if (geo) rampGeos.push(geo);
    }
    if (Math.abs(hEnd - hPrev) > THRESHOLD) {
      const geo = _createRampSegmentGeo(nodes, nodes.length - 1, nodes.length - 2, halfW, hEnd, RAMP_LEN);
      if (geo) rampGeos.push(geo);
    }
  }

  function _createRampSegmentGeo(nodes, tipIdx, adjIdx, halfW, tipH, rampLen) {
    const tip = nodes[tipIdx], adj = nodes[adjIdx];
    if (!tip || !adj) return null;
    const dx = tip[0] - adj[0], dy = tip[1] - adj[1];
    const dl = Math.hypot(dx, dy) || 1;
    const ux = dx / dl, uy = dy / dl;
    const nx = -uy, ny = ux;

    const steps = 4;
    const positions = [];
    const indices = [];
    for (let i = 0; i <= steps; i++) {
      const t = i / steps;
      const ex = tip[0] + ux * rampLen * t;
      const ey = tip[1] + uy * rampLen * t;
      const h = tipH * (1 - t);
      const [lx, , lz] = worldToThree(ex + nx * halfW, ey + ny * halfW, 0);
      const [rx, , rz] = worldToThree(ex - nx * halfW, ey - ny * halfW, 0);
      positions.push(lx, h, lz, rx, h, rz);
    }
    for (let i = 0; i < steps; i++) {
      const base = i * 2;
      indices.push(base, base + 2, base + 1, base + 1, base + 2, base + 3);
    }
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
    geo.setIndex(indices);
    geo.computeVertexNormals();
    return geo;
  }

  /** 5. BarrierEndCap：防撞桶收集 */
  function _collectBarrierEndCaps(edge, byId, endpointLocs, endpointGrid, epCell, barrelLocs) {
    const entry = byId ? byId.get(String(edge.id)) : null;
    const nodes = (edge.nodes || []).map((n) =>
      Array.isArray(n) ? n : [n.x || 0, n.y || 0, n.z || 0]);
    if (nodes.length < 2) return;

    const isDeadEnd = (node) => {
      const [x, , z] = worldToThree(node[0] || 0, node[1] || 0, node[2] || 0);
      const cx = Math.floor(x / epCell), cz = Math.floor(z / epCell);
      for (let dx = -1; dx <= 1; dx++) {
        for (let dz = -1; dz <= 1; dz++) {
          const bucket = endpointGrid.get(`${cx + dx},${cz + dz}`);
          if (!bucket) continue;
          for (const ep of bucket) {
            if (ep.id === String(edge.id)) continue;
            if (Math.hypot(ep.x - x, ep.z - z) < DEAD_END_TOL_M) return false;
          }
        }
      }
      return true;
    };

    const startDead = entry ? entry.start == null : true;
    if (startDead && isDeadEnd(nodes[0])) {
      const [sx, , sz] = worldToThree(nodes[0][0] || 0, nodes[0][1] || 0, nodes[0][2] || 0);
      const [nx1, , nz1] = worldToThree(nodes[1][0] || 0, nodes[1][1] || 0, nodes[1][2] || 0);
      const tx = nx1 - sx, tz = nz1 - sz;
      const tl = Math.hypot(tx, tz) || 1;
      barrelLocs.push({ x: sx - (tz / tl) * 0.5, z: sz + (tx / tl) * 0.5 });
    }
    const endDead = entry ? entry.end == null : true;
    if (endDead && isDeadEnd(nodes[nodes.length - 1])) {
      const a = nodes[nodes.length - 2], b = nodes[nodes.length - 1];
      const [ax, , az] = worldToThree(a[0] || 0, a[1] || 0, a[2] || 0);
      const [bx, , bz] = worldToThree(b[0] || 0, b[1] || 0, b[2] || 0);
      const tx = bx - ax, tz = bz - az;
      const tl = Math.hypot(tx, tz) || 1;
      barrelLocs.push({ x: bx - (tz / tl) * 0.5, z: bz + (tx / tl) * 0.5 });
    }
  }

  /** 工具：获取 edge 起点（优先用 nodes[0]，否则用 length_m 累计）
   *  注意：scene_pub 输出的 nodes 是 ENU [x_ENU, y_North, z_Up] 三元组，
   *  ENU→THREE 翻转统一走 worldToThree（门禁强制，禁止手写裸 -y/交换 y/z）。
   *  返回 THREE 坐标 {x, y(up), z}。 */
  function _getEdgeStart(edge) {
    if (edge.nodes && edge.nodes.length >= 1) {
      const n = edge.nodes[0];
      if (Array.isArray(n)) {
        const [tx, ty, tz] = worldToThree(n[0] || 0, n[1] || 0, n[2] || 0);
        return { x: tx, y: ty, z: tz };
      }
      if (typeof n === 'object') {
        const [tx, ty, tz] = worldToThree(n.x || 0, n.y || 0, n.z || 0);
        return { x: tx, y: ty, z: tz };
      }
    }
    if (edge.start_x != null) {
      const [tx, ty, tz] = worldToThree(edge.start_x, edge.start_z || 0, 0);
      return { x: tx, y: ty, z: tz };
    }
    return null;
  }

  /** 工具：获取 edge 终点 */
  function _getEdgeEnd(edge) {
    if (edge.nodes && edge.nodes.length >= 2) {
      const n = edge.nodes[edge.nodes.length - 1];
      if (Array.isArray(n)) {
        const [tx, ty, tz] = worldToThree(n[0] || 0, n[1] || 0, n[2] || 0);
        return { x: tx, y: ty, z: tz };
      }
      if (typeof n === 'object') {
        const [tx, ty, tz] = worldToThree(n.x || 0, n.y || 0, n.z || 0);
        return { x: tx, y: ty, z: tz };
      }
    }
    if (edge.start_x != null) {
      const len = edge.length_m || 100;
      const h = edge.heading || 0;
      const [fex, fey] = forwardENU(h);
      const sx = edge.start_x + fex * len;
      const sy = (edge.start_z || 0) + fey * len;
      const [tx, ty, tz] = worldToThree(sx, sy, 0);
      return { x: tx, y: ty, z: tz };
    }
    return null;
  }

  function isBuilt() { return built; }
  function getGroup() { return group; }

  return { build, clear, isBuilt, getGroup };
}
