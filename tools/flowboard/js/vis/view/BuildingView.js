/**
 * BuildingView.js — 道路两侧城市楼群
 *
 * 2 个 InstancedMesh（楼体 + 屋顶），共享一张程序生成窗格贴图。
 * 无外网资产；性能档只改变实例密度，不改变 draw call。
 */

import { sampleEdgeNodes } from '../math/Curve.js';
import { directionToRotationY, tangentToNormal } from '../math/Coord.js';
import { EDGE_TYPE, LANE_WIDTH } from '../core/Constants.js';
import { getCityModel, initCityModelCache } from '../../models.js';

const SPACING = { high: 55, medium: 80, low: 120 };
// 楼体完整落在路肩外的草地上，并给行道树/人行空间留出视觉缓冲。
const SETBACK = 32;

/* 城市建筑 glTF 资产（Quaternius Downtown City MegaKit, CC0）。
 * 模型缺失时回退到程序化 Box 楼体（原实现），保证无外网资产行为不变。
 * glTF 建筑尺寸/朝向各异，摆放前按目标高度归一化缩放。 */
const CITY_BUILDING_NAMES = ['city_a', 'city_b', 'city_c'];

function buildingTexture() {
  const canvas = document.createElement('canvas');
  canvas.width = 64;
  canvas.height = 128;
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#273446';
  ctx.fillRect(0, 0, 64, 128);
  for (let y = 7; y < 124; y += 14) {
    for (let x = 6; x < 62; x += 14) {
      const lit = ((x * 7 + y * 13) % 5) !== 0;
      ctx.fillStyle = lit ? '#9ec7dc' : '#182433';
      ctx.fillRect(x, y, 8, 7);
      ctx.fillStyle = lit ? '#d7eef6' : '#223142';
      ctx.fillRect(x + 1, y + 1, 6, 2);
    }
  }
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.wrapS = texture.wrapT = THREE.RepeatWrapping;
  texture.anisotropy = 2;
  return texture;
}

