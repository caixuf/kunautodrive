/**
 * JunctionView.js — 路口铺面 + 人行横道 + 停止线（阶段3 路口渲染）
 *
 * @deprecated superseded-by=ConnectorView._buildJunctionPatch remove-by=2026-09-16
 *
 * 2026-08-16：ConnectorView._buildJunctionPatch 已是本功能的超集
 * （Chaikin 圆角化 + InstancedMesh 高效批处理 + 来车停止线归属 +
 * 左转导流虚线段 + 几何一致性过滤），故本视图停用。
 * SceneDirector 中对应的 'junction' safeCall 已移除，不再调用 build。
 * 源码保留至 2026-09-16 作为参考，后续删除。
 *
 * 背景（为何需要）：之前路口只靠路面 ribbon 贯穿 + 家具/标线在路口边界停，
 * 没有独立的路口铺面。两条垂直路在中心交叉成「+」，四角（路缘之间、对角方向）
 * 是空地，俯视露出地面/草地缝隙，即用户 2026-08-14 报的「烂路口 / 毛边」。
 *
 * 方案（移植自 osm2streets 交叉口独立铺面多边形思路，JS 自研，不引入 WASM）：
 *   1. 路口铺面：以路口中心为顶点，对每条 arm 在路口半径处的路缘断面（左右缘点）
 *      做「绕中心极角排序 + 扇形三角化」，铺一块沥青色多边形，盖住四角缝隙、
 *      且与贯穿的路面同色无缝。铺面 y 略高于路面（Y_JUNCTION），盖住接缝 z-fight。
 *   2. 人行横道（斑马线）：每个 arm 口部一段沿路向的白色条纹带，条纹垂直车流。
 *   3. 停止线：每个 arm 口部横跨路宽的白色实线（在横道外侧）。
 *
 * 数据来源（单一事实源）：TopologyModel.getTopology(roadNetwork) 的
 *   centers[]（路口中心 + radius）+ armsOfJunction(ci)（每路口辐射出的道路
 *   arm：折线点 + 半宽）。与 RoadView / TreeView / BarrierView 共享同一次拓扑
 *   计算，不另起计算。路口铺面同样消费 arms 的 spine 走向，与路对齐。
 *
 * 坐标：centers / arms.pts 均已在 THREE 坐标系（x=east, z=-north）。
 *
 * 安全：单路口几何构造 try/catch 包裹，任一路口异常只跳过该路口，不影响其他；
 * 无路口 / 无 arm 时整体安全返回。
 */

import { getTopology, walkFromJunction } from '../model/TopologyModel.js';
import { mergeGeometries } from '../math/GeometryMerge.js';
import { SCENE } from '../theme/tokens.js';
import { directionToRotationY } from '../math/Coord.js';

const ASPHALT_COLOR = SCENE.asphalt;
const LINE_WHITE = SCENE.lineWhite;

/* y 分层（与 RoadView 对齐）：路面 0.10 / 路口铺面 0.106 / 标线 0.13。
 * 铺面略高于路面盖住接缝；横道/停止线在标线层，恒在铺面之上。 */
const Y_JUNCTION = 0.106;
const Y_MARK = 0.13;

/* 路口铺面 polygonOffset：极轻微负偏移，确保盖在贯穿路面之上不闪。 */
const JunctionPatchMaterial = () => new THREE.MeshStandardMaterial({
  color: ASPHALT_COLOR,
  roughness: 0.9,
  metalness: 0.02,
  side: THREE.DoubleSide,
  polygonOffset: true, polygonOffsetFactor: -1, polygonOffsetUnits: -1,
});

/* 路口标线（横道/停止线）材质：复用 RoadView 车道线同款 polygonOffset（-2），
 * 与车道标线恒居路面之上、不 z-fight。 */
const JunctionLineMaterial = () => new THREE.MeshStandardMaterial({
  color: LINE_WHITE,
  roughness: 0.6,
  metalness: 0.05,
  side: THREE.DoubleSide,
  polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -2,
});

