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
import { getTopology } from '../model/TopologyModel.js';

const SIGN_POLE_H = 1.2;
const SIGN_PLATE = 0.35;
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
    if (!centers.length) return;

    // ── 停止标志（Stop Sign）：每个路口边上放 1~2 个 ──
    const stopSigns = []; // {x, z, rotY}
    for (let ci = 0; ci < centers.length; ci++) {
      const c = centers[ci];
      // 隔一个路口放一对（避免全城密密麻麻）
      if (ci % 3 !== 0) continue;
      const offset = 4.0;
      stopSigns.push({ x: c.x + offset, z: c.z, rotY: 0 });
      stopSigns.push({ x: c.x - offset, z: c.z, rotY: Math.PI });
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

    // 停止标志：红色圆盘（用 Box 近似）+ 杆
    const poleMat = getStdMaterial(0x888888, 0.6, 0.3);
    const signMat = getStdMaterial(0xcc2222, 0.4, 0.1);
    if (stopSigns.length) {
      const poleGeo = getCylinder(0.025, 0.03, SIGN_POLE_H, 6);
      const signGeo = getBox(SIGN_PLATE, SIGN_PLATE, 0.03);
      const poleMesh = new THREE.InstancedMesh(poleGeo, poleMat, stopSigns.length);
      const signMesh = new THREE.InstancedMesh(signGeo, signMat, stopSigns.length);
      const dummy = new THREE.Object3D();
      stopSigns.forEach((s, i) => {
        dummy.position.set(s.x, SIGN_POLE_H / 2, s.z);
        dummy.updateMatrix();
        poleMesh.setMatrixAt(i, dummy.matrix);
        dummy.position.set(s.x, SIGN_POLE_H, s.z);
        dummy.rotation.y = s.rotY;
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
