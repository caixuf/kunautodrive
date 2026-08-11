/**
 * VehicleView.js — 车辆 3D 渲染 + 动画
 *
 * P2 画质升级：
 *   - glTF 车辆：遍历 mesh 把车身材质升级为 MeshPhysicalMaterial（envMap 反射 + clearcoat）
 *   - 程序化 fallback：MeshPhysicalMaterial 清漆替代 MeshStandardMaterial
 *   - PMREMGenerator 生成环境反射贴图
 *
 * 两套渲染路径：
 *   1. glTF 模型（models/*.gltf）—— 优先，加载失败回退到 2
 *   2. 程序化 fallback（utils._buildSedan 等）—— 兼容 /api/topology 无 model 字段
 *
 * Layer 树驱动：rootLayer.update(store, now) → 创建/更新车辆 + 每帧动画
 * （车轮转向/滚动、方向盘、刹车灯/转向灯/大灯控制）
 */

import { deriveLightState, LIGHT_TURN_LEFT, LIGHT_TURN_RIGHT, LIGHT_HAZARD, LIGHT_HIGH_BEAM, LIGHT_LOW_BEAM } from './VehicleLights.js';
import { getStdMaterial } from '../core/AssetFactory.js';
import { initModelCache, getModel, _setVehicleLights, _relinkWheelUserData } from '../../models.js';
import { worldToThree, headingToRotationY } from '../math/Coord.js';

// ═══════════════════════════════════════════════════════════
// createVehicleLights — THREE 灯光网格工厂（原在 VehicleLights.js，
// 移到此处以保持 VehicleLights.js 零 THREE 依赖，供 Node 单元测试）
// ═══════════════════════════════════════════════════════════

const LIGHT_OFF = new THREE.Color(0x111111);
const LIGHT_BRAKE_ON = new THREE.Color(0xff0000);
const LIGHT_TURN_ON = new THREE.Color(0xff8800);
const LIGHT_HEAD_ON = new THREE.Color(0xffffcc);

const BRAKE_Y = 0.55;      // 尾灯高度
const BRAKE_X = -2.05;     // 车尾（X-forward）
const BRAKE_Z = 0.65;      // 左右间距
const TURN_X = -2.08;
const TURN_FRONT_Y = 0.68;
const TURN_REAR_Y = 0.72;
const TURN_Z = 0.78;
const HEAD_Y = 0.45;       // 前灯高度
const HEAD_X = 2.12;       // 车头
const HEAD_Z = 0.62;

const GEO_RECT = new THREE.PlaneGeometry(0.18, 0.10);
const GEO_BRAKE = new THREE.PlaneGeometry(0.10, 0.06);  // 刹车灯：窄条不占视觉
const GEO_TURN  = new THREE.PlaneGeometry(0.22, 0.12);  // 转向灯：更大更醒目
const GEO_HEAD  = new THREE.PlaneGeometry(0.26, 0.14);  // 近光灯：宽大明显

function _makeRectMesh(geo, color, x, y, z) {
  const mat = new THREE.MeshBasicMaterial({ color, side: THREE.DoubleSide, transparent: true, opacity: 0.9 });
  const m = new THREE.Mesh(geo, mat);
  // Vehicles are X-forward in the shared ENU/THREE contract; place the
  // rectangular light on the front/rear YZ face rather than the old Z face.
  m.rotation.y = Math.PI / 2;
  m.position.set(x, y, z);
  return m;
}

function _findTurnAnchor(vehicleGroup, names, fallback) {
  let node = null;
  if (vehicleGroup && vehicleGroup.traverse) {
    vehicleGroup.traverse((child) => {
      if (!node && names.includes(child.name)) node = child;
    });
  }
  if (!node || !node.position) return fallback;
  const outward = node.position.x >= 0 ? 0.12 : -0.12;
  return { x: node.position.x + outward, y: node.position.y };
}

/** 为车辆模型创建灯光网格组。
 *  @param {THREE.Group} vehicleGroup 车辆模型根节点
 *  @param {{brake?: boolean, turn?: boolean, head?: boolean}} options
 *  @returns {{group: THREE.Group, update: (v: object) => void}} */
