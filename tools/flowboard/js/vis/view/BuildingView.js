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
import { mergeGeometries } from '../math/GeometryMerge.js';

const SPACING = { high: 55, medium: 80, low: 120 };
// 楼体完整落在路肩外的草地上，并给行道树/人行空间留出视觉缓冲。
const SETBACK = 32;
// 楼体到道路中心线的最短安全距离 = 路肩 + 人行道 + 绿化带 + 缓冲。
// 任何建筑 slot 若落进该范围内一律丢弃，防止楼直接盖在路面上。
const ROAD_OCCUPANCY_M = 24;
// 道路占用空间哈希的格子尺寸：比路宽大，保证只查附近格子。
const OCCUPANCY_CELL_M = 80;
// 建筑间去重的格子尺寸：建筑间距 ~55m、足迹 ~15m，用 15m 细格区分。
const PLACE_CELL_M = 15;

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

/** Place each building on the sampled road elevation with its facade facing
 * the road centerline. Exported as a pure geometry contract for regression
 * tests; callers add their own height and footprint dimensions. */
export function cityBuildingPose(px, py, pz, nx, nz, offset, side) {
  return {
    x: px + nx * offset * side,
    y: py,
    z: pz + nz * offset * side,
    rotation: directionToRotationY(-nx * side, -nz * side),
  };
}

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
      const isCityAsset = !!child.userData.cityAsset;
      child.traverse((node) => {
        if (!node.isMesh) return;
        // City clones share immutable geometry/textures with the cache. Their
        // per-instance materials are safe to release; disposing shared assets
        // would corrupt the next road-network rebuild.
        if (!isCityAsset && node.geometry) node.geometry.dispose();
        const materials = Array.isArray(node.material) ? node.material : [node.material];
        for (const material of materials) {
          if (!material) continue;
          if (!isCityAsset && material.map) material.map.dispose();
          material.dispose();
        }
      });
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
    model.position.set(slot.x, slot.y - scaledBox.min.y, slot.z);
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
    const tier = store.perfTier || 'low';
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

    /* ── OSM 真实建筑（单源真相）──
     * roadNetwork.buildings[] = {id, footprint:[[x,y]...], x, y, rotation, height, mesh?}
     * footprint/xy 为 ENU 米。THREE 坐标约定：x=ENU.x, z=-ENU.y（见 cityBuildingPose）。
     * 用 THREE.Shape + ExtrudeGeometry 按轮廓挤出到 height；footprint 已是绝对世界
     * 坐标，挤出后绕 X 轴 -90° 使挤出方向（原 +Z）变为 +Y（向上）。mesh 字段若由
     * osm2world 生成则优先用真实贴图模型，否则用程序化窗格材质盒体。 */
    const realBuildings = Array.isArray(roadNetwork.buildings) ? roadNetwork.buildings : [];
    if (realBuildings.length) {
      const facade = new THREE.MeshStandardMaterial({
        color: 0x8998a8, roughness: 0.78, metalness: 0.04, map: buildingTexture(),
      });
      const chunks = new Map();
      let totalGeos = 0;
      for (const b of realBuildings) {
        const fp = Array.isArray(b.footprint) ? b.footprint : null;
        if (!fp || fp.length < 3) continue;
        const h = Number(b.height) > 0 ? Number(b.height) : 12;
        const shape = new THREE.Shape();
        let cx = 0, cy = 0;
        for (let i = 0; i < fp.length; i++) {
          const wx = fp[i][0], wy = fp[i][1];
          cx += wx; cy += wy;
          if (i === 0) shape.moveTo(wx, wy); else shape.lineTo(wx, wy);
        }
        cx /= fp.length; cy /= fp.length;
        shape.closePath();
        const geo = new THREE.ExtrudeGeometry(shape, { depth: h, bevelEnabled: false });
        geo.rotateX(-Math.PI / 2);  // 挤出方向 +Z → +Y（向上），footprint 已是世界坐标
        const chunkKey = `${Math.floor(cx / 150)},${Math.floor(cy / 150)}`;
        if (!chunks.has(chunkKey)) chunks.set(chunkKey, []);
        chunks.get(chunkKey).push(geo);
        totalGeos++;
      }
      for (const [key, chunkGeos] of chunks.entries()) {
        if (chunkGeos.length > 0) {
          const merged = mergeGeometries(chunkGeos);
          if (merged) {
            merged.computeBoundingBox();
            merged.computeBoundingSphere();
            const mesh = new THREE.Mesh(merged, facade);
            mesh.castShadow = tier === 'high';
            mesh.receiveShadow = tier === 'high';
            mesh.frustumCulled = true;
            group.add(mesh);
          }
          for (const g of chunkGeos) g.dispose();
        }
      }
      if (totalGeos > 0) {
        console.log('[vis] rendered ' + totalGeos + ' OSM buildings in ' + chunks.size + ' spatial chunks (frustum culling enabled)');
        return;  // 真实建筑已合并渲染，跳过程序化天际线
      }
    }

    /* 无 OSM 建筑数据（旧地图/非 OSM 场景）→ 程序化道路两侧天际线（原实现）。 */
    const spacing = SPACING[tier] || SPACING.high;
    const slots = [];

    // ── 道路占用空间哈希（线段粒度）：把每条路的中心线折线切成小段，
    //  每段按所在格子索引。建筑候选点到最近路段的垂距 < ROAD_OCCUPANCY_M
    //  即视为"落在路上"，丢弃。这样对任意网格间距都通用，且不会误伤
    //  正常路侧建筑（只挡真正盖在路面/路侧设施上的 slot）。
    const occGrid = new Map();   // "col,row" -> [{x0,y0,x1,y1}, ...]
    function occCell(x, y) {
      const col = Math.floor(x / OCCUPANCY_CELL_M);
      const row = Math.floor(y / OCCUPANCY_CELL_M);
      return col + ',' + row;
    }
    function occAddSegment(x0, y0, x1, y1) {
      const seg = { x0, y0, x1, y1 };
      const key0 = occCell(x0, y0);
      const key1 = occCell(x1, y1);
      if (!occGrid.has(key0)) occGrid.set(key0, []);
      occGrid.get(key0).push(seg);
      if (key1 !== key0) {
        if (!occGrid.has(key1)) occGrid.set(key1, []);
        occGrid.get(key1).push(seg);
      }
    }
    for (const edge of roadNetwork.edges) {
      if (edge.type === EDGE_TYPE.VIADUCT_HIGHWAY ||
          edge.name === EDGE_TYPE.VIADUCT_HIGHWAY) continue;
      const raw = Array.isArray(edge.nodes) ? edge.nodes : [];
      if (raw.length < 2) continue;
      // ENU 坐标下切分中心线折线
      const pts = raw.map(n => Array.isArray(n)
        ? [n[0] || 0, n[1] || 0]
        : [n.x || 0, n.y || 0]);
      for (let i = 1; i < pts.length; i++) {
        const a = pts[i - 1], b = pts[i];
        // 长段按 OCCUPANCY_CELL_M 分段，保证落在所有经过的格子
        const len = Math.hypot(b[0] - a[0], b[1] - a[1]);
        const steps = Math.max(1, Math.ceil(len / OCCUPANCY_CELL_M));
        for (let s = 0; s < steps; s++) {
          const t0 = s / steps, t1 = (s + 1) / steps;
          occAddSegment(
            a[0] + (b[0] - a[0]) * t0, a[1] + (b[1] - a[1]) * t0,
            a[0] + (b[0] - a[0]) * t1, a[1] + (b[1] - a[1]) * t1,
          );
        }
      }
    }
    /** 点到最近道路中心线折线的垂距（ENU 坐标）。 */
    function roadDistance(px, py) {
      const key = occCell(px, py);
      const [c0, r0] = key.split(',').map(Number);
      let best = Infinity;
      for (let dc = -1; dc <= 1; dc++) {
        for (let dr = -1; dr <= 1; dr++) {
          const segs = occGrid.get((c0 + dc) + ',' + (r0 + dr));
          if (!segs) continue;
          for (const s of segs) {
            const dx = s.x1 - s.x0, dy = s.y1 - s.y0;
            const len2 = dx * dx + dy * dy || 1e-9;
            const t = Math.max(0, Math.min(1, ((px - s.x0) * dx + (py - s.y0) * dy) / len2));
            const cx = s.x0 + dx * t, cy = s.y0 + dy * t;
            const d = Math.hypot(px - cx, py - cy);
            if (d < best) best = d;
          }
        }
      }
      return best;
    }

    // ── 建筑间去重：同一格内已有建筑则跳过，防止相邻道路的纵深排
    //  在街区中间互相重叠（如 A 路 mid 层 x=101 与 B 路 mid 层 x=99）。──
    const placedGrid = new Map();   // "col,row" -> count
    function tryPlace(enuX, enuY, halfDiag) {
      const c0 = Math.floor((enuX - halfDiag) / PLACE_CELL_M);
      const c1 = Math.ceil((enuX + halfDiag) / PLACE_CELL_M);
      const r0 = Math.floor((enuY - halfDiag) / PLACE_CELL_M);
      const r1 = Math.ceil((enuY + halfDiag) / PLACE_CELL_M);
      for (let c = c0; c <= c1; c++) {
        for (let r = r0; r <= r1; r++) {
          if (placedGrid.has(c + ',' + r)) return false;
        }
      }
      for (let c = c0; c <= c1; c++) {
        for (let r = r0; r <= r1; r++) {
          placedGrid.set(c + ',' + r, true);
        }
      }
      return true;
    }

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
          const px = points[i], py = points[i + 1], pz = points[i + 2];
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
            const pose = cityBuildingPose(px, py, pz, nx, nz, offset, side);
            // 建筑落点（THREE 坐标）→ ENU 坐标，查最近道路中心线垂距，
            // 落进路面/路侧设施范围内则丢弃，防止楼直接盖在路上。
            const enuX = pose.x;                 // THREE.x = ENU.x
            const enuY = -pose.z;                // THREE.z = -ENU.y(north)
            if (roadDistance(enuX, enuY) < ROAD_OCCUPANCY_M) continue;
            // 与已放建筑去重（按足迹半对角占一格），避免街区中间叠楼
            if (!tryPlace(enuX, enuY, Math.hypot(width, depth) * 0.5)) continue;
            slots.push({
              ...pose,
              height, width, depth, seed,
              layer: li,
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
        dummy.position.set(slot.x, slot.y + slot.height / 2, slot.z);
        dummy.rotation.set(0, slot.rotation, 0);
        dummy.scale.set(slot.depth, slot.height, slot.width);
        dummy.updateMatrix();
        bodies.setMatrixAt(i, dummy.matrix);

        dummy.position.set(slot.x, slot.y + slot.height + 0.5, slot.z);
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
