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

import { sampleEdgeNodes } from '../math/Curve.js';
import { getStdMaterial } from '../core/AssetFactory.js';
import { LANE_WIDTH, EDGE_TYPE } from '../core/Constants.js';
import { tangentToNormal } from '../math/Coord.js';

const TREE_SPACING = 30;
const TREE_PHASE   = 5;
const TREE_OFFSET  = 3.0;
const TRUNK_H      = 3.2;
const TRUNK_R      = 0.14;
const CANOPY_R     = 1.65;

const COLOR_TRUNK  = 0x5c3a1e;
const COLOR_CANOPY_LOWER = 0x245f2a;
const COLOR_CANOPY_UPPER = 0x3d8035;

export function inferTreeSlots(roadNetwork) {
  const slots = [];
  if (!roadNetwork?.edges?.length) return slots;

  for (const edge of roadNetwork.edges) {
    if (edge.type === EDGE_TYPE.VIADUCT_HIGHWAY || edge.name === EDGE_TYPE.VIADUCT_HIGHWAY) continue;

    let nodes = edge.nodes;
    if (!nodes || nodes.length < 2) continue;
    if (nodes[0] && typeof nodes[0] === 'object' && !Array.isArray(nodes[0])) {
      nodes = nodes.map(n => [n.x || 0, n.y || 0, n.z || 0]);
    }

    const points = sampleEdgeNodes(nodes, 24);
    const lanes = edge.lanes || 2;
    const laneWidth = edge.lane_width || LANE_WIDTH;
    const halfWidth = (lanes * laneWidth) / 2;

    const spine = [];
    for (let i = 0; i < points.length; i += 3) {
      const px = points[i], py = points[i + 1], pz = points[i + 2];
      let tx = 1, tz = 0;
      if (i + 6 < points.length) { tx = points[i + 3] - px; tz = points[i + 5] - pz; }
      else if (i >= 3) { tx = px - points[i - 3]; tz = pz - points[i - 2]; }
      const [nx, nz] = tangentToNormal(tx, tz);
      spine.push({ px, py, pz, nx, nz, cum: 0 });
    }
    if (spine.length < 2) continue;

    for (let i = 1; i < spine.length; i++) {
      const dx = spine[i].px - spine[i - 1].px;
      const dz = spine[i].pz - spine[i - 1].pz;
      spine[i].cum = spine[i - 1].cum + Math.sqrt(dx * dx + dz * dz);
    }
    const totalLen = spine[spine.length - 1].cum;
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