/**
 * TreeView.js — 行道树（沿路两侧低模树木）
 *
 * 设计：
 *   - 3 个 InstancedMesh：树干 + 双层椭球树冠，共 3 draw call
 *   - 30m 间距、道路两侧成列，并与 40m 路灯错开相位
 *   - offset = halfWidth + 3.0m（树池在路灯外侧）
 *   - 道路坐标统一消费 sampleEdgeNodes 的 THREE 坐标与 Coord 法线
 *   - 纳入 vis:check（SceneDirector 注册）
 */

import { getStdMaterial } from '../core/AssetFactory.js';
import { EDGE_TYPE } from '../core/Constants.js';
import { computeEdgeAxis } from '../model/RoadAxis.js';
import { getTopology } from '../model/TopologyModel.js';

const TREE_SPACING = 30;
const TREE_PHASE   = 5;
const TREE_OFFSET  = 3.0;
const TRUNK_H      = 3.2;
const TRUNK_R      = 0.14;
const CANOPY_R     = 1.65;

const COLOR_TRUNK  = 0x5c3a1e;
const COLOR_CANOPY_LOWER = 0x245f2a;
const COLOR_CANOPY_UPPER = 0x3d8035;

/** 点是否落在某多边形内（射线法）。多边形顶点支持 [x,y,z] / [x,z] / {x,z}；
 * 约定 z 取第三分量（x,y,z）或第二分量（x,z），与多数几何数组一致。
 * 阶段6 植被按用地落位：landuse（forest/grass 多边形）由 DSL 枢纽 net2map
 * 从 OSM landuse/natural 抽取，坐标系与 spine（THREE x,z）一致（方案 §阶段6）。 */
function pointInPolygon(x, z, poly) {
  if (!Array.isArray(poly) || poly.length < 3) return false;
  let inside = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const pi = poly[i], pj = poly[j];
    if (!pi || !pj) continue;
    const xi = Array.isArray(pi) ? pi[0] : pi.x;
    const zi = Array.isArray(pi) ? (pi.length > 2 ? pi[2] : pi[1]) : pi.z;
    const xj = Array.isArray(pj) ? pj[0] : pj.x;
    const zj = Array.isArray(pj) ? (pj.length > 2 ? pj[2] : pj[1]) : pj.z;
    const denom = (zj - zi) || 1e-9;
    const intersect = ((zi > z) !== (zj > z)) &&
      (x < (xj - xi) * (z - zi) / denom + xi);
    if (intersect) inside = !inside;
  }
  return inside;
}

export function inferTreeSlots(roadNetwork) {
  const slots = [];
  if (!roadNetwork?.edges?.length) return slots;

  /* 2026-08-14 路口避让：树不落在路口内/路口边缘（OSM 实测树长在马路和
   * 路口中间）。到最近 junction 中心距离 < radius + 2m 的槽位丢弃。
   * P0：拓扑走 TopologyModel 单一事实源（与其他 view 共享同一次计算）。 */
  const topo = getTopology(roadNetwork);
  const nearJunction = (x, z) => !!(topo && topo.nearJunction(x, z, 2.0));

  /* 阶段6 植被按用地落位（安全消费，零回归）：landuse 多边形存在时，只在
   * 绿地（forest/grass）内种树，去掉"每条路两侧必种树"的邻近启发式；
   * landuse 缺失（当前 osm_lujiazui_v2 等地图暂未抽取）→ 保持两侧都种，行为不变。
   * DSL 枢纽抽取落地后自动点亮，无需回改本 view。 */
  const landuse = Array.isArray(roadNetwork.landuse) ? roadNetwork.landuse : null;
  const inLanduse = (x, z) => !landuse || landuse.some((poly) => pointInPolygon(x, z, poly));

  for (const edge of roadNetwork.edges) {
    if (edge.type === EDGE_TYPE.VIADUCT_HIGHWAY || edge.name === EDGE_TYPE.VIADUCT_HIGHWAY) continue;

    /* 共享路轴（单一事实源）：road.centerline 是最左车道左缘，须由 computeEdgeAxis
     * 推导 TRUE 中心 spine + 车道组半宽。家具相对 TRUE 中心偏移，才不会压路 /
     * 与 RoadView 错位（此前各视图各自从 edge.nodes 建 spine、按 lane count 估半宽，
     * 路整体左偏、树/灯落于沥青上）。lane_data 缺失时 fromLanes=false、居中回退。 */
    const axis = computeEdgeAxis(edge, roadNetwork.lane_data);
    if (!axis.ok || axis.spine.length < 2) continue;
    const spine = axis.spine;
    for (let i = 0; i < spine.length; i++) spine[i].cum = axis.cum[i];
    const halfWidth = axis.halfWidth;
    const totalLen = axis.cum[axis.cum.length - 1];
    const count = Math.floor((totalLen - TREE_PHASE) / TREE_SPACING) + 1;
    if (count <= 0) continue;

    for (let i = 0; i < count; i++) {
      const targetArc = TREE_PHASE + i * TREE_SPACING;
      if (targetArc > totalLen) break;
      let j = 1;
      while (j < spine.length && spine[j].cum < targetArc) j++;
      if (j >= spine.length) j = spine.length - 1;
      const s = spine[j];
      for (const side of [-1, 1]) {
        const x = s.px + s.nx * (halfWidth + TREE_OFFSET) * side;
        const z = s.pz + s.nz * (halfWidth + TREE_OFFSET) * side;
        if (nearJunction(x, z)) continue;   // 路口避让
        if (!inLanduse(x, z)) continue;      // 阶段6：仅绿地内种树
        const variant = ((i * 17 + (side > 0 ? 7 : 3)) % 11) / 10;
        slots.push({ x, z, py: s.py, side, arc: targetArc, variant });
      }
    }
  }
  return slots;
}