function createVehicleLights(vehicleGroup, options = {}) {
  const group = new THREE.Group();
  const showBrakeOverlay = options.brake !== false;
  const showTurnOverlay = options.turn !== false;
  const showHeadOverlay = options.head !== false;
  const frontTurn = _findTurnAnchor(vehicleGroup,
    ['Light', 'Light.002', 'Light002', 'LightGlass.004', 'LightGlass004'],
    { x: HEAD_X + 0.12, y: TURN_FRONT_Y });
  const rearTurn = _findTurnAnchor(vehicleGroup,
    ['Light.003', 'Light003', 'LightGlass'],
    { x: TURN_X - 0.12, y: TURN_REAR_Y });

  // In the shared X-forward frame, left is -Z and right is +Z. Keep each
  // pair on the same front/rear face; the previous wiring swapped X/Z and
  // put one brake/turn/head lamp at the wrong end of the car.
  const brakeL = _makeRectMesh(GEO_BRAKE, LIGHT_OFF, BRAKE_X, BRAKE_Y, -BRAKE_Z);
  const brakeR = _makeRectMesh(GEO_BRAKE, LIGHT_OFF, BRAKE_X, BRAKE_Y,  BRAKE_Z);
  const turnFL = _makeRectMesh(GEO_TURN, LIGHT_OFF, frontTurn.x, frontTurn.y, -TURN_Z);
  const turnFR = _makeRectMesh(GEO_TURN, LIGHT_OFF, frontTurn.x, frontTurn.y,  TURN_Z);
  const turnRL = _makeRectMesh(GEO_TURN, LIGHT_OFF, rearTurn.x, rearTurn.y, -TURN_Z);
  const turnRR = _makeRectMesh(GEO_TURN, LIGHT_OFF, rearTurn.x, rearTurn.y,  TURN_Z);
  const headL  = _makeRectMesh(GEO_HEAD, LIGHT_OFF, HEAD_X,  HEAD_Y, -HEAD_Z);
  const headR  = _makeRectMesh(GEO_HEAD, LIGHT_OFF, HEAD_X,  HEAD_Y,  HEAD_Z);

  // 熄灯时隐藏，避免暗色灯片显示为车身边缘的黑方块
  brakeL.visible = brakeR.visible = false;
  turnFL.visible = turnFR.visible = turnRL.visible = turnRR.visible = false;
  headL.visible = headR.visible = false;

  group.add(brakeL, brakeR, turnFL, turnFR, turnRL, turnRR, headL, headR);

  return {
    group,
    update(v, nowMs) {
      const s = deriveLightState(v.lights || 0, v.brake || 0);
      const blinkOn = nowMs === undefined
        ? true
        : (((nowMs / 1000) * 1.5) % 1) < 0.5;
      brakeL.visible = showBrakeOverlay && s.brake;
      brakeR.visible = showBrakeOverlay && s.brake;
      brakeL.material.color.copy(LIGHT_BRAKE_ON);
      brakeR.material.color.copy(LIGHT_BRAKE_ON);
      const turnL = showTurnOverlay && s.turnL && blinkOn;
      const turnR = showTurnOverlay && s.turnR && blinkOn;
      turnFL.visible = turnRL.visible = turnL;
      turnFR.visible = turnRR.visible = turnR;
      turnFL.material.color.copy(LIGHT_TURN_ON);
      turnFR.material.color.copy(LIGHT_TURN_ON);
      turnRL.material.color.copy(LIGHT_TURN_ON);
      turnRR.material.color.copy(LIGHT_TURN_ON);
      headL.visible = showHeadOverlay && s.head;
      headR.visible = showHeadOverlay && s.head;
      headL.material.color.copy(LIGHT_HEAD_ON);
      headR.material.color.copy(LIGHT_HEAD_ON);
    }
  };
}

// ═══════════════════════════════════════════════════════════
// 车漆参数（MeshPhysicalMaterial）
// ═══════════════════════════════════════════════════════════

const PAINT_CLEARCOAT = 0.9;
const PAINT_CLEARCOAT_ROUGHNESS = 0.15;
const PAINT_ROUGHNESS = 0.35;
const PAINT_METALNESS = 0.15;
const ENVMAP_INTENSITY = 0.9;

const BODY_KEYWORDS = ['body', 'car', 'paint', 'chassis', 'body_', 'Body', 'Chassis', 'CAR', 'Paint', 'Car', 'mesh_'];

// ═══════════════════════════════════════════════════════════
// 全局 envMap 缓存（PMREMGenerator）
// ═══════════════════════════════════════════════════════════

let _pmrem = null;
let _envMap = null;

