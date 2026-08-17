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
import { SCENE } from '../theme/tokens.js';

/* 配色全部来自 theme/tokens.js（P3 设计 token 单一事实源）。
 * TAPER/JUNCTION 必须与 RoadView 路面同色（无缝），所以共享 SCENE.asphalt。 */
const TAPER_COLOR  = SCENE.asphalt;   // 锥形过渡路面（同沥青色）
const JUNCTION_COLOR = SCENE.asphalt; // 路口区域与路面同色（2026-08-14 无缝，不再像补丁块）
const CROSSWALK_COLOR = SCENE.crosswalk; // 斑马线白
const MERGE_LINE_COLOR = SCENE.guideLine; // 导流线白
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
    /* fork/merge 转向记录走 map_junctions（map.json 原样透传）；
     * roadNetwork.junctions 保留给 {x,y,z,radius} 路口中心格式（JunctionDetect 用）。 */
    const junctions = roadNetwork.map_junctions || [];

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
    //  沥青路面（比路宽大一点，覆盖四向收口后的缺口）+ 四个方向斑马线 ──
    _buildJunctionPatch(topo, centers, junctions);

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

    // ── 4. ViaductPier：高架（bridge）edge 按 centerline.z 落墩 ──
    // 数据契约：本仓库 map.json 的 edge 携带 bridge/tunnel 布尔 + centerline.z
    // （osm2kmap.apply_osm_elevation 写 6.0*max(1,layer) / -4），**不产出**
    // elevation_profile。旧实现读 elevation_profile → 永远跳过（死代码）。
    // 现改为消费 bridge 标记 + edge.nodes z：高架（h>0.5）每隔 20m 落一根桥墩。
    for (const edge of edges) {
      if (!edge.bridge) continue;
      _buildViaductPier(edge);
    }

    // ── 5. BarrierEndCap：只在**真断头端点**（孤端）放防撞桶 ──
    //  2026-08-14：旧实现对每个 edge 起止点都放，OSM 地图每条 way 是一个
    //  edge、端点几乎都在路口/连续处 → 防撞桶铺满全图。改为 byId 判定：
    //  entry.start/end == null 才是断头路/地图边界。
    //  2026-08（本会话）：byId 只识别 ≥3 臂路口簇，2 端点接缝（连续连接/
    //  弯道转折/环形路段间）被剔成 null → 连接处/弯路仍铺桶（用户报障）。
    //  补"孤端检测"：端点无任何其他 edge 端点在邻接容差内才是真断头；
    //  连续接缝/弯道转折/路口汇聚都有邻接端点 → 不放桶。
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
    for (const edge of edges) {
      if (edge.type === 'ramp_curve' || edge.type === 'intersection') continue;
      _buildBarrierEndCap(edge, byId, endpointLocs);
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
  function _buildJunctionPatch(topo, centers, junctionData) {
    if (!topo || !centers.length) return;

    // 路口多边形（合并单 mesh）+ 斑马线/停止线/导流线 InstancedMesh
    const polyPositions = [], polyIndices = [];
    const crossInstances = [];    // {x, z, rotY, len, w}
    const stopInstances = [];     // {x, z, rotY, len, w}
    const guideInstances = [];    // {x, z, rotY, len, w} 转向引导虚线

    for (let ci = 0; ci < centers.length; ci++) {
      const c = centers[ci];
      const arms = topo.armsOfJunction(ci);
      if (arms.length >= 2) {
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
        for (const p of smooth) polyPositions.push(p.x, PATCH_Y, p.z);
        for (let i = 1; i < smooth.length - 1; i++) polyIndices.push(base, base + i, base + i + 1);
      }

      // ── 斑马线 + 停止线 + 转向导流线 ──
      // P1 路口渠化：先算本路口的转向连接（fork incoming→connecting 双 arm 都在
      // 本路口才算），停止线按来车归属——只有作为某条转向路径来车端的 arm 才画
      // （单行道驶出侧/纯出口 arm 不画）；无 turn 数据时回退每个 arm 全画。
      // 斑马线行人过街与车流方向无关，保持每个 arm 都画。
      const conns = connectingRoads(ci, arms, junctionData);
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
          crossInstances.push({ x: wc.x + nx * across, z: wc.z + nz * across, rotY, len: CROSSWALK_LENGTH, w: CROSSWALK_STRIPE_W });
        }
        if (incomingIdx.size && !incomingIdx.has(ai)) continue;   // 非来车 arm：无停止线
        const ws = walkFromJunction(a.pts, a.fromEnd, c.x, c.z, c.radius + 2.0 + CROSSWALK_LENGTH + 1.5);
        const halfW = a.roadW * 0.25;
        stopInstances.push({ x: ws.x + ws.uz * halfW, z: ws.z - ws.ux * halfW, rotY: directionToRotationY(ws.ux, ws.uz) + Math.PI / 2, len: a.roadW * 0.5, w: STOP_LINE_W });
      }
      // 转向引导线：本路口每个 fork junction 的 incoming→connecting 路径
      for (const conn of conns) {
        if (conn.fromIdx == null || conn.toIdx == null) continue;
        const fa = arms[conn.fromIdx], ta = arms[conn.toIdx];
        const p0 = walkFromJunction(fa.pts, fa.fromEnd, c.x, c.z, c.radius + 2.5);
        const p1 = walkFromJunction(ta.pts, ta.fromEnd, c.x, c.z, c.radius + 2.5);
        _drawTurnGuide(p0, p1, conn.turn, guideInstances);
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
        dummy.position.set(s.x, CROSS_Y, s.z);
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
        dummy.position.set(s.x, STOP_Y, s.z);
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
        dummy.position.set(s.x, STOP_Y, s.z);
        dummy.rotation.y = s.rotY;
        dummy.scale.set(s.len, 1, s.w);
        dummy.updateMatrix();
        mesh.setMatrixAt(i, dummy.matrix);
      });
      mesh.instanceMatrix.needsUpdate = true;
      mesh.receiveShadow = true;
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
   *  分隔防"东泰路北段"类误配）；一条双向路的所有分段 arm 都是来车端。 */
  function connectingRoads(ci, arms, junctionData) {
    const out = [];
    if (!Array.isArray(junctionData) || !junctionData.length) return out;
    const armIdx = new Map();   // edgeId -> arms 下标
    arms.forEach((a, i) => { if (!armIdx.has(a.edgeId)) armIdx.set(a.edgeId, i); });
    const fromIndices = (incoming) => {
      const id = String(incoming);
      const prefix = id + '_';
      const idxs = [];
      arms.forEach((a, i) => {
        if (a.edgeId === id || a.edgeId.startsWith(prefix)) idxs.push(i);
      });
      return idxs;
    };
    for (const f of junctionData) {
      if (!f || f.type !== 'fork') continue;
      const fromIdxs = fromIndices(f.incoming_road);
      if (!fromIdxs.length) continue;
      for (const conn of (f.connecting_roads || [])) {
        const toIdx = armIdx.get(String(conn.id));
        if (toIdx == null) continue;
        for (const fromIdx of fromIdxs) {
          if (toIdx === fromIdx) continue;
          out.push({ fromIdx, toIdx, turn: conn.turn || 'straight' });
        }
      }
    }
    return out;
  }

  /** 左转弯引导虚线：从来车 arm 停止线锚点到目标 arm 锚点的二次贝塞尔，
   *  按 1m 实线 + 1m 间隔铺虚线段。控制点 = 两臂切线交点（平行退化为中点）。
   *  p0 的 walk 方向朝路口外，交通进入路口 = -u0；p1 朝路口外 = +u1（驶离）。 */
  function _drawTurnGuide(p0, p1, turn, out) {
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
        out.push({ x: mx, z: mz, rotY: directionToRotationY(dx, dz), len: (s1 - s0), w: 0.15 });
      }
      acc = segEnd;
    }
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

  /** 4. ViaductPier：高架桥墩（每隔 20m 一根圆柱）
   *  数据契约：edge.nodes 是 ENU [x,y,z]，z 即高程（osm2kmap 写
   *  6.0*max(1,layer)）。沿 edge 弧长每 20m 取一点，高程 h>0.5（高架）才落墩，
   *  桥墩从地面 y=0 立到路面下沿 y=h。曲线/弯道路也按弧长真实取点（不再假设沿
   *  +X 直伸），位置走 worldToThree。 */
  function _buildViaductPier(edge) {
    const nodes = (edge.nodes || []).map((n) =>
      Array.isArray(n) ? [n[0] || 0, n[1] || 0, n[2] || 0]
        : [n.x || 0, n.y || 0, n.z || 0]);
    if (nodes.length < 2) return;

    // 累计弧长 + 各点高程（ENU z）
    const cum = [0];
    for (let i = 1; i < nodes.length; i++) {
      cum.push(cum[i - 1] + Math.hypot(
        (nodes[i][0] - nodes[i - 1][0]) || 0, (nodes[i][1] - nodes[i - 1][1]) || 0));
    }
    const total = cum[cum.length - 1] || (edge.length_m || 0);
    if (total <= 0) return;

    // 弧长 s 处插值 ENU(x,y) 与高程 z
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

    const pierMat = getStdMaterial(PIER_COLOR, 0.9, 0.0);
    const pierGeo = getCylinder(0.6, 0.8, 1);  // 高度 1，后续按 h 缩放
    let lastS = -1e9;
    for (let s = 10; s < total; s += 20) {
      const p = sampleAt(s);
      const h = p.z || 0;
      if (h < 0.5) continue;                 // 仅高架（地面/隧道不落墩）
      if (s - lastS < 20) continue;
      const [tx, , tz] = worldToThree(p.x, p.y, h);  // 桥墩顶到路面下沿
      const pier = new THREE.Mesh(pierGeo, pierMat);
      pier.position.set(tx, h / 2, tz);      // 圆柱高 h，span 0→h
      pier.scale.y = h;
      pier.castShadow = true;
      pier.receiveShadow = true;
      group.add(pier);
      lastS = s;
    }
  }

  /** 5. BarrierEndCap：防撞桶（红白圆柱）只放在真断头（孤端）端点。
   *  byId: Map<edgeId,{start,end}>（≥3 臂路口中心索引或 null），用于跳过路口。
   *  endpointLocs: 所有 edge 端点（THREE 坐标 + edge id），孤端判定用。
   *  断头 = 端点既不属于 ≥3 臂路口，又无任何其他 edge 端点在邻接容差内
   *  （连续连接/弯道转折/环形路段间两端点重合 → 有邻接 → 不是断头）。
   *  位置用端点切线的右法线（沿道路外侧），不再硬编码 +z。 */
  function _buildBarrierEndCap(edge, byId, endpointLocs) {
    const entry = byId ? byId.get(String(edge.id)) : null;
    const nodes = (edge.nodes || []).map((n) =>
      Array.isArray(n) ? n : [n.x || 0, n.y || 0, n.z || 0]);
    if (nodes.length < 2) return;

    const put = (pos, outward) => {
      const redGeo = getCylinder(0.3, 0.35, 0.4);
      const redMat = getStdMaterial(BARREL_RED, 0.6, 0.1);
      const red = new THREE.Mesh(redGeo, redMat);
      red.position.set(pos.x + outward.x * 0.5, 0.3, pos.z + outward.z * 0.5);
      red.castShadow = true;
      group.add(red);

      const whiteGeo = getCylinder(0.3, 0.3, 0.3);
      const whiteMat = getStdMaterial(BARREL_WHITE, 0.6, 0.1);
      const white = new THREE.Mesh(whiteGeo, whiteMat);
      white.position.set(pos.x + outward.x * 0.5, 0.75, pos.z + outward.z * 0.5);
      group.add(white);
    };

    // 端点是否真断头：不属于 ≥3 臂路口 + 无任何邻接端点
    const isDeadEnd = (node) => {
      const [x, , z] = worldToThree(node[0] || 0, node[1] || 0, node[2] || 0);
      for (const ep of endpointLocs) {
        if (ep.id === String(edge.id)) continue;
        if (Math.hypot(ep.x - x, ep.z - z) < DEAD_END_TOL_M) return false;
      }
      return true;
    };

    // 起点断头：切线 = nodes[0]→nodes[1]，向外 = -切线 的右法线
    const startDead = entry ? entry.start == null : true;
    if (startDead && isDeadEnd(nodes[0])) {
      const [sx, , sz] = worldToThree(nodes[0][0] || 0, nodes[0][1] || 0, nodes[0][2] || 0);
      const [nx1, , nz1] = worldToThree(nodes[1][0] || 0, nodes[1][1] || 0, nodes[1][2] || 0);
      const tx = nx1 - sx, tz = nz1 - sz;
      const tl = Math.hypot(tx, tz) || 1;
      // 右法线（切线右侧）= (-tz, tx)/tl；起点向外取右法线
      put({ x: sx, z: sz }, { x: -tz / tl, z: tx / tl });
    }
    // 终点断头：切线 = nodes[n-2]→nodes[n-1]，向外 = 右法线
    const endDead = entry ? entry.end == null : true;
    if (endDead && isDeadEnd(nodes[nodes.length - 1])) {
      const a = nodes[nodes.length - 2], b = nodes[nodes.length - 1];
      const [ax, , az] = worldToThree(a[0] || 0, a[1] || 0, a[2] || 0);
      const [bx, , bz] = worldToThree(b[0] || 0, b[1] || 0, b[2] || 0);
      const tx = bx - ax, tz = bz - az;
      const tl = Math.hypot(tx, tz) || 1;
      put({ x: bx, z: bz }, { x: -tz / tl, z: tx / tl });
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