/* 斑马线参数 */
const CROSSWALK_STRIPE_W = 0.5;   // 单条条纹沿路向宽 (m)
const CROSSWALK_GAP = 0.5;       // 条纹间隔 (m)
const CROSSWALK_INNER = 0.5;     // 横道带内沿：距中心 0.5×radius
const CROSSWALK_OUTER = 0.92;    // 横道带外沿：距中心 0.92×radius
const STOP_LINE_W = 0.3;         // 停止线沿路向宽 (m)
const STOP_LINE_R = 0.95;        // 停止线位置：0.95×radius（横道最外侧）

/** 由路口中心 + 各 arm 路缘断面点（绕中心极角排序）扇形三角化出铺面几何。
 *  mouths: [{x,z}, ...]（已按极角排序，星形多边形）。返回 BufferGeometry 或 null。 */
function fanJunctionPatch(center, mouths, yOff) {
  if (mouths.length < 3) return null;
  const positions = [center.x, yOff, center.z];
  const uvs = [0, 0];
  for (const m of mouths) { positions.push(m.x, yOff, m.z); uvs.push(0, 0); }
  const n = mouths.length;
  const indices = [];
  for (let i = 1; i <= n - 1; i++) indices.push(0, i, i + 1);
  const geo = new THREE.BufferGeometry();
  geo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
  geo.setAttribute('uv', new THREE.Float32BufferAttribute(uvs, 2));
  geo.setIndex(indices);
  geo.computeVertexNormals();
  return geo;
}

/** 在单个 arm 口部生成斑马线 + 停止线几何，push 到 out（白线几何数组）。
 *  arm: {pts, fromEnd, hw}；center: {x,z}；radius: 路口半径。 */
function buildArmCrosswalk(arm, center, radius, out) {
  if (!arm || !Array.isArray(arm.pts) || arm.pts.length < 2) return;
  const hw = Number(arm.hw) || 3.5;
  const a = walkFromJunction(arm.pts, arm.fromEnd, center.x, center.z, radius);
  if (!a) return;
  const dirx = a.ux, dirz = a.uz;       // 远离路口的局部切线（车流方向）
  const nx = -dirz, nz = dirx;          // 垂直于车流（横跨路宽方向）
  const base_x = a.x, base_z = a.z;     // 路缘断面锚点（半径处）

  // ── 斑马线：沿路向 [INNER, OUTER]×radius 逐条纹 ──
  const bandLen = (CROSSWALK_OUTER - CROSSWALK_INNER) * radius;
  const step = CROSSWALK_STRIPE_W + CROSSWALK_GAP;
  for (let s = 0; s + CROSSWALK_STRIPE_W * 0.5 < bandLen; s += step) {
    const c0 = CROSSWALK_INNER * radius + s;
    const c1 = c0 + CROSSWALK_STRIPE_W;
    const ax = base_x + dirx * c0, az = base_z + dirz * c0;
    const bx = base_x + dirx * c1, bz = base_z + dirz * c1;
    // 条纹横跨路宽：两端 ±normal*hw
    out.push(quadGeo(
      ax + nx * hw, az + nz * hw,
      ax - nx * hw, az - nz * hw,
      bx - nx * hw, bz - nz * hw,
      bx + nx * hw, bz + nz * hw,
      Y_MARK,
    ));
  }

  // ── 停止线：横道最外侧横跨路宽实线 ──
  const sr = STOP_LINE_R * radius;
  const sx = base_x + dirx * sr, sz = base_z + dirz * sr;
  out.push(quadGeo(
    sx + nx * hw, sz + nz * hw,
    sx - nx * hw, sz - nz * hw,
    sx - nx * hw + dirx * STOP_LINE_W, sz - nz * hw + dirz * STOP_LINE_W,
    sx + nx * hw + dirx * STOP_LINE_W, sz + nz * hw + dirz * STOP_LINE_W,
    Y_MARK,
  ));
}

/** 由 4 个角点（顺时针/逆时针均可，双三角）生成一个 quad 几何（白线，y=Y_MARK）。 */
function quadGeo(x0, z0, x1, z1, x2, z2, x3, z3, y) {
  const positions = [
    x0, y, z0, x1, y, z1, x2, y, z2, x3, y, z3,
  ];
  const uvs = [0, 0, 1, 0, 1, 1, 0, 1];
  const indices = [0, 2, 1, 1, 2, 3];
  const geo = new THREE.BufferGeometry();
  geo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
  geo.setAttribute('uv', new THREE.Float32BufferAttribute(uvs, 2));
  geo.setIndex(indices);
  geo.computeVertexNormals();
  return geo;
}

