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

import { deriveLightState, LIGHT_TURN_LEFT, LIGHT_TURN_RIGHT, LIGHT_HAZARD, LIGHT_HIGH_BEAM, LIGHT_LOW_BEAM, LIGHT_CLEARANCE, LIGHT_FOG } from './VehicleLights.js';
import { getStdMaterial } from '../core/AssetFactory.js';
import { initModelCache, getModel, _setVehicleLights, _relinkWheelUserData } from '../../models.js';
import { worldToThree, headingToRotationY, forwardENU } from '../math/Coord.js';
import { roadHeightAt } from '../math/RoadHeight.js';

// ═══════════════════════════════════════════════════════════
// createVehicleLights — THREE 灯光网格工厂（原在 VehicleLights.js，
// 移到此处以保持 VehicleLights.js 零 THREE 依赖，供 Node 单元测试）
// ═══════════════════════════════════════════════════════════

const LIGHT_OFF = new THREE.Color(0x111111);
const LIGHT_BRAKE_ON = new THREE.Color(0xff0000);
const LIGHT_TURN_ON = new THREE.Color(0xff8800);
const LIGHT_HEAD_ON = new THREE.Color(0xffffcc);
const LIGHT_FOG_ON = new THREE.Color(0xffea66);

// 车辆接地偏移：车辆模型原点=路面(y=0，轮子下缘≈0)，而渲染路面画在
// RoadView.Y_ROAD=0.10（车道线/边线防 z-fight 的微抬升）。车辆 y 取所在位置
// 的道路高度(roadHeightAt) + 此偏移，保证匝道/高架高度变化时仍贴合渲染路面。
export const VEHICLE_GROUND_Y = 0.10;

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
  const mat = new THREE.MeshBasicMaterial({
    color,
    side: THREE.DoubleSide,
    transparent: true,
    opacity: 0.95,
    depthWrite: false,
    blending: THREE.AdditiveBlending,
  });
  const m = new THREE.Mesh(geo, mat);
  // Vehicles are X-forward in the shared ENU/THREE contract; place the
  // rectangular light on the front/rear YZ face rather than the old Z face.
  m.rotation.y = Math.PI / 2;
  m.position.set(x, y, z);
  m.renderOrder = 14;
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

let _headlightCarpetTex = null;
function getHeadlightCarpetTexture() {
  if (_headlightCarpetTex) return _headlightCarpetTex;
  if (typeof document === 'undefined' || !document.createElement) return null;
  const canvas = document.createElement('canvas');
  canvas.width = 512;
  canvas.height = 512;
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#000000';
  ctx.fillRect(0, 0, 512, 512);

  // 双透镜近光聚光核心 + ECE Z型低高切线光形（左低右高防眩）
  const gL = ctx.createRadialGradient(180, 140, 0, 180, 140, 240);
  gL.addColorStop(0.0, 'rgba(255, 255, 255, 1.0)');
  gL.addColorStop(0.18, 'rgba(255, 250, 240, 0.95)');
  gL.addColorStop(0.45, 'rgba(240, 245, 255, 0.65)');
  gL.addColorStop(0.75, 'rgba(220, 235, 255, 0.25)');
  gL.addColorStop(1.0, 'rgba(0, 0, 0, 0.0)');
  ctx.fillStyle = gL;
  ctx.beginPath();
  ctx.arc(180, 140, 240, 0, Math.PI * 2);
  ctx.fill();

  const gR = ctx.createRadialGradient(332, 120, 0, 332, 120, 260);
  gR.addColorStop(0.0, 'rgba(255, 255, 255, 1.0)');
  gR.addColorStop(0.18, 'rgba(255, 250, 240, 0.95)');
  gR.addColorStop(0.50, 'rgba(240, 245, 255, 0.70)');
  gR.addColorStop(0.80, 'rgba(220, 235, 255, 0.30)');
  gR.addColorStop(1.0, 'rgba(0, 0, 0, 0.0)');
  ctx.fillStyle = gR;
  ctx.beginPath();
  ctx.arc(332, 120, 260, 0, Math.PI * 2);
  ctx.fill();

  const gSpread = ctx.createRadialGradient(256, 80, 10, 256, 120, 340);
  gSpread.addColorStop(0.0, 'rgba(255, 255, 255, 0.85)');
  gSpread.addColorStop(0.35, 'rgba(245, 248, 255, 0.50)');
  gSpread.addColorStop(0.70, 'rgba(200, 220, 255, 0.18)');
  gSpread.addColorStop(1.0, 'rgba(0, 0, 0, 0.0)');
  ctx.fillStyle = gSpread;
  ctx.beginPath();
  ctx.arc(256, 120, 340, 0, Math.PI * 2);
  ctx.fill();

  _headlightCarpetTex = new THREE.CanvasTexture(canvas);
  return _headlightCarpetTex;
}