/** 确保 envMap 已生成，返回共享的 envMap texture */
function _ensureEnvMap(renderer, scene) {
  if (_envMap) return _envMap;
  if (!renderer || !scene) return null;
  // main.js already bakes the scene environment during initialization. Reuse
  // it instead of creating a second PMREM target on the first vehicle tick.
  if (scene.environment) {
    _envMap = scene.environment;
    return _envMap;
  }
  try {
    _pmrem = new THREE.PMREMGenerator(renderer);
    // 从场景渲染环境贴图（天空 + 光照）
    _envMap = _pmrem.fromScene(scene, 0.04).texture;
    _envMap.colorSpace = THREE.SRGBColorSpace;
  } catch (e) {
    console.warn('[VehicleView] PMREMGenerator failed, envMap unavailable:', e.message);
  }
  return _envMap;
}

/** 判断 mesh 是否是车身（需要车漆升级） */
function _isBodyMesh(mesh) {
  const name = mesh.name || '';
  return BODY_KEYWORDS.some(kw => name.includes(kw));
}

/** 把 mesh 的材质升级为车漆 MeshPhysicalMaterial */
function _upgradeToCarPaint(mesh, envMap) {
  if (!mesh.material) return;

  const upgrade = (oldMat) => {
    if (!oldMat || (oldMat.isMeshPhysicalMaterial && oldMat.clearcoat > 0.5)) return oldMat;
    // Black trim/interior is intentionally not car paint.  Keeping its
    // original StandardMaterial avoids turning a rough dark part into a
    // mirror-black PhysicalMaterial.
    const materialName = (oldMat.name || '').toLowerCase();
    if (materialName.includes('smoothblack') ||
        materialName.includes('frostedblack') ||
        materialName.includes('iron') ||
        materialName.includes('wheel') ||
        materialName.includes('logo')) {
      return oldMat;
    }

    // Preserve the common StandardMaterial fields explicitly. Three r160's
    // MeshPhysicalMaterial.copy() also copies physical-only fields and throws
    // when its source is a MeshStandardMaterial.
    const mat = new THREE.MeshPhysicalMaterial();
    mat.name = oldMat.name || '';
    if (oldMat.color) mat.color.copy(oldMat.color);
    if (oldMat.emissive && mat.emissive) mat.emissive.copy(oldMat.emissive);
    mat.map = oldMat.map || null;
    mat.lightMap = oldMat.lightMap || null;
    mat.lightMapIntensity = oldMat.lightMapIntensity !== undefined ? oldMat.lightMapIntensity : 1;
    mat.aoMap = oldMat.aoMap || null;
    mat.aoMapIntensity = oldMat.aoMapIntensity !== undefined ? oldMat.aoMapIntensity : 1;
    mat.emissiveMap = oldMat.emissiveMap || null;
    mat.emissiveIntensity = oldMat.emissiveIntensity !== undefined ? oldMat.emissiveIntensity : 1;
    mat.bumpMap = oldMat.bumpMap || null;
    mat.bumpScale = oldMat.bumpScale !== undefined ? oldMat.bumpScale : 1;
    mat.normalMap = oldMat.normalMap || null;
    mat.normalMapType = oldMat.normalMapType;
    if (oldMat.normalScale && mat.normalScale) mat.normalScale.copy(oldMat.normalScale);
    mat.displacementMap = oldMat.displacementMap || null;
    mat.displacementScale = oldMat.displacementScale !== undefined ? oldMat.displacementScale : 1;
    mat.displacementBias = oldMat.displacementBias !== undefined ? oldMat.displacementBias : 0;
    mat.roughnessMap = oldMat.roughnessMap || null;
    mat.metalnessMap = oldMat.metalnessMap || null;
    mat.alphaMap = oldMat.alphaMap || null;
    mat.transparent = !!oldMat.transparent;
    mat.opacity = oldMat.opacity !== undefined ? oldMat.opacity : 1;
    mat.alphaTest = oldMat.alphaTest !== undefined ? oldMat.alphaTest : 0;
    mat.side = oldMat.side !== undefined ? oldMat.side : THREE.FrontSide;
    mat.vertexColors = !!oldMat.vertexColors;
    mat.flatShading = !!oldMat.flatShading;
    mat.depthWrite = oldMat.depthWrite !== undefined ? oldMat.depthWrite : true;
    mat.depthTest = oldMat.depthTest !== undefined ? oldMat.depthTest : true;
    mat.toneMapped = oldMat.toneMapped !== undefined ? oldMat.toneMapped : true;
    mat.metalness = oldMat.metalness !== undefined
      ? Math.min(oldMat.metalness, PAINT_METALNESS)
      : PAINT_METALNESS;
    const oldRoughness = oldMat.roughness !== undefined
      ? oldMat.roughness
      : PAINT_ROUGHNESS;
    // Avoid a near-zero roughness mirror that becomes black when the
    // reflection probe has little energy, while retaining the glossy paint.
    mat.roughness = Math.max(0.15, Math.min(oldRoughness, PAINT_ROUGHNESS));
    mat.clearcoat = PAINT_CLEARCOAT;
    mat.clearcoatRoughness = PAINT_CLEARCOAT_ROUGHNESS;
    mat.envMap = envMap || oldMat.envMap || null;
    mat.envMapIntensity = ENVMAP_INTENSITY;
    oldMat.dispose();
    return mat;
  };

  if (Array.isArray(mesh.material)) {
    mesh.material = mesh.material.map(upgrade);
  } else {
    mesh.material = upgrade(mesh.material);
  }
}

