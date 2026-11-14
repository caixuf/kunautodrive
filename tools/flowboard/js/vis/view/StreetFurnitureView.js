/**
 * StreetFurnitureView.js — 城市家具占位实现
 *
 * 在路口附近放置停止标志、公交站台、垃圾桶等城市家具。
 * 使用程序化简单几何体（Box + Cylinder），零外部模型。
 * 未来可替换为 glTF 模型（OSM 数据或第三方素材）。
 *
 * 设计：所有家具用 InstancedMesh 合批，城市网格 < 3000 个路口也仅
 * 数个 draw call。
 */

import { getStdMaterial, getCylinder, getBox } from '../core/AssetFactory.js';
import { isGarageOrRampEdge } from '../core/Constants.js';
import { getTopology } from '../model/TopologyModel.js';
import { directionToRotationY, tangentToNormal, offsetAlongNormal } from '../math/Coord.js';

const SIGN_POLE_H = 2.0;
const STOP_SIGN_RADIUS = 0.36;
const SPEED_SIGN_RADIUS = 0.32;
const BUS_SHELTER_H = 2.0;
const BUS_SHELTER_W = 2.0;
const BUS_SHELTER_D = 1.0;

export function createStreetFurnitureView(scene) {
  const group = new THREE.Group();
  scene.add(group);

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
  }

  function build(roadNetwork) {
    clear();
    if (!roadNetwork || !roadNetwork.edges) return;

    /* 与其它静态 View 共享已缓存的拓扑，避免大地图重复做路口检测。 */
    const topology = getTopology(roadNetwork);
    const centers = topology ? topology.centers : [];

    // ── 停止标志（八角 STOP 牌）与限速圆牌 ──
    const stopSigns = []; // {x, z, rotY}
    const speedSigns = []; // {x, z, rotY}

    const edges = Array.isArray(roadNetwork.edges) ? roadNetwork.edges : [];
    for (let ei = 0; ei < edges.length; ei++) {
      const edge = edges[ei];
      if (isGarageOrRampEdge(edge)) continue;
      const nodes = Array.isArray(edge.nodes) ? edge.nodes : [];
      if (nodes.length < 2) continue;
      const lanes = edge.lanes || 2;
      const laneWidth = edge.lane_width || 3.5;
      const roadHalfW = (lanes * laneWidth) / 2 + 1.2;

      // 靠近路口端（倒数第二到最后一个节点）放八角 STOP 牌
      if (ei % 2 === 0) {
        const a = nodes[nodes.length - 2];
        const b = nodes[nodes.length - 1];
        const ax = Array.isArray(a) ? a[0] : (a.x || 0);
        const az = Array.isArray(a) ? -(a[1] || 0) : -(a.y || 0);
        const bx = Array.isArray(b) ? b[0] : (b.x || 0);
        const bz = Array.isArray(b) ? -(b[1] || 0) : -(b.y || 0);
        const dx = bx - ax, dz = bz - az;
        const len = Math.hypot(dx, dz);
        if (len > 5) {
          const [nx, nz] = tangentToNormal(dx, dz);
          const [sx, sz] = offsetAlongNormal(bx - dx * 0.15, bz - dz * 0.15, nx, nz, -roadHalfW);
          const rotY = directionToRotationY(-dx, -dz);
          stopSigns.push({ x: sx, z: sz, rotY });
        }
      }

      // 路段前段放限速圆牌
      if (ei % 3 === 0) {
        const a = nodes[0];
        const b = nodes[1];
        const ax = Array.isArray(a) ? a[0] : (a.x || 0);
        const az = Array.isArray(a) ? -(a[1] || 0) : -(a.y || 0);
        const bx = Array.isArray(b) ? b[0] : (b.x || 0);
        const bz = Array.isArray(b) ? -(b[1] || 0) : -(b.y || 0);
        const dx = bx - ax, dz = bz - az;
        const len = Math.hypot(dx, dz);
        if (len > 12) {
          const [nx, nz] = tangentToNormal(dx, dz);
          const [sx, sz] = offsetAlongNormal(ax + dx * 0.25, az + dz * 0.25, nx, nz, -roadHalfW);
          const rotY = directionToRotationY(-dx, -dz);
          speedSigns.push({ x: sx, z: sz, rotY });
        }
      }
    }

    // 兜底拓扑中心点
    if (!stopSigns.length && centers.length) {
      for (let ci = 0; ci < centers.length; ci++) {
        if (ci % 3 !== 0) continue;
        const c = centers[ci];
        stopSigns.push({ x: c.x + 4.0, z: c.z, rotY: 0 });
      }
    }

    // ── 公交站台（Bus Shelter）：每 5 个路口放一个 ──
    const busShelters = []; // {x, z, rotY}
    for (let ci = 0; ci < centers.length; ci++) {
      if (ci % 5 !== 0) continue;
      const c = centers[ci];
      // 沿网格方向放
      busShelters.push({ x: c.x + 6, z: c.z, rotY: 0 });
      busShelters.push({ x: c.x - 6, z: c.z, rotY: Math.PI });
    }

    // ── 渲染全部家具 ──
    const poleMat = getStdMaterial(0x888888, 0.6, 0.3);
    const dummy = new THREE.Object3D();

    // 八角 STOP 标志：八角柱 + 杆
    const stopSignMat = getStdMaterial(0xcc2222, 0.4, 0.1);
    if (stopSigns.length) {
      const poleGeo = getCylinder(0.025, 0.03, SIGN_POLE_H, 6);
      const signGeo = new THREE.CylinderGeometry(STOP_SIGN_RADIUS, STOP_SIGN_RADIUS, 0.03, 8);
      const poleMesh = new THREE.InstancedMesh(poleGeo, poleMat, stopSigns.length);
      const signMesh = new THREE.InstancedMesh(signGeo, stopSignMat, stopSigns.length);
      stopSigns.forEach((s, i) => {
        dummy.position.set(s.x, SIGN_POLE_H / 2, s.z);
        dummy.rotation.set(0, 0, 0);
        dummy.scale.set(1, 1, 1);
        dummy.updateMatrix();
        poleMesh.setMatrixAt(i, dummy.matrix);

        dummy.position.set(s.x, SIGN_POLE_H + 0.1, s.z);
        dummy.rotation.set(Math.PI / 2, 0, s.rotY);
        dummy.scale.set(1, 1, 1);
        dummy.updateMatrix();
        signMesh.setMatrixAt(i, dummy.matrix);
      });
      poleMesh.instanceMatrix.needsUpdate = true;
      signMesh.instanceMatrix.needsUpdate = true;
      group.add(poleMesh, signMesh);
    }

    // 限速圆牌：白面圆盘 + 红边
    const speedSignMat = getStdMaterial(0xeeeeee, 0.3, 0.1);
    if (speedSigns.length) {
      const poleGeo = getCylinder(0.025, 0.03, SIGN_POLE_H, 6);
      const signGeo = new THREE.CylinderGeometry(SPEED_SIGN_RADIUS, SPEED_SIGN_RADIUS, 0.025, 24);
      const poleMesh = new THREE.InstancedMesh(poleGeo, poleMat, speedSigns.length);
      const signMesh = new THREE.InstancedMesh(signGeo, speedSignMat, speedSigns.length);
      speedSigns.forEach((s, i) => {
        dummy.position.set(s.x, SIGN_POLE_H / 2, s.z);
        dummy.rotation.set(0, 0, 0);
        dummy.scale.set(1, 1, 1);
        dummy.updateMatrix();
        poleMesh.setMatrixAt(i, dummy.matrix);

        dummy.position.set(s.x, SIGN_POLE_H + 0.1, s.z);
        dummy.rotation.set(Math.PI / 2, 0, s.rotY);
        dummy.scale.set(1, 1, 1);
        dummy.updateMatrix();
        signMesh.setMatrixAt(i, dummy.matrix);
      });
      poleMesh.instanceMatrix.needsUpdate = true;
      signMesh.instanceMatrix.needsUpdate = true;
      group.add(poleMesh, signMesh);
    }

    // 公交站台：两个立柱 + 顶棚 + 背板
    if (busShelters.length) {
      const pillarGeo = getCylinder(0.04, 0.05, BUS_SHELTER_H, 6);
      const roofGeo = getBox(BUS_SHELTER_W, 0.06, BUS_SHELTER_D);
      const backGeo = getBox(BUS_SHELTER_W, BUS_SHELTER_H * 0.7, 0.03);
      const pillarMat = getStdMaterial(0x555555, 0.6, 0.4);
      const roofMat = getStdMaterial(0x4488aa, 0.5, 0.1);
      const backMat = getStdMaterial(0x88aacc, 0.7, 0.0);

      const count = busShelters.length;
      const pillarMesh = new THREE.InstancedMesh(pillarGeo, pillarMat, count * 2);
      const roofMesh = new THREE.InstancedMesh(roofGeo, roofMat, count);
      const backMesh = new THREE.InstancedMesh(backGeo, backMat, count);
      const dummy = new THREE.Object3D();
      busShelters.forEach((s, i) => {
        const hw = BUS_SHELTER_W / 2;
        // 左柱
        dummy.position.set(s.x - hw, BUS_SHELTER_H / 2, s.z);
        dummy.rotation.set(0, 0, 0);
        dummy.updateMatrix();
        pillarMesh.setMatrixAt(i * 2, dummy.matrix);
        // 右柱
        dummy.position.set(s.x + hw, BUS_SHELTER_H / 2, s.z);
        dummy.updateMatrix();
        pillarMesh.setMatrixAt(i * 2 + 1, dummy.matrix);
        // 顶棚
        dummy.position.set(s.x, BUS_SHELTER_H, s.z);
        dummy.rotation.y = s.rotY;
        dummy.updateMatrix();
        roofMesh.setMatrixAt(i, dummy.matrix);
        // 背板
        dummy.position.set(s.x, BUS_SHELTER_H * 0.35, s.z - BUS_SHELTER_D / 2);
        dummy.rotation.y = s.rotY;
        dummy.updateMatrix();
        backMesh.setMatrixAt(i, dummy.matrix);
      });
      pillarMesh.instanceMatrix.needsUpdate = true;
      roofMesh.instanceMatrix.needsUpdate = true;
      backMesh.instanceMatrix.needsUpdate = true;
      group.add(pillarMesh, roofMesh, backMesh);
    }
  }

  function getGroup() { return group; }

  return { build, clear, getGroup };
}