export function createJunctionView(scene) {
  const group = new THREE.Group();
  scene.add(group);
  let built = false;
  let stats = { junctions: 0, crosswalks: 0 };

  function clear() {
    while (group.children.length) {
      const c = group.children[0];
      group.remove(c);
      if (c.geometry) c.geometry.dispose();
      if (c.material) c.material.dispose();
    }
    built = false;
    stats = { junctions: 0, crosswalks: 0 };
  }

  function build(roadNetwork) {
    clear();
    const topo = getTopology(roadNetwork);
    if (!topo || !Array.isArray(topo.centers) || !topo.centers.length) return;

    const patchGeos = [];
    const lineGeos = [];
    let junctionCount = 0;
    let crosswalkCount = 0;

    for (let ci = 0; ci < topo.centers.length; ci++) {
      const c = topo.centers[ci];
      if (!c || !(c.radius > 0)) continue;
      let arms;
      try {
        arms = topo.armsOfJunction(ci);
      } catch (e) {
        console.warn('[JunctionView] armsOfJunction 失败，跳过路口', ci, e);
        continue;
      }
      if (!arms || arms.length < 2) continue;   // 非交叉口（<2 arm）不画铺面

      // 各 arm 在路口半径处的路缘断面（左/右缘点）
      const mouths = [];
      let ok = true;
      for (const arm of arms) {
        if (!arm || !Array.isArray(arm.pts) || arm.pts.length < 2) { ok = false; break; }
        const hw = Number(arm.hw) || 3.5;
        let a;
        try {
          a = walkFromJunction(arm.pts, arm.fromEnd, c.x, c.z, c.radius);
        } catch (e) { ok = false; break; }
        if (!a) { ok = false; break; }
        const dirx = a.ux, dirz = a.uz;
        const nx = -dirz, nz = dirx;
        mouths.push({ x: a.x + nx * hw, z: a.z + nz * hw });
        mouths.push({ x: a.x - nx * hw, z: a.z - nz * hw });
      }
      if (!ok || mouths.length < 3) continue;

      // 绕中心极角排序 → 星形多边形（中心必在其内部）
      const polar = (p) => directionToRotationY(p.x - c.x, p.z - c.z);
      mouths.sort((p, q) => polar(p) - polar(q));

      try {
        const patch = fanJunctionPatch({ x: c.x, z: c.z }, mouths, Y_JUNCTION);
        if (patch) patchGeos.push(patch);
      } catch (e) {
        console.warn('[JunctionView] 铺面三角化失败，跳过', ci, e);
      }

      // 每个 arm 的斑马线 + 停止线
      for (const arm of arms) {
        try {
          const before = lineGeos.length;
          buildArmCrosswalk(arm, { x: c.x, z: c.z }, c.radius, lineGeos);
          if (lineGeos.length > before) crosswalkCount++;
        } catch (e) {
          console.warn('[JunctionView] 横道生成失败，跳过 arm', ci, e);
        }
      }
      junctionCount++;
    }

    // ── 合批 + 上材质 ──
    if (patchGeos.length) {
      const merged = mergeGeometries(patchGeos);
      patchGeos.forEach(g => g.dispose && g.dispose());
      if (merged) {
        const mesh = new THREE.Mesh(merged, JunctionPatchMaterial());
        mesh.receiveShadow = true;
        group.add(mesh);
      }
    }
    if (lineGeos.length) {
      const merged = mergeGeometries(lineGeos);
      lineGeos.forEach(g => g.dispose && g.dispose());
      if (merged) {
        group.add(new THREE.Mesh(merged, JunctionLineMaterial()));
      }
    }

    built = true;
    stats = { junctions: junctionCount, crosswalks: crosswalkCount };
  }

  function isBuilt() { return built; }
  function getStats() { return { ...stats }; }
  function getGroup() { return group; }

  return { build, clear, isBuilt, getStats, getGroup };
}