let _headlightFlareTex = null;
function getHeadlightFlareTexture() {
  if (_headlightFlareTex) return _headlightFlareTex;
  if (typeof document === 'undefined' || !document.createElement) return null;
  const canvas = document.createElement('canvas');
  canvas.width = 128;
  canvas.height = 128;
  const ctx = canvas.getContext('2d');
  const g = ctx.createRadialGradient(64, 64, 0, 64, 64, 64);
  g.addColorStop(0.0, 'rgba(255, 255, 255, 1.0)');
  g.addColorStop(0.15, 'rgba(255, 252, 245, 0.90)');
  g.addColorStop(0.40, 'rgba(210, 235, 255, 0.45)');
  g.addColorStop(0.70, 'rgba(150, 190, 255, 0.15)');
  g.addColorStop(1.0, 'rgba(0, 0, 0, 0.0)');
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(64, 64, 64, 0, Math.PI * 2);
  ctx.fill();
  _headlightFlareTex = new THREE.CanvasTexture(canvas);
  return _headlightFlareTex;
}

let _headlightBeamTex = null;
function getHeadlightBeamTexture() {
  if (_headlightBeamTex) return _headlightBeamTex;
  if (typeof document === 'undefined' || !document.createElement) return null;
  const canvas = document.createElement('canvas');
  canvas.width = 256;
  canvas.height = 64;
  const ctx = canvas.getContext('2d');
  const g = ctx.createLinearGradient(0, 0, 256, 0);
  g.addColorStop(0.0, 'rgba(255, 255, 255, 1.0)');
  g.addColorStop(0.08, 'rgba(255, 250, 245, 0.90)');
  g.addColorStop(0.35, 'rgba(230, 242, 255, 0.55)');
  g.addColorStop(0.70, 'rgba(180, 215, 255, 0.20)');
  g.addColorStop(1.0, 'rgba(0, 0, 0, 0.0)');
  ctx.fillStyle = g;
  ctx.fillRect(0, 0, 256, 64);
  _headlightBeamTex = new THREE.CanvasTexture(canvas);
  return _headlightBeamTex;
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

  const isSu7 = (vehicleGroup && vehicleGroup.userData && vehicleGroup.userData.modelType === 'su7');
  const headX = isSu7 ? 2.28 : HEAD_X;
  const headY = isSu7 ? 0.70 : HEAD_Y;
  const headZ = isSu7 ? 0.72 : HEAD_Z;

  // In the shared X-forward frame, left is -Z and right is +Z. Keep each
  // pair on the same front/rear face; the previous wiring swapped X/Z and
  // put one brake/turn/head lamp at the wrong end of the car.
  const brakeL = _makeRectMesh(GEO_BRAKE, LIGHT_OFF, BRAKE_X, BRAKE_Y, -BRAKE_Z);
  const brakeR = _makeRectMesh(GEO_BRAKE, LIGHT_OFF, BRAKE_X, BRAKE_Y,  BRAKE_Z);
  const turnFL = _makeRectMesh(GEO_TURN, LIGHT_OFF, frontTurn.x, frontTurn.y, -TURN_Z);
  const turnFR = _makeRectMesh(GEO_TURN, LIGHT_OFF, frontTurn.x, frontTurn.y,  TURN_Z);
  const turnRL = _makeRectMesh(GEO_TURN, LIGHT_OFF, rearTurn.x, rearTurn.y, -TURN_Z);
  const turnRR = _makeRectMesh(GEO_TURN, LIGHT_OFF, rearTurn.x, rearTurn.y,  TURN_Z);
  const headL  = _makeRectMesh(GEO_HEAD, LIGHT_OFF, headX, headY, -headZ);
  const headR  = _makeRectMesh(GEO_HEAD, LIGHT_OFF, headX, headY,  headZ);

  // 熄灯时隐藏，避免暗色灯片显示为车身边缘的黑方块
  brakeL.visible = brakeR.visible = false;
  turnFL.visible = turnFR.visible = turnRL.visible = turnRR.visible = false;
  headL.visible = headR.visible = false;

  group.add(brakeL, brakeR, turnFL, turnFR, turnRL, turnRR, headL, headR);

  return {
    group,
    update(v, nowMs, env) {
      const s = deriveLightState(v.lights || 0, v.brake || 0, env);
      const blinkOn = nowMs === undefined
        ? true
        : (((nowMs / 1000) * 1.5) % 1) < 0.5;

      // 刹车与示廓尾灯：刹车亮红(opacity 1.0)，示廓/大灯开启时常亮(opacity 0.45 示位红)
      const isTail = s.clearance || s.head || s.fog;
      brakeL.visible = showBrakeOverlay && (s.brake || isTail);
      brakeR.visible = showBrakeOverlay && (s.brake || isTail);
      brakeL.material.color.copy(LIGHT_BRAKE_ON);
      brakeR.material.color.copy(LIGHT_BRAKE_ON);
      brakeL.material.opacity = s.brake ? 1.0 : 0.45;
      brakeR.material.opacity = s.brake ? 1.0 : 0.45;

      const turnL = showTurnOverlay && s.turnL && blinkOn;
      const turnR = showTurnOverlay && s.turnR && blinkOn;
      turnFL.visible = turnRL.visible = turnL;
      turnFR.visible = turnRR.visible = turnR;
      turnFL.material.color.copy(LIGHT_TURN_ON);
      turnFR.material.color.copy(LIGHT_TURN_ON);
      turnRL.material.color.copy(LIGHT_TURN_ON);
      turnRR.material.color.copy(LIGHT_TURN_ON);

      // 前大灯：近光高亮璀璨白光(0xfffaee, opacity 0.95)，示廓开启时微光(0.40)
      const isHeadOn = s.head || s.clearance;
      headL.visible = showHeadOverlay && isHeadOn;
      headR.visible = showHeadOverlay && isHeadOn;
      headL.material.color.copy(s.head ? LIGHT_HEAD_ON : new THREE.Color(0xffeedd));
      headR.material.color.copy(s.head ? LIGHT_HEAD_ON : new THREE.Color(0xffeedd));
      headL.material.opacity = s.head ? 0.95 : 0.40;
      headR.material.opacity = s.head ? 0.95 : 0.40;
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

function _isLightMesh(mesh) {
  if (!mesh) return false;
  const name = (mesh.name || '').toLowerCase();
  if (name.includes('light') || name.includes('lamp') || name.includes('lens') || name.includes('fog')) return true;
  const mat = mesh.material;
  const matName = Array.isArray(mat) ? mat.map(m => m && m.name || '').join(' ') : (mat && mat.name || '');
  if (/car_ight|light|lamp|emissive|fog/i.test(matName)) return true;
  return false;
}

/** 判断 mesh 是否是车身（需要车漆升级） */
function _isBodyMesh(mesh) {
  if (_isLightMesh(mesh)) return false;
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

/** 新能源绿牌纹理，依据 GA 36-2018《中华人民共和国机动车号牌》小型新能源
 *  汽车号牌规格绘制（Canvas 1px=1mm，零外部资源）：
 *  - 外廓 440×140mm，8 字符（省简称 + 发牌字母 + 6 位序号）；
 *  - 字高 90mm；前两字宽 45mm，后 6 字宽 43mm，字距 9mm；
 *  - 渐变绿底、黑字、黑框线，第 2/3 字间有间隔圆点。
 *  逐字符排版并按实测字宽横向压缩，避免方形西文字体把 8 个字挤出板面。 */
function _makeNewEnergyPlateTexture(plateText) {
  const W = 440, H = 140;
  const c = document.createElement('canvas');
  c.width = W; c.height = H;
  const ctx = c.getContext('2d');

  // 渐变绿底（上亮下深）
  const g = ctx.createLinearGradient(0, 0, 0, H);
  g.addColorStop(0, '#3fc05a');
  g.addColorStop(1, '#0d7d3a');
  ctx.fillStyle = g;
  ctx.fillRect(0, 0, W, H);

  const text = String(plateText || '京A6666666').slice(0, 8);

  // GA 36-2018 字符布局（mm）：字高 90，前两字 45 宽、后六字 43 宽
  const widths = [45, 45, 43, 43, 43, 43, 43, 43];
  const gap = 9;
  const contentW = widths.reduce((a, b) => a + b, 0) + gap * (widths.length - 1);
  let x = (W - contentW) / 2;
  const centers = widths.map((w) => { x += w / 2; const cx = x; x += w / 2 + gap; return cx; });

  // 黑框线（距边 5mm，线宽 2mm）
  ctx.strokeStyle = '#0a0a0a';
  ctx.lineWidth = 2;
  ctx.strokeRect(5, 5, W - 10, H - 10);

  // 第 2、3 字符之间的间隔圆点（直径 8mm）
  ctx.fillStyle = '#0a0a0a';
  ctx.beginPath();
  ctx.arc((centers[1] + widths[1] / 2 + centers[2] - widths[2] / 2) / 2, H / 2, 4, 0, Math.PI * 2);
  ctx.fill();

  // 逐字符绘制：测量实际字宽后横向压缩进 43/45mm 字格（只压不拉，窄字符如 1 保留自然字宽）
  ctx.fillStyle = '#0a0a0a';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.font = '900 90px "Arial Black","PingFang SC","Microsoft YaHei","Noto Sans CJK SC",sans-serif';
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    const targetW = widths[i] || 43;
    const measured = ctx.measureText(ch).width || targetW;
    const sx = Math.min(1, targetW / measured);
    ctx.save();
    ctx.translate(centers[i], H / 2);
    ctx.scale(sx, 1);
    ctx.fillText(ch, 0, 2);
    ctx.restore();
  }

  const tex = new THREE.CanvasTexture(c);
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.anisotropy = 4;
  return tex;
}

/** 给 su7 叠加新能源绿牌（前后各一块）。
 *  定位全部来自模型自带 "ChePai" 节点的实测顶点（解码 meshopt 压缩后取世界坐标）：
 *  - 前牌：垂直平面，位于前保险杠凹槽内，x≈2.627、y 中心≈0.546、z≈0，
 *    尺寸 0.428(宽)×0.133(高)；外框唇边在 x≈2.630，车牌收进框内 3mm。
 *  - 后牌：随尾门倾斜，绝非竖直——底边靠后 x≈-2.584、顶边靠前 x≈-2.536，
 *    中心 (-2.560, 0.502, 0)，尺寸 0.428×0.135。用基底矩阵把平面旋到贴合
 *    尾门斜面（法线朝后并略向上）。
 *  车牌尺寸取 ChePai 实测值而非国标 0.48×0.14，以恰好落进原厂预留框。 */
function _applyNewEnergyPlate(scene, plateText) {
  const tex = _makeNewEnergyPlateTexture(plateText);
  const mat = new THREE.MeshStandardMaterial({ map: tex, roughness: 0.35, metalness: 0.1 });

  // 前牌：垂直，正面朝 +X。比 ChePai(2.627) 前出 1mm 避免 z-fight，仍收在唇边(2.630)内。
  const front = new THREE.Mesh(new THREE.PlaneGeometry(0.428, 0.133), mat);
  front.rotation.y = Math.PI / 2;                 // 纹理右沿世界 -Z，正面朝 +X
  front.position.set(2.628, 0.546, -0.001);
  scene.add(front);

  // 后牌：贴合倾斜尾门。局部 X(宽)→世界 +Z，局部 Y(高)→尾门向上方向，
  // 局部 Z(正面)→外法线（朝后且略向上）。
  const rear = new THREE.Mesh(new THREE.PlaneGeometry(0.428, 0.135), mat);
  const right = new THREE.Vector3(0, 0, 1);
  // 底边中心 (-2.584, 0.439) → 顶边中心 (-2.536, 0.567)
  const up = new THREE.Vector3(0.048, 0.128, 0).normalize();
  const normal = new THREE.Vector3().crossVectors(right, up).normalize();
  rear.quaternion.setFromRotationMatrix(new THREE.Matrix4().makeBasis(right, up, normal));
  rear.position.set(-2.560, 0.502, 0.0).addScaledVector(normal, 0.003);
  scene.add(rear);
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

/**
 * 车轮滚动里程（沿车头方向累计的有符号位移）。
 * 用死推算的**平滑位姿**（NPC 的 smoothX，ego 的 _dr.smoothX）逐帧位移来
 * 积分，而不是 speed×帧间隔：平滑位姿本就是帧率无关、无抖动地推进的，所以
 * 车轮角速度与渲染帧率 / 速度信号抖动完全解耦——车速不变时车轮匀速转动，
 * 不再忽快忽慢。直接用 ego 的 raw lastX 不行：lastX 只在 SSE tick 落点才变
 * （心跳去重丢弃 <1cm 的重复帧），两次 tick 之间恒定，里程会"台阶式"跳变
 * → 车轮反而更卡。相机仍用 raw lastX（定案：raw 无顿挫），与此处互不影响。
 * 首帧无历史位姿时回退 speed×dt，保证 dt/speed 仍被引用。
 * @returns {number} 累计滚动里程（米，可正可负）
 */
export function _advanceWheelOdometry(entry, v, dt, speed_mps) {
  // 优先用平滑位姿（ego 经 SceneDirector 注入 smoothX/smoothZ/smoothHeading；
  // NPC ent.x 本就是 smooth）。拿不到时回退 raw。
  const ex = v.smoothX ?? v.x ?? v.px ?? 0;
  const ey = v.smoothZ ?? v.y ?? v.py ?? 0;
  const h = v.smoothHeading ?? v.heading ?? v.yaw ?? 0;
  if (entry._odoPrevX === undefined) {
    entry._odoPrevX = ex; entry._odoPrevY = ey;
    entry._odoDist = (Number.isFinite(speed_mps) ? speed_mps : 0) * (dt || 0);
    return entry._odoDist;
  }
  const dx = ex - entry._odoPrevX;
  const dy = ey - entry._odoPrevY;
  entry._odoPrevX = ex; entry._odoPrevY = ey;
  // 沿车头方向取有符号位移（支持倒车）：位移在 forwardENU(h) 上的投影。
  const [fx, fy] = forwardENU(h);
  let d = dx * fx + dy * fy;
  // 钳制：正常每帧位移 = speed×dt；超过约 3 倍预期值的突变视为瞬移 / 重连
  // 落点，钳住避免车轮被异常位移带着整圈疯转。这样高速 + 低帧率（如非
  // observe 10fps）下的正常大位移不会被误杀，仍能匀速转动。
  const cap = Math.max(0.5, (Number.isFinite(speed_mps) ? speed_mps : 0) * (dt || 0) * 3);
  if (d > cap) d = cap; else if (d < -cap) d = -cap;
  entry._odoDist = (entry._odoDist || 0) + d;
  return entry._odoDist;
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
    // 新能源绿牌（京A6666666，前后各一块）
    if (type === 'su7') _applyNewEnergyPlate(scene, '京A6666666');
    // 注入方向盘：sedan/suv/truck 由 gen_models.py 生成，只有四个外露车轮、
    // 没有内饰，需要程序化补一个方向盘以提供转向反馈。SU7 是授权高精度模型，
    // 驾驶位自带方向盘（interior.009 @ 左舵驾驶位），不能再叠一个丑的。
    if (type !== 'su7') scene.add(_createSteeringWheel());
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

    // 车轮里程（沿车头方向累计位移）：每帧只算一次、所有车轮共享同一个值，
    // 否则在 vis.traverse 里逐轮调用会重复累加 entry._odoPrevX，导致第 2 个
    // 起的车轮 dx=0 永不转动。里程只与"实际走过的路程"相关，与渲染帧率 /
    // 速度信号抖动完全解耦——车速不变时车轮匀速滚动，不再忽快忽慢。
    const wheelOdo = _advanceWheelOdometry(entry, v, dt, speed_mps);

    // 前轴转向：glTF 模型 axle_front（含 wheel_FL/wheel_FR）整体绕 Y 旋转。
    // 仿真 steer>0 会让 ENU y/heading 增大，即左转；THREE +Y 也把 +X
    // 车头转向 -Z（左侧），所以不再额外取反。steer<0 正确显示右转。
    // 1:1 映射（2026-08 修复）：后端 step_bicycle 的 steer 就是前轮转角 δ，
    // 直接显示 —— 变道 0.05-0.1 rad ≈ 3-6°（可见），掉头满舵 0.45-0.60 rad
    // ≈ 26-34°（真实前轮最大转角）。旧乘子 0.6 把前轮角又缩小 40%，
    // 变道时前轮只转 2-3.5° 肉眼不可见 → "前轮不会动"。
    // 死区滤波：|steer| < 0.005 时视为 0，避免直路巡航时方向盘和车轮微动。
    const steering = _steeringVisualState(entry && entry.steerAngle);
    /* 前轴转向：让每个前轮绕**自身**垂直轴(kingpin)原地转向，而不是 yaw 整个
     * axle_front。整体旋转 axle_front 会让左右前轮一起绕轴心平移——掉头满舵
     * (0.58 rad) 下各轮 ~0.47m 脱离轮拱，看起来"车轮漂移"（wheel_FL/FR 是
     * axle_front 的子节点，几何居中于自身原点，rotation.y 正好绕轮心转向）。
     * 真实前轮绕各自 kingpin 转向、轮心不动，故只设前轮 rotation.y。 */
    const fa = vis.userData && vis.userData.frontAxle;
    if (fa) {
      fa.rotation.y = 0;
      fa.traverse((child) => {
        if (!child.isMesh || _isSteeringWheelNode(child.name)) return;
        if (/wheel|tire|tyre/i.test(child.name || '')) {
          child.rotation.y = steering.frontAxleYaw;
        }
      });
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

      // 车轮滚动：用里程累计（沿车头方向位移）而非 speed×dt，彻底与渲染帧率 /
      // 速度信号抖动解耦。车速不变时每帧位移恒定 → 车轮匀速转动，无忽快忽慢。
      // 转向（rotation.y / kingpin）与滚动（rotation.x|z）是独立自由度，
      // 故 glTF 前轮即便在打方向时也照常滚动，不再"锁死停转"。
      if (!_isSteeringWheelNode(child.name) &&
          (name.includes('wheel') || name.includes('tire') || name.includes('tyre'))) {
        const radius = 0.35;
        const TAU = Math.PI * 2;
        const wheelAngle = (wheelOdo / radius) % TAU;
        if (child.userData && child.userData.rollAxis === 'z') {
          child.rotation.z = wheelAngle;
        } else {
          child.rotation.x = wheelAngle;
        }
      }
    });
  }

  // ── 公共 API ──

  /** 添加或更新一辆车 */
  function updateVehicle(id, vehicleData, type, store) {
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

      // SU7 与 glTF 真实车辆：完全由真实 3D 灯罩与 4 透镜矩阵几何发光（emissive），
      // 绝不在精致模型表面叠加扁平悬浮 PlaneGeometry 贴片；仅在 procedural fallback 简模上叠加灯光
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
      const ex = vehicleData.x || vehicleData.px || 0;
      const ey = vehicleData.y || vehicleData.py || 0;
      const [tx, , tz] = worldToThree(ex, ey, 0);
      // 车辆贴地：高度 = 所在位置的路面高度(roadHeightAt，跟随匝道/高架坡度)
      // + 渲染路面厚度偏移(RoadView.Y_ROAD，车道线防 z-fight 的微抬升)。
      // 用道路高度而非数据 z，保证道路高度变化（匝道/高架）时车辆始终贴合路面，
      // 轮子不会陷入路面也不会悬空。
      const roadZ = store ? roadHeightAt(store, ex, ey, vehicleData.z) : (vehicleData.z || 0);
      const ty = roadZ + VEHICLE_GROUND_Y;
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
      updateVehicle(egoId, store.ego, 'su7', store);
    }

    // 2. 其他实体（car/truck/suv/pedestrian — 排除红绿灯/ETC/停止线等非车辆）
    const VEHICLE_TYPES = new Set(['car', 'suv', 'truck', 'pedestrian']);
    const entities = store.entities || [];
    for (const ent of entities) {
      if (!ent || !ent.id) continue;
      if (!VEHICLE_TYPES.has(ent.type)) continue;
      activeIds.add(ent.id);
      updateVehicle(ent.id, ent, ent.type || 'car', store);
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
        // 程序化 fallback（_buildSedan）前轮 kingpin 转向 + 全轮滚动。
        const vis = _getVisGroup(entry);
        if (vis && vis.userData && vis.userData.wheels) {
          const steering = _steeringVisualState(entry && entry.steerAngle);
          const yaw = steering.frontAxleYaw;
          // 前轮转向：绕各自 kingpin（轮心垂直轴，pivot 在轮心）→ 原地转不漂移。
          vis.userData.wheels.forEach((w) => {
            if (w.userData && w.userData.kingpin) {
              w.userData.kingpin.rotation.y = yaw;
            }
          });
          // 车轮滚动：同样用里程累计（与 glTF 路径共享 _advanceWheelOdometry，
          // 每帧只调一次避免逐轮重复累加）。半径取程序化模型的实际轮胎半径。
          const speed_mps = v
            ? (Number.isFinite(v.speed_mps) ? v.speed_mps
              : (Number.isFinite(v.speed) ? v.speed : 0))
            : 0;
          const radius = vis.userData.wheelRadius || 0.33;
          const wheelOdo = _advanceWheelOdometry(entry, v, dt, speed_mps);
          const TAU = Math.PI * 2;
          const wheelAngle = (wheelOdo / radius) % TAU;
          vis.userData.wheels.forEach((w) => {
            if (w.userData && w.userData.rollAxis === 'z') {
              w.rotation.z = wheelAngle;
            } else {
              w.rotation.x = wheelAngle;
            }
          });
        }
      }

      const userOverride = (id === 'ego' && store.userLightOverride !== undefined && store.userLightOverride !== null)
        ? store.userLightOverride : null;
      const effectiveLights = (userOverride !== null) ? userOverride : (v.lights || 0);
      const ls = deriveLightState(effectiveLights, v.brake || 0, store.env);

      // 模型和 fallback 都复用同一套可见灯光反馈；有语义 emissive 节点
      // 的模型同时保留其材质亮度控制。
      if (entry.lights) {
        entry.lights.update({ ...v, lights: effectiveLights }, now, store.env);
      }

      // glTF 模型 emissive 灯光（含 ads_indicator 小蓝灯与后雾灯）
      const vis = _getVisGroup(entry);
      if (vis && vis.userData &&
          (vis.userData.su7RawLights ||
           vis.userData.rearFogLight ||
           vis.userData.brakeLights ||
           vis.userData.turnSignals ||
           vis.userData.headlights ||
           vis.userData.adsIndicators)) {
        _setVehicleLights(vis, ls, now !== undefined ? now / 1000 : undefined);
      }

      // 真实环境物理光照系统：大灯铺路切线光斑、双前灯物理投影光源、车尾刹车红光与立体空间光锥
      if (id === 'ego' && entry.group) {
        if (!entry.headlightSpotL && THREE && typeof THREE.SpotLight === 'function') {
          // 1. 左右前大灯真实物理 SpotLight 光源（真实照亮前方道路、车道线与周围场景）
          const spotL = new THREE.SpotLight(0xfffdf5, 0, 120, Math.PI / 3.0, 0.8, 1.0);
          spotL.position.set(2.35, 0.70, -0.68);
          const targetL = new THREE.Object3D();
          targetL.position.set(45.0, -0.1, -0.80);
          entry.group.add(spotL);
          entry.group.add(targetL);
          spotL.target = targetL;
          entry.headlightSpotL = spotL;

          const spotR = new THREE.SpotLight(0xfffdf5, 0, 120, Math.PI / 3.0, 0.8, 1.0);
          spotR.position.set(2.35, 0.70, 0.68);
          const targetR = new THREE.Object3D();
          targetR.position.set(45.0, -0.1, 0.80);
          entry.group.add(spotR);
          entry.group.add(targetR);
          spotR.target = targetR;
          entry.headlightSpotR = spotR;

          // 2. 车头左右大灯透镜晶莹光晕 (Lens Corona Glow Flare Sprites)
          if (THREE.Sprite && THREE.SpriteMaterial) {
            const flareTex = getHeadlightFlareTexture();
            const flareMatL = new THREE.SpriteMaterial({
              map: flareTex,
              color: 0xffffff,
              transparent: true,
              opacity: 0.0,
              blending: THREE.AdditiveBlending,
              depthWrite: false,
            });
            const flareL = new THREE.Sprite(flareMatL);
            flareL.position.set(2.32, 0.70, -0.68);
            flareL.scale.set(0.95, 0.95, 1.0);
            entry.group.add(flareL);
            entry.headlightFlareL = flareL;

            const flareMatR = new THREE.SpriteMaterial({
              map: flareTex,
              color: 0xffffff,
              transparent: true,
              opacity: 0.0,
              blending: THREE.AdditiveBlending,
              depthWrite: false,
            });
            const flareR = new THREE.Sprite(flareMatR);
            flareR.position.set(2.32, 0.70, 0.68);
            flareR.scale.set(0.95, 0.95, 1.0);
            entry.group.add(flareR);
            entry.headlightFlareR = flareR;
          }

          // 3. 车尾刹车地面物理点光源（刹车时照亮车尾地表与后车）
          if (THREE.PointLight) {
            const brakeLight = new THREE.PointLight(0xff1800, 0, 16, 1.2);
            brakeLight.position.set(-2.40, 0.60, 0.0);
            entry.group.add(brakeLight);
            entry.brakePointLight = brakeLight;
          }

          // 4. 铺路光毯（前大灯开启时投射在车头前方的宽幅切线地面高光）
          if (THREE.PlaneGeometry && THREE.MeshBasicMaterial) {
            const carpetGeo = new THREE.PlaneGeometry(14.0, 52.0);
            const carpetTex = getHeadlightCarpetTexture();
            const carpetMat = new THREE.MeshBasicMaterial({
              color: 0xffffff,
              map: carpetTex,
              transparent: true,
              opacity: 0.0,
              depthWrite: false,
              blending: THREE.AdditiveBlending,
              side: THREE.DoubleSide,
            });
            const carpetMesh = new THREE.Mesh(carpetGeo, carpetMat);
            carpetMesh.rotation.x = -Math.PI / 2;
            carpetMesh.rotation.z = -Math.PI / 2;
            carpetMesh.position.set(26.0, 0.03, 0.0);
            carpetMesh.renderOrder = 8;
            entry.group.add(carpetMesh);
            entry.headlightCarpet = carpetMesh;

            // 车尾刹车地表红色光晕
            const brakeCarpetGeo = new THREE.PlaneGeometry(3.6, 4.8);
            const brakeCarpetMat = new THREE.MeshBasicMaterial({
              color: 0xff1100,
              transparent: true,
              opacity: 0.0,
              depthWrite: false,
              blending: THREE.AdditiveBlending,
              side: THREE.DoubleSide,
            });
            const brakeCarpetMesh = new THREE.Mesh(brakeCarpetGeo, brakeCarpetMat);
            brakeCarpetMesh.rotation.x = -Math.PI / 2;
            brakeCarpetMesh.rotation.z = -Math.PI / 2;
            brakeCarpetMesh.position.set(-3.6, 0.03, 0.0);
            brakeCarpetMesh.renderOrder = 8;
            entry.group.add(brakeCarpetMesh);
            entry.brakeCarpet = brakeCarpetMesh;
          }

          // 5. 左右前大灯 3D 立体光学光锥（Volumetric Light Beams，从透镜柱射出穿透黑夜）
          if (THREE.CylinderGeometry && THREE.MeshBasicMaterial) {
            const beamGeo = new THREE.CylinderGeometry(0.12, 2.4, 38.0, 16, 1, true);
            beamGeo.rotateZ(-Math.PI / 2);
            beamGeo.translate(19.0, 0, 0); // 几何中心向 +X 前移 19m，原点对齐透镜位置
            const beamTex = getHeadlightBeamTexture();
            const beamMat = new THREE.MeshBasicMaterial({
              color: 0xfffdf5,
              map: beamTex,
              transparent: true,
              opacity: 0.0,
              depthWrite: false,
              blending: THREE.AdditiveBlending,
              side: THREE.DoubleSide,
            });
            const beamL = new THREE.Mesh(beamGeo, beamMat);
            beamL.position.set(2.26, 0.70, -0.72);
            beamL.rotation.y = -0.02;
            beamL.renderOrder = 9;

            const beamR = new THREE.Mesh(beamGeo, beamMat);
            beamR.position.set(2.26, 0.70, 0.72);
            beamR.rotation.y = 0.02;
            beamR.renderOrder = 9;

            const beamsGroup = new THREE.Group();
            beamsGroup.add(beamL, beamR);
            entry.group.add(beamsGroup);
            entry.headlightBeams = beamsGroup;
            entry.headlightBeamMat = beamMat;
          }
        }

        // 环境感知与昼夜判定
        const isNight = !!(store.env && (store.env.isNight || store.env.lighting === 'night'));
        const isDim = isNight || !!(store.env && (store.env.lighting === 'dusk' || store.env.lighting === 'dawn')) || !!ls.fog;

        // 1. 物理 SpotLight 光源：白天关闭，仅在夜间/黄昏/浓雾且大灯开启时柔和照亮前方道路
        const spotIntensity = ls.head
          ? (ls.fog ? 320.0 : (isNight ? 220.0 : (isDim ? 120.0 : 0.0)))
          : 0.0;
        if (entry.headlightSpotL) entry.headlightSpotL.intensity = spotIntensity;
        if (entry.headlightSpotR) entry.headlightSpotR.intensity = spotIntensity;

        // 2. 透镜光晕（Flare Sprite）：白天仅柔和晶莹点缀(0.20)，夜间自然发光(0.50)
        const flareOp = ls.head
          ? (isDim ? 0.50 : 0.20)
          : (ls.clearance ? (isDim ? 0.18 : 0.08) : 0.0);
        if (entry.headlightFlareL && entry.headlightFlareL.material) {
          entry.headlightFlareL.material.opacity = flareOp;
          entry.headlightFlareL.visible = flareOp > 0.001;
        }
        if (entry.headlightFlareR && entry.headlightFlareR.material) {
          entry.headlightFlareR.material.opacity = flareOp;
          entry.headlightFlareR.visible = flareOp > 0.001;
        }

        // 3. 车尾刹车地面物理点光源
        if (entry.brakePointLight) {
          entry.brakePointLight.intensity = ls.brake ? 35.0 : (isDim && ls.head ? 4.0 : 0.0);
        }

        // 4. 铺路光毯：大白天完全隐藏，仅夜间/雾天显现自然沥青路面漫反射切线光斑
        if (entry.headlightCarpet && entry.headlightCarpet.material) {
          const carpetOp = ls.head
            ? (ls.fog ? 0.45 : (isNight ? 0.38 : (isDim ? 0.20 : 0.0)))
            : 0.0;
          entry.headlightCarpet.material.opacity = carpetOp;
          entry.headlightCarpet.visible = carpetOp > 0.001;
        }
        if (entry.brakeCarpet && entry.brakeCarpet.material) {
          const brakeCarpetOp = ls.brake ? (isDim ? 0.35 : 0.20) : (isDim && ls.head ? 0.08 : 0.0);
          entry.brakeCarpet.material.opacity = brakeCarpetOp;
          entry.brakeCarpet.visible = brakeCarpetOp > 0.001;
        }

        // 5. 3D 立体空间光锥：白天完全隐藏，仅在浓雾/夜间显现淡淡的丁达尔穿透光
        if (entry.headlightBeamMat) {
          let beamOpacity = 0.0;
          if (ls.head) {
            if (ls.fog) {
              beamOpacity = 0.32;
            } else if (isNight) {
              beamOpacity = 0.14;
            } else if (isDim) {
              beamOpacity = 0.08;
            }
          }
          entry.headlightBeamMat.opacity = beamOpacity;
          if (entry.headlightBeams) {
            entry.headlightBeams.visible = beamOpacity > 0.001;
          }
        }
      }
    };

    animateVehicle('ego', 'su7');
    for (const ent of entities) {
      if (ent && ent.id) animateVehicle(ent.id, ent.type);
    }
  }

  return { update, updateVehicle, removeVehicle, dispose, getVehicleGroup, getVehicleMap };
}
