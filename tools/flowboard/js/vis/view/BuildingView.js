/**
 * BuildingView.js — 道路两侧城市楼群
 *
 * 远景使用 2 个 InstancedMesh（楼体 + 屋顶），近景按预算复用真实
 * Downtown City 建筑；模型缺失时始终回退到程序化楼体。
 */

import { sampleEdgeNodes } from '../math/Curve.js';
import { directionToRotationY, tangentToNormal } from '../math/Coord.js';
import { EDGE_TYPE, LANE_WIDTH } from '../core/Constants.js';
import { getCityModel, initCityModelCache } from '../../models.js';

const SPACING = { high: 55, medium: 80, low: 120 };
// 楼体完整落在路肩外的草地上，并给行道树/人行空间留出视觉缓冲。
const SETBACK = 32;

/* ── 三排纵深天际线 ─────────────────────────────────────────────
 * 真实城市天际线的"剪影感"来自多层纵深：近排沿路楼 + 中排 + 远排高层。
 * 用三排 InstancedMesh（合并进同一批），每排 offset 递增、高度递增、
 * 饱和度递减（远景被雾冲淡、强调剪影），一处生成、一处组成 draw call，
 * 不增加渲染层复杂度。 */
const LAYERS = [
  { offsetMul: 1.0, hMin: 18, hMax: 60, spacingMul: 1.0, sat: 1.0 },  // 近排
  { offsetMul: 2.6, hMin: 40, hMax: 110, spacingMul: 0.8, sat: 0.72 }, // 中排高层
  { offsetMul: 5.0, hMin: 70, hMax: 180, spacingMul: 0.6, sat: 0.5 }, // 远景摩天楼剪影
];

/* 城市建筑 glTF 资产（Quaternius Downtown City MegaKit, CC0）。
 * 模型缺失时回退到程序化 Box 楼体（原实现），保证无外网资产行为不变。
 * glTF 建筑尺寸/朝向各异，摆放前按目标高度归一化缩放。 */
const CITY_BUILDING_NAMES = ['city_a', 'city_b', 'city_c'];
export const CITY_MODEL_BUDGET = Object.freeze({
  high: 18,
  medium: 8,
  low: 0,
});

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
  function placeCityModel(name, slot, tier) {
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
    model.traverse((child) => {
      if (!child.isMesh) return;
      child.castShadow = tier === 'high';
      child.receiveShadow = false;
      child.frustumCulled = true;
    });
    model.updateMatrixWorld(true);
    model.matrixAutoUpdate = false;
    model.userData.cityAsset = true;
    group.add(model);
    return true;
  }

  let cityLoadStarted = false;
  let lastRoadNetwork = null;
  let lastStore = null;

  function build(roadNetwork, store = {}) {
    const tier = store.perfTier || 'high';
    const cityBudget = CITY_MODEL_BUDGET[tier] || 0;
    /* 首次 build 时启动城市模型加载（fire-and-forget）。加载完成后若
     * 路网未变则重建一次以切换到 glTF 建筑；低档位不下载真实模型。 */
    if (!cityLoadStarted && cityBudget > 0) {
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
      const baseOffset = lanes * laneWidth / 2 + SETBACK;

      for (let li = 0; li < LAYERS.length; li++) {
        const layer = LAYERS[li];
        const offset = baseOffset * layer.offsetMul;
        const layerSpacing = spacing * layer.spacingMul;

        let distance = 0;
        for (let i = 3; i < points.length; i += 3) {
          const px = points[i], pz = points[i + 2];
          const prevX = points[i - 3], prevZ = points[i - 1];
          const segment = Math.hypot(px - prevX, pz - prevZ);
          distance += segment;
          if (distance < layerSpacing) continue;
          distance = 0;
          const [nx, nz] = tangentToNormal(px - prevX, pz - prevZ);
          const hash = Math.abs(Math.trunc(px * 17 + pz * 31 + li * 997)) % 997;
          const seed = hash / 997;
          const height = layer.hMin + seed * (layer.hMax - layer.hMin);
          const width = (12 + seed * 8) * (li === 0 ? 1 : 0.8);
          const depth = (10 + (1 - seed) * 8) * (li === 0 ? 1 : 0.8);
          for (const side of [-1, 1]) {
            slots.push({
              x: px + nx * offset * side,
              z: pz + nz * offset * side,
              height, width, depth, seed,
              layer: li,
              rotation: directionToRotationY(nx, nz),
            });
          }
        }
      }
    }
    if (!slots.length) return;

    /* 真实建筑只放在近景且受性能档预算限制。每栋 Downtown 建筑有
     * 12~13 个材质 primitive，不能像盒体一样为每个 slot 克隆；剩余
     * 位置统一进入下方 InstancedMesh，保持中远景的低 draw call。 */
    const useCity = CITY_BUILDING_NAMES.some((n) => getCityModel(n) !== null);
    const realModelBudget = useCity ? cityBudget : 0;
    let cityPlaced = 0;
    const boxSlots = [];
    for (const slot of slots) {
      const useRealModel = slot.layer === 0 && cityPlaced < realModelBudget;
      if (useRealModel) {
        const name = CITY_BUILDING_NAMES[Math.floor(slot.seed * CITY_BUILDING_NAMES.length) % CITY_BUILDING_NAMES.length];
        if (placeCityModel(name, slot, tier)) {
          cityPlaced++;
          continue;
        }
      }
      boxSlots.push(slot);
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
    // 远景排：剪影材质（低饱和被雾冲淡 + 无窗格贴图，降低 draw call 与视觉噪点）
    const farFacade = new THREE.MeshStandardMaterial({
      color: 0x8a95a5,
      roughness: 0.95,
      metalness: 0.0,
    });
    const farRoof = new THREE.MeshStandardMaterial({
      color: 0x6b7686,
      roughness: 1.0,
    });

    const nearMid = boxSlots.filter((s) => s.layer < 2);
    const far = boxSlots.filter((s) => s.layer >= 2);

    const bodyGeo = new THREE.BoxGeometry(1, 1, 1);
    const roofGeo = new THREE.BoxGeometry(1, 1, 1);
    const dummy = new THREE.Object3D();
    const buildBatch = function(slotList, facadeMat, roofMat) {
      if (!slotList.length) return;
      const bodies = new THREE.InstancedMesh(bodyGeo, facadeMat, slotList.length);
      const roofs = new THREE.InstancedMesh(roofGeo, roofMat, slotList.length);
      slotList.forEach((slot, i) => {
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
      bodies.castShadow = slotList === nearMid ? tier === 'high' : false;
      bodies.receiveShadow = true;
      group.add(bodies, roofs);
    };
    buildBatch(nearMid, facade, roof);
    buildBatch(far, farFacade, farRoof);
  }

  return { build, clear, getGroup: () => group };
}