export function createBuildingView(scene) {
  const group = new THREE.Group();
  scene.add(group);

  function clear() {
    while (group.children.length) {
      const child = group.children[0];
      group.remove(child);
      if (child.geometry) child.geometry.dispose();
      if (child.material) {
        if (child.material.map) child.material.map.dispose();
        child.material.dispose();
      }
    }
  }

  /** 把一个城市建筑 glTF 克隆按目标高度归一化后放到指定位置。
   *  返回 true 表示已用真实模型，false 表示该资产缺失（调用方回退 Box）。 */
  function placeCityModel(name, slot) {
    const model = getCityModel(name);
    if (!model) return false;
    // 归一化：按源模型包围盒高度缩放到目标高度，底部对齐地面。
    const box = new THREE.Box3().setFromObject(model);
    const srcH = Math.max(box.max.y - box.min.y, 1e-6);
    const scale = slot.height / srcH;
    model.scale.set(scale, scale, scale);
    // Box3 含模型原点偏移：重新计算缩放后底部，确保落在地面（y=0）。
    const scaledBox = new THREE.Box3().setFromObject(model);
    model.position.set(slot.x, -scaledBox.min.y, slot.z);
    model.rotation.y = slot.rotation;
    model.castShadow = true;
    model.receiveShadow = true;
    group.add(model);
    return true;
  }

  let cityLoadStarted = false;
  let lastRoadNetwork = null;
  let lastStore = null;

  function build(roadNetwork, store = {}) {
    /* 首次 build 时启动城市模型加载（fire-and-forget）。加载完成后若
     * 路网未变则重建一次以切换到 glTF 建筑；否则下次 build 自然使用。 */
    if (!cityLoadStarted) {
      cityLoadStarted = true;
      lastRoadNetwork = roadNetwork;
      lastStore = store;
      initCityModelCache().then(function() {
        if (lastRoadNetwork) build(lastRoadNetwork, lastStore || {});
      });
    } else {
      lastRoadNetwork = roadNetwork;
      lastStore = store;
    }
    clear();
    if (!roadNetwork || !Array.isArray(roadNetwork.edges)) return;
    const tier = store.perfTier || 'high';
    const spacing = SPACING[tier] || SPACING.high;
    const slots = [];

    for (const edge of roadNetwork.edges) {
      if (edge.type === EDGE_TYPE.VIADUCT_HIGHWAY ||
          edge.name === EDGE_TYPE.VIADUCT_HIGHWAY) continue;
      const raw = Array.isArray(edge.nodes) ? edge.nodes : [];
      if (raw.length < 2) continue;
      const nodes = raw.map((n) => Array.isArray(n) ? n : [n.x || 0, n.y || 0, n.z || 0]);
      const points = sampleEdgeNodes(nodes, 18);
      const lanes = edge.lanes || 2;
      const laneWidth = edge.lane_width || LANE_WIDTH;
      const offset = lanes * laneWidth / 2 + SETBACK;

      let distance = 0;
      for (let i = 3; i < points.length; i += 3) {
        const px = points[i], pz = points[i + 2];
        const prevX = points[i - 3], prevZ = points[i - 1];
        const segment = Math.hypot(px - prevX, pz - prevZ);
        distance += segment;
        if (distance < spacing) continue;
        distance = 0;
        const [nx, nz] = tangentToNormal(px - prevX, pz - prevZ);
        const hash = Math.abs(Math.trunc(px * 17 + pz * 31)) % 997;
        const seed = hash / 997;
        const height = 18 + seed * 42;
        const width = 12 + seed * 8;
        const depth = 10 + (1 - seed) * 8;
        for (const side of [-1, 1]) {
          slots.push({
            x: px + nx * offset * side,
            z: pz + nz * offset * side,
            height, width, depth, seed,
            rotation: directionToRotationY(nx, nz),
          });
        }
      }
    }
    if (!slots.length) return;

    /* 优先用城市 glTF 建筑（若任一已加载）。逐 slot 尝试真实模型，
     * 缺失的 slot 落到 Box 兜底——模型文件不存在时整体回退到
     * 程序化贴图楼体，保证无外网资产时视觉与旧版一致。 */
    const useCity = CITY_BUILDING_NAMES.some((n) => getCityModel(n) !== null);
    if (useCity) {
      for (const slot of slots) {
        const name = CITY_BUILDING_NAMES[Math.floor(slot.seed * CITY_BUILDING_NAMES.length) % CITY_BUILDING_NAMES.length];
        if (!placeCityModel(name, slot)) {
          // 单个模型缺失：落 Box（见下方 Box 路径），这里按目标尺寸补一个简单楼体。
          const bodyGeo = new THREE.BoxGeometry(slot.depth, slot.height, slot.width);
          const facade = new THREE.MeshStandardMaterial({ color: 0x8998a8, roughness: 0.78, metalness: 0.04 });
          const mesh = new THREE.Mesh(bodyGeo, facade);
          mesh.position.set(slot.x, slot.height / 2, slot.z);
          mesh.rotation.y = slot.rotation;
          group.add(mesh);
        }
      }
      return;
    }

    const facade = new THREE.MeshStandardMaterial({
      color: 0x8998a8,
      roughness: 0.78,
      metalness: 0.04,
      map: buildingTexture(),
    });
    const roof = new THREE.MeshStandardMaterial({
      color: 0x4a5664,
      roughness: 0.9,
    });
    const bodyGeo = new THREE.BoxGeometry(1, 1, 1);
    const roofGeo = new THREE.BoxGeometry(1, 1, 1);
    const bodies = new THREE.InstancedMesh(bodyGeo, facade, slots.length);
    const roofs = new THREE.InstancedMesh(roofGeo, roof, slots.length);
    const dummy = new THREE.Object3D();

    slots.forEach((slot, i) => {
      dummy.position.set(slot.x, slot.height / 2, slot.z);
      dummy.rotation.set(0, slot.rotation, 0);
      dummy.scale.set(slot.depth, slot.height, slot.width);
      dummy.updateMatrix();
      bodies.setMatrixAt(i, dummy.matrix);

      dummy.position.set(slot.x, slot.height + 0.5, slot.z);
      dummy.scale.set(slot.depth * 0.72, 1, slot.width * 0.72);
      dummy.updateMatrix();
      roofs.setMatrixAt(i, dummy.matrix);
    });
    bodies.instanceMatrix.needsUpdate = true;
    roofs.instanceMatrix.needsUpdate = true;
    bodies.castShadow = tier === 'high';
    bodies.receiveShadow = true;
    group.add(bodies, roofs);
  }

  return { build, clear, getGroup: () => group };
}