export function createTreeView(scene) {
  const group = new THREE.Group();
  scene.add(group);

  let trunkMesh, lowerCanopyMesh, upperCanopyMesh;
  let stats = { trees: 0, drawCalls: 0 };

  function clear() {
    while (group.children.length) {
      const c = group.children[0];
      group.remove(c);
      if (c.geometry) c.geometry.dispose();
      if (c.material) {
        if (Array.isArray(c.material)) c.material.forEach(m => m.dispose());
        else c.material.dispose();
      }
    }
    trunkMesh = lowerCanopyMesh = upperCanopyMesh = null;
    stats = { trees: 0, drawCalls: 0 };
  }

  function build(roadNetwork) {
    clear();
    const slots = inferTreeSlots(roadNetwork);

    if (slots.length === 0) return;

    // ── 第二遍：构建 3 个 InstancedMesh ──
    const N = slots.length;
    const trunkGeo = new THREE.CylinderGeometry(TRUNK_R, TRUNK_R * 1.3, TRUNK_H, 8);
    const lowerCanopyGeo = new THREE.SphereGeometry(CANOPY_R, 8, 6);
    const upperCanopyGeo = new THREE.SphereGeometry(CANOPY_R * 0.82, 8, 6);

    const trunkMat = getStdMaterial(COLOR_TRUNK, 0.8, 0.5);
    const lowerCanopyMat = getStdMaterial(COLOR_CANOPY_LOWER, 0.78, 0.0);
    const upperCanopyMat = getStdMaterial(COLOR_CANOPY_UPPER, 0.72, 0.0);

    trunkMesh = new THREE.InstancedMesh(trunkGeo, trunkMat, N);
    lowerCanopyMesh = new THREE.InstancedMesh(lowerCanopyGeo, lowerCanopyMat, N);
    upperCanopyMesh = new THREE.InstancedMesh(upperCanopyGeo, upperCanopyMat, N);

    const dummy = new THREE.Object3D();
    for (let i = 0; i < N; i++) {
      const s = slots[i];
      const heightScale = 0.88 + s.variant * 0.24;
      const crownScale = 0.90 + s.variant * 0.18;
      const trunkH = TRUNK_H * heightScale;
      dummy.position.set(s.x, s.py + trunkH / 2, s.z); // Coord: sampled THREE coordinates
      dummy.rotation.set(0, s.variant * Math.PI * 2, 0);
      dummy.scale.set(1, heightScale, 1);
      dummy.updateMatrix();
      trunkMesh.setMatrixAt(i, dummy.matrix);

      dummy.position.set(s.x, s.py + trunkH + 0.65, s.z); // Coord: sampled THREE coordinates
      dummy.scale.set(crownScale, 0.78 * crownScale, crownScale);
      dummy.updateMatrix();
      lowerCanopyMesh.setMatrixAt(i, dummy.matrix);

      dummy.position.set(
        s.x + (s.variant - 0.5) * 0.45,
        s.py + trunkH + 2.15,
        s.z + (0.5 - s.variant) * 0.35,
      ); // Coord: sampled THREE coordinates
      dummy.scale.set(0.82 * crownScale, 0.92 * crownScale, 0.82 * crownScale);
      dummy.updateMatrix();
      upperCanopyMesh.setMatrixAt(i, dummy.matrix);
    }
    trunkMesh.instanceMatrix.needsUpdate = true;
    lowerCanopyMesh.instanceMatrix.needsUpdate = true;
    upperCanopyMesh.instanceMatrix.needsUpdate = true;

    group.add(trunkMesh, lowerCanopyMesh, upperCanopyMesh);
    stats = { trees: N, drawCalls: 3 };
  }

  function getGroup() { return group; }
  function getStats() { return { ...stats }; }

  return { build, clear, getGroup, getStats };
}