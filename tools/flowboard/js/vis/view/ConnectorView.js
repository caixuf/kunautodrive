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
import { LANE_WIDTH, DEFAULT_LANES, EDGE_TYPE } from '../core/Constants.js';
import { worldToThree, forwardENU, directionToRotationY } from '../math/Coord.js';
import { detectJunctions } from './JunctionDetect.js';

const TAPER_COLOR  = 0x2a2a2a;   // 锥形过渡路面（同沥青色）
const JUNCTION_COLOR = 0x2a2a2a; // 路口区域与路面同色（2026-08-14 无缝，不再像补丁块）
const CROSSWALK_COLOR = 0xf5f5f0; // 斑马线白
const MERGE_LINE_COLOR = 0xffffff; // 导流线白
const PIER_COLOR   = 0x6b6b6b;   // 桥墩灰
const BARREL_RED   = 0xd02020;   // 防撞桶红
const BARREL_WHITE = 0xf0f0f0;   // 防撞桶白
const CROSSWALK_STRIPE_W = 0.5;  // 条带宽（跨道路方向）
const CROSSWALK_GAP = 0.5;       // 条带间距（跨道路方向）
const CROSSWALK_LENGTH = 2.5;    // 条纹沿道路长度（GB 5768.3 交叉口 ≥2m）
const STOP_LINE_W = 0.45;        // 停止线宽（垂直于行车的横条）
const PATCH_Y = 0.105;           // 路口铺装略高于路面防 z-fight
const CROSS_Y = 0.16;            // 斑马线高度
const STOP_Y = 0.17;             // 停止线高度（比斑马线再高一点防 z-fight）

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
    const junctions = roadNetwork.junctions || [];

    // ── 1. LaneTaper：相邻 edge 车道数/宽度不同 ──
    for (let i = 0; i < edges.length - 1; i++) {
      const a = edges[i], b = edges[i + 1];
      // 只处理顺序相连的 edge（id 连续），跳过匝道等跳跃 edge
      if (b.id !== a.id + 1) continue;
      const widthA = (a.lanes || DEFAULT_LANES) * (a.lane_width || LANE_WIDTH);
      const widthB = (b.lanes || DEFAULT_LANES) * (b.lane_width || LANE_WIDTH);
      if (Math.abs(widthA - widthB) < 0.1) continue;
      _buildLaneTaper(a, b, widthA, widthB);
    }

    // ── 2. JunctionCap 已废弃（2026-08-14）：路面 ribbon 不再截断，intersection
    //  类型 edge 的路面由 RoadView 贯穿渲染覆盖；旧 cap 硬编码 +X 方向与数据驱动
    //  的 _buildJunctionPatch 重叠渲染且方向错，删除。 ──

    // ── 2b. 交叉口路面补齐：在每个检测到的路口中心铺一块**真实路口多边形**
    //  沥青路面（比路宽大一点，覆盖四向收口后的缺口）+ 四个方向斑马线 ──
    const { centers, byId } = detectJunctions(roadNetwork);
    _buildJunctionPatch(roadNetwork, centers, byId);

    // ── 2c. 转向连接曲线：按 fork junctions[] 的 connecting_roads[].turn 画
    //  车辆转向路径（左转向左弯/右转向右弯/直行平直），颜色区分转向类型。──
    if (junctions.length) _buildTurnConnectors(junctions, roadNetwork, centers, byId);

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

    // ── 4. ViaductPier：elevation_profile 中 h > 0.5 的段 ──
    for (const edge of edges) {
      if (!edge.elevation_profile) continue;
      _buildViaductPier(edge);
    }

    // ── 5. BarrierEndCap：每个 edge 起止点放防撞桶 ──
    for (const edge of edges) {
      if (edge.type === 'ramp_curve' || edge.type === 'intersection') continue;
      _buildBarrierEndCap(edge);
    }

    built = true;
  }

  /** 1. LaneTaper：锥形过渡路面
   *  在 edgeA 终点位置生成一个梯形（宽 widthA → widthB），长度 5m */
  function _buildLaneTaper(edgeA, edgeB, widthA, widthB) {
    // 推断接缝位置：用 edgeA 的 nodes 末点，或 length_m 累计
    const pos = _getEdgeEnd(edgeA);
    if (!pos) return;

    const taperLen = 5;
    const hA = widthA / 2, hB = widthB / 2;
    // 梯形顶点（沿 X 轴铺路假设）
    const positions = [
      pos.x - taperLen/2, 0.11, pos.z + hA,   // 左后
      pos.x - taperLen/2, 0.11, pos.z - hA,   // 右后
      pos.x + taperLen/2, 0.11, pos.z + hB,   // 左前
      pos.x + taperLen/2, 0.11, pos.z - hB,   // 右前
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

  /** 2b. 交叉口路面补齐：在每个检测到的路口中心铺一块**真实路口多边形**。
   *  2026-08-14：路面 ribbon 不再截断（RoadView 贯穿路口），patch 不再是
   *  "补缺口"的唯一手段，改为颜色统一（沥青底色覆盖四路重叠的杂乱边缘）；
   *  斑马线/停止线方向统一用路口端向外切线（与 polygon arms 同一基准）。
   *  数据源：数据层 junctions[]（scene_pub 预计算）优先，几何聚类兜底。
   *  斑马线/停止线保持 InstancedMesh 合批；路口多边形合并为单个 BufferGeometry。
   *  centers/byId 由 build() 统一 detectJunctions 计算后传入，避免重复检测。 */
  function _buildJunctionPatch(roadNetwork, centers, byId) {
    if (!centers.length) return;

    const edges = roadNetwork.edges;
    const edgeMap = new Map(edges.map((e) => [String(e.id), e]));

    // 路口多边形（全部合并进一个 BufferGeometry，静态路网单 mesh）+ 标线实例
    const polyPositions = [], polyIndices = [];
    const crossInstances = [];    // {x, z, rotY, len, w}
    const stopInstances = [];     // {x, z, rotY, len, w}

    for (let ci = 0; ci < centers.length; ci++) {
      const c = centers[ci];

      // ── 收集本路口所有进入方向的 (单位方向, 半宽)，构建真实路口多边形 ──
      // 每条关联 road 在收口半径处有两个边界点（中心线端 ± 半宽×法线），
      // 按绕中心的角度排序围成凸多边形（十字路口 → 八边形，非正方形）。
      const arms = [];   // {dx, dz, hw}
      for (const [edgeId, entry] of byId) {
        if (entry.start !== ci && entry.end !== ci) continue;
        const edge = edgeMap.get(edgeId);
        if (!edge || !Array.isArray(edge.nodes) || edge.nodes.length < 2) continue;
        const nodes = edge.nodes.map((n) =>
          Array.isArray(n) ? n : [n.x || 0, n.y || 0, n.z || 0]);
        const fromEnd = entry.end === ci;
        const aIdx = fromEnd ? nodes.length - 2 : 0;
        const bIdx = fromEnd ? nodes.length - 1 : 1;
        /* ENU [x,y,z] → THREE 走 worldToThree（门禁强制，禁止裸 -y 翻转） */
        const [ax, , az] = worldToThree(Number(nodes[aIdx][0]) || 0, Number(nodes[aIdx][1]) || 0, Number(nodes[aIdx][2]) || 0);
        const [bx, , bz] = worldToThree(Number(nodes[bIdx][0]) || 0, Number(nodes[bIdx][1]) || 0, Number(nodes[bIdx][2]) || 0);
        // 向外切线（路口端最后一小段），与斑马线/停止线共用同一方向基准
        const dirX = fromEnd ? ax - bx : bx - ax;
        const dirZ = fromEnd ? az - bz : bz - az;
        const len = Math.hypot(dirX, dirZ) || 1;
        const laneW = Number(edge.lane_width) || LANE_WIDTH;
        const hw = ((Number(edge.lanes) || DEFAULT_LANES) * laneW) / 2;
        arms.push({ dx: dirX / len, dz: dirZ / len, hw });
      }
      if (arms.length >= 2) {
        // 边界点（THREE 平面）：center + dir*radius ± 法线*hw
        const pts = [];
        for (const a of arms) {
          const pxn = -a.dz, pzn = a.dx;           // XZ 平面右法线
          const bx = c.x + a.dx * c.radius + pxn * a.hw;
          const bz = c.z + a.dz * c.radius + pzn * a.hw;
          const tx = c.x + a.dx * c.radius - pxn * a.hw;
          const tz = c.z + a.dz * c.radius - pzn * a.hw;
          pts.push({ x: bx, z: bz, ang: Math.atan2(bz - c.z, bx - c.x) }); // exempt: 排序用，非 heading 计算
          pts.push({ x: tx, z: tz, ang: Math.atan2(tz - c.z, tx - c.x) }); // exempt: 排序用，非 heading 计算
        }
        pts.sort((a, b) => a.ang - b.ang);
        const base = polyPositions.length / 3;
        for (const p of pts) {
          polyPositions.push(p.x, PATCH_Y, p.z);
        }
        // 扇形三角化（中心 → 相邻顶点，凸多边形成立）
        for (let i = 1; i < pts.length - 1; i++) {
          polyIndices.push(base, base + i, base + i + 1);
        }
      }

      // ── 斑马线 + 停止线：遍历关联该路口的 edge，取靠近路口的端点方向 ──
      for (const [edgeId, entry] of byId) {
        if (entry.start !== ci && entry.end !== ci) continue;
        const edge = edgeMap.get(edgeId);
        if (!edge || !Array.isArray(edge.nodes) || edge.nodes.length < 2) continue;
        const nodes = edge.nodes.map((n) =>
          Array.isArray(n) ? n : [n.x || 0, n.y || 0, n.z || 0]);
        const fromEnd = entry.end === ci;
        const aIdx = fromEnd ? nodes.length - 2 : 0;
        const bIdx = fromEnd ? nodes.length - 1 : 1;
        /* ENU [x,y,z] → THREE 走 worldToThree（门禁强制，禁止裸 -y 翻转） */
        const [ax, , az] = worldToThree(Number(nodes[aIdx][0]) || 0, Number(nodes[aIdx][1]) || 0, Number(nodes[aIdx][2]) || 0);
        const [bx, , bz] = worldToThree(Number(nodes[bIdx][0]) || 0, Number(nodes[bIdx][1]) || 0, Number(nodes[bIdx][2]) || 0);
        // 从路口中心指向本 edge 的**向外切线**：取路口端最后一小段的切线。
        // 旧实现 !fromEnd 用 a-center（a 恰在路口中心，≈零向量→方向退化）、
        // fromEnd 用 center-a（指向路口内，方向反）→ 斑马线/停止线朝向随机乱。
        const dirX = fromEnd ? ax - bx : bx - ax;
        const dirZ = fromEnd ? az - bz : bz - az;
        const len = Math.hypot(dirX, dirZ) || 1;
        const ux = dirX / len, uz = dirZ / len;
        // 斑马线中心：在路口多边形**外侧**的路面上（跨整幅路），不贴 patch 边缘
        const cx = c.x + ux * (c.radius + 2.0);
        const cz = c.z + uz * (c.radius + 2.0);
        // GB 5768.3 5.8：斑马线条纹应与道路中心线平行（与车同向），条纹沿道路
        // 长度短（交叉口 ≥2m），跨道路方向铺多条条带。旧实现条纹长轴垂直道路
        // （横跨整幅路面）→ 观感像给车走的横条，方向反了。
        const laneW = Number(edge.lane_width) || LANE_WIDTH;
        const roadW = (Number(edge.lanes) || DEFAULT_LANES) * laneW + 0.6;
        const rotY = directionToRotationY(ux, uz);   // 条纹长轴沿道路（平行行车）
        const step = CROSSWALK_STRIPE_W + CROSSWALK_GAP;
        const stripeCount = Math.max(2, Math.round(roadW / step));
        const nx = -uz, nz = ux;                     // 跨道路方向（铺条）
        for (let i = 0; i < stripeCount; i++) {
          const across = (i - (stripeCount - 1) / 2) * step;
          crossInstances.push({
            x: cx + nx * across,
            z: cz + nz * across,
            rotY,
            len: CROSSWALK_LENGTH,
            w: CROSSWALK_STRIPE_W,
          });
        }
        // 停止线：GB 5768.3 5.19 白色实线横跨**来车方向半幅**路面（垂直行车），
        // 位于斑马线外侧（来向更远一点）。来向 = -u，右行制下来车半幅在来向右侧
        // （THREE 方向 (ux,uz) 的右侧 = (-uz,ux)；来向 -u 的右侧 = (uz,-ux)）。
        const stopDist = c.radius + 2.0 + CROSSWALK_LENGTH + 1.5;
        const halfW = roadW * 0.25;
        stopInstances.push({
          x: c.x + ux * stopDist + uz * halfW,
          z: c.z + uz * stopDist - ux * halfW,
          rotY: rotY + Math.PI / 2,
          len: roadW * 0.5,
          w: STOP_LINE_W,
        });
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
  }

  /** 2c. 转向连接曲线：按 fork junctions[] 的 connecting_roads[].turn 画车辆转向
   *  路径（左转向左弯/右转向右弯/直行平直），颜色区分转向类型。仅作连通可视化
   *  提示，不参与碰撞/几何。坐标走 worldToThree（与 JunctionDetect 一致）。 */
  const TURN_COLOR = { straight: 0xffffff, left: 0xffcc33, right: 0x66ff99, uturn: 0xff6633 };

  function _nodePoint(node) {
    if (!node) return null;
    if (Array.isArray(node)) {
      const [x, , z] = worldToThree(node[0] || 0, node[1] || 0, node[2] || 0);
      return { x, z };
    }
    if (typeof node === 'object') {
      const [x, , z] = worldToThree(node.x || 0, node.y || 0, node.z || 0);
      return { x, z };
    }
    return null;
  }

  /** edge 在路口端的端点（THREE XZ），useEnd=true 取末节点，否则首节点。 */
  function _edgeEndpoint(edge, useEnd) {
    const nodes = edge.nodes;
    if (!Array.isArray(nodes) || nodes.length < 2) return null;
    return _nodePoint(useEnd ? nodes[nodes.length - 1] : nodes[0]);
  }

  /** 由折线生成扁平 ribbon（XZ 平面，左法线偏移），合批成单 mesh 加入 group。 */
  function _ribbonFromPoints(points, halfW, y, color) {
    if (points.length < 2) return;
    const positions = [];
    for (let i = 0; i < points.length; i++) {
      const prev = points[Math.max(0, i - 1)];
      const next = points[Math.min(points.length - 1, i + 1)];
      let tx = next.x - prev.x, tz = next.z - prev.z;
      const tl = Math.hypot(tx, tz) || 1; tx /= tl; tz /= tl;
      const nx = -tz, nz = tx;   // XZ 左法线
      positions.push(points[i].x + nx * halfW, y, points[i].z + nz * halfW);
      positions.push(points[i].x - nx * halfW, y, points[i].z - nz * halfW);
    }
    const indices = [];
    for (let i = 0; i < points.length - 1; i++) {
      const a = i * 2, b = i * 2 + 1, c = (i + 1) * 2, d = (i + 1) * 2 + 1;
      indices.push(a, b, c, b, d, c);
    }
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
    geo.setIndex(indices);
    geo.computeVertexNormals();
    const mesh = new THREE.Mesh(geo, getStdMaterial(color, 0.7, 0.0));
    mesh.receiveShadow = true;
    group.add(mesh);
  }

  /** 二次贝塞尔：p0(进路口端) → 控制点(按转向弯) → p1(出路口端)，采样成 ribbon。 */
  function _drawTurnCurve(p0, p1, turn, color) {
    const mx = (p0.x + p1.x) / 2, mz = (p0.z + p1.z) / 2;
    const dx = p1.x - p0.x, dz = p1.z - p0.z;
    const len = Math.hypot(dx, dz) || 1;
    const nx = -dz / len, nz = dx / len;   // 左法线
    const bend = 0.45 * len;
    let ctrl;
    if (turn === 'left') ctrl = { x: mx + nx * bend, z: mz + nz * bend };
    else if (turn === 'right') ctrl = { x: mx - nx * bend, z: mz - nz * bend };
    else ctrl = { x: mx, z: mz };          // straight / uturn → 平直
    const N = 14, pts = [];
    for (let i = 0; i <= N; i++) {
      const t = i / N, u = 1 - t;
      pts.push({
        x: u * u * p0.x + 2 * u * t * ctrl.x + t * t * p1.x,
        z: u * u * p0.z + 2 * u * t * ctrl.z + t * t * p1.z,
      });
    }
    _ribbonFromPoints(pts, 0.18, 0.14, color);
  }

  function _buildTurnConnectors(junctions, roadNetwork, centers, byId) {
    const edges = roadNetwork.edges || [];
    const edgeMap = new Map(edges.map((e) => [String(e.id), e]));
    for (const fork of junctions) {
      if (fork.type !== 'fork') continue;
      const inId = String(fork.incoming_road);
      const inEntry = byId.get(inId);
      if (!inEntry) continue;
      const ci = inEntry.start != null ? inEntry.start : inEntry.end;
      if (ci == null) continue;
      const inEdge = edgeMap.get(inId);
      if (!inEdge) continue;
      const p0 = _edgeEndpoint(inEdge, inEntry.end === ci);
      if (!p0) continue;
      for (const conn of (fork.connecting_roads || [])) {
        const cId = String(conn.id);
        const cEntry = byId.get(cId);
        if (!cEntry) continue;
        if (!(cEntry.start === ci || cEntry.end === ci)) continue;
        const cEdge = edgeMap.get(cId);
        if (!cEdge) continue;
        const p1 = _edgeEndpoint(cEdge, cEntry.end === ci);
        if (!p1) continue;
        const turn = conn.turn || 'straight';
        _drawTurnCurve(p0, p1, turn, TURN_COLOR[turn] || TURN_COLOR.straight);
      }
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

  /** 4. ViaductPier：高架桥墩（每隔 20m 一根圆柱） */
  function _buildViaductPier(edge) {
    const elev = edge.elevation_profile || [];
    if (elev.length === 0) return;
    const start = _getEdgeStart(edge);
    if (!start) return;

    const pierMat = getStdMaterial(PIER_COLOR, 0.9, 0.0);
    const pierGeo = getCylinder(0.6, 0.8, 1);  // 高度 1，后续按 elevation 缩放
    let prevS = 0, prevH = elev[0].h || 0;

    // 沿 edge 每 20m 放一根桥墩，高度按 elevation_profile 插值
    const totalLen = edge.length_m || 100;
    for (let s = 10; s < totalLen; s += 20) {
      // 找到 s 对应的 elevation
      let h = prevH;
      for (let i = 1; i < elev.length; i++) {
        if (s <= elev[i].s) {
          const t = (s - prevS) / (elev[i].s - prevS || 1);
          h = prevH + (elev[i].h - prevH) * t;
          break;
        }
        prevS = elev[i].s; prevH = elev[i].h;
      }
      if (h < 0.5) continue;  // 低于 0.5m 不需要桥墩

      const pier = new THREE.Mesh(pierGeo, pierMat);
      pier.position.set(start.x + s, h / 2, start.z);
      pier.scale.y = h;
      pier.castShadow = true;
      pier.receiveShadow = true;
      group.add(pier);
    }
  }

  /** 5. BarrierEndCap：防撞桶（红白圆柱）放在 edge 起止点 */
  function _buildBarrierEndCap(edge) {
    const start = _getEdgeStart(edge);
    const end = _getEdgeEnd(edge);
    if (!start || !end) return;

    const width = (edge.lanes || DEFAULT_LANES) * (edge.lane_width || LANE_WIDTH);
    [start, end].forEach(pos => {
      // 红白两段圆柱
      const redGeo = getCylinder(0.3, 0.35, 0.4);
      const redMat = getStdMaterial(BARREL_RED, 0.6, 0.1);
      const red = new THREE.Mesh(redGeo, redMat);
      red.position.set(pos.x, 0.3, pos.z + width / 2 + 0.5);
      red.castShadow = true;
      group.add(red);

      const whiteGeo = getCylinder(0.3, 0.3, 0.3);
      const whiteMat = getStdMaterial(BARREL_WHITE, 0.6, 0.1);
      const white = new THREE.Mesh(whiteGeo, whiteMat);
      white.position.set(pos.x, 0.75, pos.z + width / 2 + 0.5);
      group.add(white);
    });
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