/** 遍历 glTF scene，给车身 mesh 上清漆 */
function _applyCarPaintToScene(gltfScene, envMap) {
  gltfScene.traverse((child) => {
    if (!child.isMesh) return;
    if (_isBodyMesh(child)) {
      _upgradeToCarPaint(child, envMap);
    }
  });
}

/** Keep the authorized SU7 as a clean production vehicle.
 * The source asset contains a text/logo AO decal and separate plate/badge
 * meshes. They are presentation stickers rather than vehicle paint. */
export function _cleanSu7Exterior(scene) {
  scene.traverse((child) => {
    const name = child.name || '';
    if (name === 'ChePai' || name.toLowerCase().indexOf('logo') === 0) {
      child.visible = false;
      return;
    }
    if (!child.isMesh || !child.material) return;
    const materials = Array.isArray(child.material) ? child.material : [child.material];
    materials.forEach((material) => {
      if ((material.name || '').toLowerCase() === 'car_body') {
        // sm_car_img0 is a text/logo atlas used as ambient occlusion on the
        // paint shell. Removing only this map preserves the original color
        // while eliminating the baked-on lettering.
        material.aoMap = null;
        material.aoMapIntensity = 1;
      }
    });
  });
}

// ═══════════════════════════════════════════════════════════
// 方向盘程序化注入
// ═══════════════════════════════════════════════════════════

const STEERING_DEADZONE = 0.005;
const STEERING_WHEEL_RATIO = 7;

/** 将后端前轮转角映射到前轴和方向盘的显示轴。 */
export function _steeringVisualState(steer) {
  const numericSteer = Number.isFinite(steer) ? steer : 0;
  const filteredSteer = Math.abs(numericSteer) < STEERING_DEADZONE ? 0 : numericSteer;
  return {
    frontAxleYaw: filteredSteer,
    steeringWheelRoll: filteredSteer * STEERING_WHEEL_RATIO,
    steeringWheelAxis: 'x',
  };
}

/** 方向盘网格不能进入车轮滚动分支，否则会随车速持续“自转”。 */
export function _isSteeringWheelNode(name) {
  const normalized = String(name || '').toLowerCase().replace(/[-\s]/g, '_');
  return normalized === 'steering_wheel' || normalized === 'steeringwheel';
}

/* glTF 模型（sedan/suv/truck/su7）均无 "steering"/"steer"/"handle" 命名节点，
 * 因此程序化注入一个带 steering_column / steering_axis / steering_wheel_pivot
 * 的真实转向组件。仿真约定 steer>0 会让 ENU heading/y 增大（左转），
 * steer<0 是右转；前轴和方向盘都沿这个约定显示。 */
function _createSteeringWheel() {
  const pos = new THREE.Group();
  pos.name = 'steering_system';
  // SU7 is left-hand drive: the shared vehicle frame maps the driver's side
  // to -Z (THREE left), not the passenger side at +Z.
  pos.position.set(0.5, 1.0, -0.4);

  const axis = new THREE.Group();
  axis.name = 'steering_axis';
  axis.rotation.z = -0.10;  // column points from dashboard toward the driver

  const columnGeo = new THREE.CylinderGeometry(0.022, 0.028, 0.34, 8);
  const columnMat = new THREE.MeshStandardMaterial({
    color: 0x222228, roughness: 0.5, metalness: 0.65, // exempt: steering column
  });
  const column = new THREE.Mesh(columnGeo, columnMat);
  column.name = 'steering_column_shaft';
  column.rotation.z = Math.PI / 2;  // cylinder axis Y → local steering axis X
  column.position.x = -0.15;
  axis.add(column);

  const wheelPivot = new THREE.Group();
  wheelPivot.name = 'steering_wheel_pivot';
  wheelPivot.position.x = -0.30;

  // 方向盘圆环：旋转到 YZ 平面，法线沿车辆 X 轴。
  const torusGeo = new THREE.TorusGeometry(0.16, 0.018, 8, 24);
  const torusMat = new THREE.MeshStandardMaterial({
    color: 0x111111, roughness: 0.45, metalness: 0.7,  // exempt: steering wheel rim (non-body metal)
  });
  const torus = new THREE.Mesh(torusGeo, torusMat);
  torus.name = 'steering_wheel';  // 让 _updateGltfVehicle 的 traverse 选中
  torus.rotation.y = Math.PI / 2;
  torus.castShadow = false;       // 方向盘不投影，避免车内饰阴影噪音

  // 中心 hub（child of torus，跟随旋转）
  const hubGeo = new THREE.CylinderGeometry(0.04, 0.04, 0.05, 16);
  const hubMat = new THREE.MeshStandardMaterial({
    color: 0x222222, roughness: 0.4, metalness: 0.8,  // exempt: steering wheel hub (non-body metal)
  });
  const hub = new THREE.Mesh(hubGeo, hubMat);
  hub.rotation.z = Math.PI / 2;  // 圆柱轴 Y → X (沿 column)
  torus.add(hub);

  // 3 条辐条（child of torus，跟随旋转；各 120° 间隔）
  const spokeGeo = new THREE.BoxGeometry(0.022, 0.30, 0.010);
  const spokeMat = new THREE.MeshStandardMaterial({
    color: 0x333333, roughness: 0.5, metalness: 0.6,  // exempt: steering wheel spokes (non-body metal)
  });
  for (let i = 0; i < 3; i++) {
    const spoke = new THREE.Mesh(spokeGeo, spokeMat);
    spoke.rotation.x = (i * 120) * Math.PI / 180;
    torus.add(spoke);
  }

  wheelPivot.add(torus);
  axis.add(wheelPivot);
  pos.add(axis);
  return pos;
}

// ═══════════════════════════════════════════════════════════
// 主工厂
// ═══════════════════════════════════════════════════════════

/** 异步预加载 glTF 车辆模型（SU7/sedan/suv/truck）。
 *  加载完成前用程序化 fallback，完成后新车自动用 glTF。
 *  main.js 在 init3DScene 中调用，无需 await。 */
export function initModels() {
  initModelCache();
}

export function createVehicleView(scene, renderer, modelCache) {
  const vehicleMap = new Map();  // id → { group, lights, modelData, ... }
  let vehicleGroup = new THREE.Group();
  let lastUpdateMs = null;
  scene.add(vehicleGroup);

  // ── 辅助工具 ──

  /** 获取车辆的可视 group（兼容 glTF 包裹层） */
  function _getVisGroup(entry) {
    if (!entry) return null;
    if (entry.visGroup) return entry.visGroup;
    // glTF 路径：scene 对象本身可能就是个 group
    if (entry.modelData && entry.modelData.scene) {
      return entry.modelData.scene;
    }
    return entry.group;
  }

  /** 创建程序化 fallback（MeshPhysicalMaterial 车漆 + X-forward 朝向） */
  function _createFallbackVehicle(type, id) {
    const group = new THREE.Group();

    // 车型 → 车身颜色映射（与 gen_models.py 材质一致）
    const COLOR_MAP = {
      su7: 0x1a5288,    // 海湾蓝
      sedan: 0x2a6fc4,   // 深蓝
      truck: 0x4a4a4a,   // 深灰
      suv: 0x2d6b3a,     // 深绿
      car: 0x2a6fc4,
    };
    const bodyColor = COLOR_MAP[type] || 0xcccccc;

    const bodyMat = new THREE.MeshPhysicalMaterial({
      color: bodyColor,
      metalness: PAINT_METALNESS,
      roughness: PAINT_ROUGHNESS,
      clearcoat: PAINT_CLEARCOAT,
      clearcoatRoughness: PAINT_CLEARCOAT_ROUGHNESS,
      envMap: _envMap || null,
      envMapIntensity: ENVMAP_INTENSITY,
      depthWrite: true,
    });

    // 车身：长轴沿 X（forward），宽沿 Z（与 glTF 模型一致）
    const bodyGeo = new THREE.BoxGeometry(4.5, 0.6, 1.8);
    const body = new THREE.Mesh(bodyGeo, bodyMat);
    body.castShadow = true;
    body.receiveShadow = true;
    body.position.y = 0.65;
    group.add(body);

    // 挡风玻璃（半透明）
    const glassGeo = new THREE.BoxGeometry(0.1, 0.35, 1.6);
    const glassMat = new THREE.MeshPhysicalMaterial({
      color: 0x88ccff,
      roughness: 0.1,
      metalness: 0.1,
      opacity: 0.4,
      transparent: true,
      depthWrite: false,
    });
    const glass = new THREE.Mesh(glassGeo, glassMat);
    glass.position.set(0.8, 0.95, 0);
    group.add(glass);

    // 后窗
    const rearGlass = new THREE.Mesh(glassGeo.clone(), glassMat);
    rearGlass.position.set(-0.8, 0.95, 0);
    group.add(rearGlass);

    // 轮毂（4个圆柱，轴沿 Z 与 glTF 一致）
    const wheelGeo = new THREE.CylinderGeometry(0.3, 0.3, 0.2, 16);
    const wheelMat = new THREE.MeshStandardMaterial({ color: 0x222222, roughness: 0.6, metalness: 0.7 });
    const wheelPositions = [
      [1.3, 0.3, -0.8], [1.3, 0.3, 0.8],
      [-1.3, 0.3, -0.8], [-1.3, 0.3, 0.8],
    ];
    wheelPositions.forEach(([x, y, z]) => {
      const w = new THREE.Mesh(wheelGeo, wheelMat);
      w.position.set(x, y, z);
      w.rotation.x = Math.PI / 2; // 圆柱轴从 Y → Z（横向，与 glTF cylinder axis=Z 一致）
      w.castShadow = true;
      group.add(w);
    });

    // 注入方向盘（fallback 车辆同样需要可见的转向反馈，行人跳过）
    if (type !== 'pedestrian') {
      group.add(_createSteeringWheel());
    }

    group.name = 'fallback_' + type + '_' + id;
    return group;
  }

  /** 从 glTF 创建车辆/行人。
   *  getModel() 返回的已经是 THREE.Group（非 loader 原始 {scene}），
   *  直接使用，不要再次 clone 或走 .scene，否则会浪费一份完整模型并
   *  让大模型加载时产生额外卡顿。
   *  行人只显示躯干+头+腿，不注入方向盘/车漆/车轮动画。 */
  function _createGltfVehicle(gltf, id, type) {
    const scene = gltf;
    scene.name = 'gltf_' + id;
    // clone() 不深拷贝 userData 引用，重建 wheel/light 引用
    _relinkWheelUserData(scene);
    if (type === 'pedestrian') {
      // 行人：无车漆、无方向盘、无灯光节点
      return scene;
    }
    // 应用车漆升级
    if (type === 'su7') _cleanSu7Exterior(scene);
    _applyCarPaintToScene(scene, _envMap);
    // 注入方向盘（glTF 车辆模型未自带 steering_wheel mesh）
    scene.add(_createSteeringWheel());
    return scene;
  }

  /** 每帧更新 glTF 车辆/行人：前轴转向 + 车轮旋转 + 灯光 */
  function _updateGltfVehicle(entry, v, dt, type) {
    const vis = _getVisGroup(entry);
    if (!vis) return;
    // 行人：无方向盘/车轮/灯光动画
    if (type === 'pedestrian') return;
    // SceneDirector normalizes the dashboard contract to `speed` (m/s).
    // Keep speed_mps as a compatibility alias for standalone callers.
    const speed_mps = v
      ? (Number.isFinite(v.speed_mps) ? v.speed_mps
        : (Number.isFinite(v.speed) ? v.speed : 0))
      : 0;

    // 前轴转向：glTF 模型 axle_front（含 wheel_FL/wheel_FR）整体绕 Y 旋转。
    // 仿真 steer>0 会让 ENU y/heading 增大，即左转；THREE +Y 也把 +X
    // 车头转向 -Z（左侧），所以不再额外取反。steer<0 正确显示右转。
    // 1:1 映射（2026-08 修复）：后端 step_bicycle 的 steer 就是前轮转角 δ，
    // 直接显示 —— 变道 0.05-0.1 rad ≈ 3-6°（可见），掉头满舵 0.45-0.60 rad
    // ≈ 26-34°（真实前轮最大转角）。旧乘子 0.6 把前轮角又缩小 40%，
    // 变道时前轮只转 2-3.5° 肉眼不可见 → "前轮不会动"。
    // 死区滤波：|steer| < 0.005 时视为 0，避免直路巡航时方向盘和车轮微动。
    const steering = _steeringVisualState(entry && entry.steerAngle);
    if (vis.userData && vis.userData.frontAxle) {
      vis.userData.frontAxle.rotation.y = steering.frontAxleYaw;
    }

    let steeringWheelPivot = null;
    vis.traverse((child) => {
      if (!steeringWheelPivot && child.name === 'steering_wheel_pivot') {
        steeringWheelPivot = child;
      }
    });
    if (steeringWheelPivot) {
      // A real local pivot is used instead of rotating the torus mesh itself;
      // the shaft, hub and spokes therefore share one physical steering axis.
      steeringWheelPivot.rotation.x = steering.steeringWheelRoll;
    }

    vis.traverse((child) => {
      const name = (child.name || '').toLowerCase();

      // 兼容外部 glTF 的旧 steering_wheel 节点；本项目注入的 SU7 组件
      // 已经由 steering_wheel_pivot 处理，避免 torus 被二次旋转。
      if (!steeringWheelPivot &&
          (name.includes('steering') || name.includes('steer') || name.includes('handle')) &&
          steering.frontAxleYaw !== undefined) {
        child.rotation.z = steering.steeringWheelRoll;
        // 命中方向盘后不再处理车轮逻辑（避免方向盘被误判为 wheel）
        return;
      }

      if (!child.isMesh) return;

      // 车轮旋转：程序化模型沿 X，glTF 车轮沿 Z。
      if (!_isSteeringWheelNode(child.name) &&
          (name.includes('wheel') || name.includes('tire') || name.includes('tyre'))) {
        if (speed_mps !== undefined) {
          const radius = 0.35;
          const angularSpeed = speed_mps / radius;
          if (child.userData && child.userData.rollAxis === 'z') {
            child.rotation.z += angularSpeed * dt;
          } else {
            child.rotation.x += angularSpeed * dt;
          }
        }
      }
    });
  }

  // ── 公共 API ──

  /** 添加或更新一辆车 */
  function updateVehicle(id, vehicleData, type) {
    // 确保 envMap 已生成
    _ensureEnvMap(renderer, scene);

    let entry = vehicleMap.get(id);
    if (!entry) {
      entry = { group: null, lights: null, modelData: null, steerAngle: 0 };
      vehicleMap.set(id, entry);
    }

    // Only clone a cached glTF when this vehicle has not adopted it yet.
    // Calling getModel() unconditionally here cloned the full SU7 hierarchy
    // (169k triangles/materials) on every animation frame.
    const gltf = entry.modelData ? null : getModel(type);
    if (gltf && !entry.modelData) {
      // 清除旧 fallback group（避免双重模型叠加）
      if (entry.group) {
        vehicleGroup.remove(entry.group);
        entry.group = null;
      }
      entry.modelData = gltf;
      entry.group = _createGltfVehicle(gltf, id, type);
      vehicleGroup.add(entry.group);

      // SU7 原始 Light/LightGlass 节点复用真实材质；仅在没有真实灯节点
      // 的 fallback 模型上创建灯光网格，避免悬浮方块覆盖精细车身。
      const ud = entry.group.userData || {};
      const hasCompleteSemanticLights = ud.brakeLights && ud.turnSignals && ud.headlights;
      const hasRawSu7Lights = !!ud.su7RawLights;
      entry.lights = hasCompleteSemanticLights || hasRawSu7Lights ? null : createVehicleLights(entry.group, {
        brake: !ud.brakeLights,
        turn: !ud.turnSignals,
        head: !ud.headlights,
      });
      if (entry.lights) {
        entry.group.add(entry.lights.group);
      }
    }

    // 如果没有 group（glTF 加载失败或未就绪），创建 fallback
    if (!entry.group) {
      entry.group = _createFallbackVehicle(type, id);
      vehicleGroup.add(entry.group);
      entry.lights = createVehicleLights(entry.group);
      if (entry.lights && entry.lights.group) {
        entry.group.add(entry.lights.group);
      }
    }

    // 更新位姿（ENU → THREE 坐标映射）
    if (vehicleData) {
      const [tx, ty, tz] = worldToThree(
        vehicleData.x || vehicleData.px || 0,
        vehicleData.y || vehicleData.py || 0,
        vehicleData.z || vehicleData.pz || 0
      );
      entry.group.position.set(tx, ty, tz);
      entry.group.rotation.set(0, headingToRotationY(vehicleData.heading || vehicleData.yaw || 0), 0);
      entry.steerAngle = vehicleData.steer ?? vehicleData.steerAngle ?? 0;
    }

    return entry;
  }

  /* ── tick() 已删除 ──
   *
   * MVC 重构后 Layer 树只调 view.update()，不调 view.tick()。
   * 车轮转向/滚动 + 灯光控制等每帧动画已全部合并到 update() 中。
   * 若需单独驱动动画，直接调 update(store, now) 即可。 */


  /** 移除车辆 */
  function removeVehicle(id) {
    const entry = vehicleMap.get(id);
    if (!entry) return;
    if (entry.group) {
      vehicleGroup.remove(entry.group);
      entry.group.traverse((child) => {
        if (child.geometry) child.geometry.dispose();
        if (child.material) {
          if (Array.isArray(child.material)) {
            child.material.forEach(m => m.dispose());
          } else {
            child.material.dispose();
          }
        }
      });
    }
    vehicleMap.delete(id);
  }

  /** 清理 */
  function dispose() {
    vehicleMap.forEach((entry, id) => removeVehicle(id));
    vehicleMap.clear();
    scene.remove(vehicleGroup);
    if (_pmrem) {
      _pmrem.dispose();
      _pmrem = null;
    }
    if (_envMap) {
      _envMap.dispose();
      _envMap = null;
    }
  }

  function getVehicleGroup() { return vehicleGroup; }
  function getVehicleMap() { return vehicleMap; }

  /** Layer 树每帧调用：从 store 同步 ego + entities → 创建/更新/删除车辆 + 动画 */
  function update(store, now) {
    if (!store) return;
    const dt = lastUpdateMs === null || now === undefined
      ? 1 / 60
      : Math.min(0.1, Math.max(0, (now - lastUpdateMs) / 1000));
    lastUpdateMs = now === undefined ? lastUpdateMs : now;

    // 收集所有需要渲染的实体（ego + 其他车辆/NPC）
    const activeIds = new Set();

    // 1. ego 车辆（type 用于选模型：'ego' → su7）
    if (store.ego) {
      const egoId = 'ego';
      activeIds.add(egoId);
      updateVehicle(egoId, store.ego, 'su7');
    }

    // 2. 其他实体（car/truck/suv/pedestrian — 排除红绿灯/ETC/停止线等非车辆）
    const VEHICLE_TYPES = new Set(['car', 'suv', 'truck', 'pedestrian']);
    const entities = store.entities || [];
    for (const ent of entities) {
      if (!ent || !ent.id) continue;
      if (!VEHICLE_TYPES.has(ent.type)) continue;
      activeIds.add(ent.id);
      updateVehicle(ent.id, ent, ent.type || 'car');
    }

    // 3. 删除消失的车辆
    for (const id of Array.from(vehicleMap.keys())) {
      if (!activeIds.has(id)) {
        removeVehicle(id);
      }
    }

    // 4. 每帧动画：前轴转向 + 车轮旋转 + 灯光
    const animateVehicle = (id, type) => {
      const entry = vehicleMap.get(id);
      if (!entry) return;
      const v = (id === 'ego') ? store.ego
               : entities.find(e => e && e.id === id);
      if (!v) return;

      // glTF 动画（行人跳过方向盘/车轮/灯光）
      if (entry.modelData) {
        _updateGltfVehicle(entry, v, dt, type);
        // 行人灯光不处理
        if (type === 'pedestrian') return;
      } else {
        // 程序化 fallback 前轴转向（1:1 显示前轮角，见上方 glTF 路径注释；
        // 加死区滤波，避免直路巡航时微动）
        const vis = _getVisGroup(entry);
        if (vis && vis.userData && vis.userData.frontAxle) {
          const steering = _steeringVisualState(entry && entry.steerAngle);
          vis.userData.frontAxle.rotation.y = steering.frontAxleYaw;
        }
      }

      // 模型和 fallback 都复用同一套可见灯光反馈；有语义 emissive 节点
      // 的模型同时保留其材质亮度控制。
      if (entry.lights) {
        entry.lights.update(v, now);
      }

      // glTF 模型 emissive 灯光（含 ads_indicator 小蓝灯）
      const vis = _getVisGroup(entry);
      if (vis && vis.userData &&
          (vis.userData.su7RawLights ||
           vis.userData.brakeLights ||
           vis.userData.turnSignals ||
           vis.userData.headlights ||
           vis.userData.adsIndicators)) {
        const ls = deriveLightState(v.lights || 0, v.brake || 0);
        _setVehicleLights(vis, ls, now !== undefined ? now / 1000 : undefined);
      }
    };

    animateVehicle('ego', 'su7');
    for (const ent of entities) {
      if (ent && ent.id) animateVehicle(ent.id, ent.type);
    }
  }

  return { update, updateVehicle, removeVehicle, dispose, getVehicleGroup, getVehicleMap };
}