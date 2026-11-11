/**
 * EffectView.js — 车周动态视觉特效（制动尾光 / 双闪告警 / 旁车切入贴地警示翼 / 雨天轮胎水雾）
 *
 * 职责边界：
 *   - 全局雨丝、地面雨滴涟漪与雪花由 SkyEnv.js 统一渲染（避免重复叠加方形粒子点）
 *   - 3D 障碍物感知框与点云由 PerceptionView.js 统一渲染（移除旧头顶悬浮方块）
 *   - 本视图专注车周物理与安全告警贴地特效：
 *       1. 紧急制动车尾贴地红光晕（brake > 0.5）
 *       2. 紧急双闪光效（hazard 标志位激活）
 *       3. 旁车切入贴地双 V 警示翼（按车体航向与切入侧法线贴地放置）
 *       4. 降雨天气高速行驶双后轮水雾尾迹（仅 rain/storm 且车速 > 2 m/s）
 *
 * 所有坐标/朝向/法线偏移均走 Coord.js，所有颜色均来自 tokens.js (SCENE)。
 */

import {
  worldToThree,
  forwardENU,
  headingToRotationY,
  tangentToNormal,
  offsetAlongNormal,
  distanceENU,
} from '../math/Coord.js';
import { roadHeightAt } from '../math/RoadHeight.js';
import { SCENE } from '../theme/tokens.js';

const BRAKE_THRESHOLD = 0.5;
const CUTIN_LATERAL_SPEED = 1.8;   // 切入预警横向速度阈值 (m/s)
const CUTIN_MAX_DIST_M = 28.0;     // 切入预警纵向/欧氏距离门限 (m)
const CUTIN_LIFT_M = 0.17;         // 贴地警示翼离地高度 (m，高于路面 0.10m 与车道线 0.14m)
const BRAKE_REAR_OFFSET_M = 2.35;  // 制动光晕相对自车中心后移距离 (m)
const BRAKE_LIFT_M = 0.16;         // 制动地面光晕离地高度 (m)
const HAZARD_HEIGHT_M = 1.62;
const FLASH_INTERVAL_MS = 500;
const LIGHT_HAZARD_MASK = 4;       // bit 2: LIGHT_HAZARD
const WAKE_REAR_OFFSET_M = 2.15;
const WAKE_TRACK_HALF_M = 0.78;
const WAKE_LIFT_M = 0.16;          // 雨天双后轮水雾贴片离地高度 (m)

function _createSoftMistTexture() {
  if (typeof document === 'undefined') return null;
  const canvas = document.createElement('canvas');
  canvas.width = 64;
  canvas.height = 64;
  const ctx = canvas.getContext('2d');
  if (!ctx) return null;
  const grad = ctx.createRadialGradient(32, 32, 2, 32, 32, 30);
  grad.addColorStop(0.0, 'rgba(255, 255, 255, 0.90)');
  grad.addColorStop(0.45, 'rgba(220, 235, 250, 0.45)');
  grad.addColorStop(1.0, 'rgba(180, 210, 240, 0.0)');
  ctx.clearRect(0, 0, 64, 64);
  ctx.fillStyle = grad;
  ctx.beginPath();
  ctx.arc(32, 32, 30, 0, Math.PI * 2);
  ctx.fill();
  return new THREE.CanvasTexture(canvas);
}

/**
 * 构建贴地切入警示翼几何体（局部坐标系：+X 为车头前向，+Z 为切入外侧方向）
 * 包含 1 条车侧警示边条 + 2 枚指向 +Z 切入侧的贴地 V 形导流翼。
 */
function _buildCutinChevronGeometry() {
  const verts = [];
  const pushQuad = (ax, az, bx, bz, cx, cz, dx, dz) => {
    verts.push(
      ax, 0, az, bx, 0, bz, cx, 0, cz,
      ax, 0, az, cx, 0, cz, dx, 0, dz
    );
  };

  // 车侧边缘细警示条
  pushQuad(-1.05, 0.0, 1.05, 0.0, 1.05, 0.08, -1.05, 0.08);

  // 两枚并列指向 +Z（切入侧）的贴地 V 形翼标
  const centers = [-0.44, 0.44];
  for (let i = 0; i < centers.length; i++) {
    const xc = centers[i];
    const z0 = 0.16;
    const zTipOut = 0.54;
    const zTipIn = 0.38;
    // 左翼臂
    pushQuad(xc - 0.34, z0, xc - 0.19, z0, xc, zTipIn, xc, zTipOut);
    // 右翼臂
    pushQuad(xc, zTipOut, xc, zTipIn, xc + 0.19, z0, xc + 0.34, z0);
  }

  const geo = new THREE.BufferGeometry();
  geo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(verts), 3));
  return geo;
}

export function createEffectView(scene) {
  const group = new THREE.Group();
  group.name = 'effectGroup';
  scene.add(group);

  const mistTex = _createSoftMistTexture();
  let brakeGlow = null;
  let hazardFlash = null;
  let _hazardMat = null;

  const obstacleMarkers = new Map();
  const cutinArrows = new Map();

  // 降雨天气双后轮柔和雾化水花贴片（不与 SkyEnv 全局雨丝冲突）
  const wakeGroup = new THREE.Group();
  wakeGroup.visible = false;
  const wakeGeo = new THREE.PlaneGeometry(1.9, 0.65);
  wakeGeo.rotateX(-Math.PI / 2);
  const wakeMat = new THREE.MeshBasicMaterial({
    color: SCENE.sprayMist,
    map: mistTex,
    transparent: true,
    opacity: 0.20,
    depthTest: true,
    depthWrite: false,
    side: THREE.DoubleSide,
    polygonOffset: true,
    polygonOffsetFactor: -3,
    polygonOffsetUnits: -6,
  });
  const wakeLeft = new THREE.Mesh(wakeGeo, wakeMat);
  const wakeRight = new THREE.Mesh(wakeGeo, wakeMat);
  wakeGroup.add(wakeLeft);
  wakeGroup.add(wakeRight);
  group.add(wakeGroup);

  function _clearDynamicPools() {
    if (brakeGlow) {
      group.remove(brakeGlow);
      brakeGlow.geometry?.dispose?.();
      brakeGlow.material?.dispose?.();
      brakeGlow = null;
    }
    if (hazardFlash) {
      group.remove(hazardFlash);
      hazardFlash.geometry?.dispose?.();
      hazardFlash.material?.dispose?.();
      hazardFlash = null;
      _hazardMat = null;
    }
    for (const [, mesh] of obstacleMarkers) {
      group.remove(mesh);
      mesh.geometry?.dispose?.();
      mesh.material?.dispose?.();
    }
    obstacleMarkers.clear();

    for (const [, mesh] of cutinArrows) {
      group.remove(mesh);
      mesh.geometry?.dispose?.();
      mesh.material?.dispose?.();
    }
    cutinArrows.clear();
  }

  function clear() {
    _clearDynamicPools();
    wakeGroup.visible = false;
  }

  function dispose() {
    _clearDynamicPools();
    group.remove(wakeGroup);
    wakeGeo.dispose();
    wakeMat.dispose();
    mistTex?.dispose?.();
    scene.remove(group);
  }

  /** 紧急制动车尾贴地柔和红光晕 */
  function _updateBrakeGlow(ego, store) {
    const isBraking = (ego.brake || 0) > BRAKE_THRESHOLD;
    if (!isBraking) {
      if (brakeGlow) {
        group.remove(brakeGlow);
        brakeGlow.geometry?.dispose?.();
        brakeGlow.material?.dispose?.();
        brakeGlow = null;
      }
      return;
    }

    if (!brakeGlow) {
      const geo = new THREE.PlaneGeometry(1.7, 2.2);
      geo.rotateX(-Math.PI / 2);
      const mat = new THREE.MeshBasicMaterial({
        color: SCENE.brakeGlow,
        map: mistTex,
        transparent: true,
        opacity: 0.35,
        side: THREE.DoubleSide,
        depthTest: true,
        depthWrite: false,
        blending: THREE.AdditiveBlending,
        polygonOffset: true,
        polygonOffsetFactor: -3,
        polygonOffsetUnits: -6,
      });
      brakeGlow = new THREE.Mesh(geo, mat);
      brakeGlow.renderOrder = 19;
      group.add(brakeGlow);
    }

    const heading = ego.heading || 0;
    const [fx, fy] = forwardENU(heading);
    const rearEnuX = ego.x - fx * BRAKE_REAR_OFFSET_M;
    const rearEnuY = ego.y - fy * BRAKE_REAR_OFFSET_M;
    const groundZ = (ego.z != null) ? ego.z : roadHeightAt(store, rearEnuX, rearEnuY);
    const [bx, by, bz] = worldToThree(rearEnuX, rearEnuY, groundZ + BRAKE_LIFT_M);

    brakeGlow.position.set(bx, by, bz);
    brakeGlow.rotation.y = headingToRotationY(heading);

    const intensity = Math.min(1.0, (ego.brake || 0) / 1.5);
    brakeGlow.material.opacity = 0.18 + intensity * 0.34;
    const s = 0.75 + intensity * 0.45;
    brakeGlow.scale.set(s, 1.0, s);
  }

  /** 紧急双闪光效 */
  function _updateHazardFlash(ego, store, now) {
    const isHazard = !!((ego.lights || 0) & LIGHT_HAZARD_MASK);
    if (!isHazard) {
      if (hazardFlash) {
        group.remove(hazardFlash);
        hazardFlash.geometry?.dispose?.();
        hazardFlash.material?.dispose?.();
        hazardFlash = null;
        _hazardMat = null;
      }
      return;
    }

    const flashOn = Math.floor(now / FLASH_INTERVAL_MS) % 2 === 0;
    if (!hazardFlash) {
      const geo = new THREE.SphereGeometry(0.22, 10, 10);
      _hazardMat = new THREE.MeshBasicMaterial({
        color: SCENE.hazardOrange,
        transparent: true,
        opacity: 0.75,
        depthTest: true,
        depthWrite: false,
      });
      hazardFlash = new THREE.Mesh(geo, _hazardMat);
      group.add(hazardFlash);
    }

    const groundZ = (ego.z != null) ? ego.z : roadHeightAt(store, ego.x, ego.y);
    const [tx, ty, tz] = worldToThree(ego.x, ego.y, groundZ + HAZARD_HEIGHT_M);
    hazardFlash.position.set(tx, ty, tz);
    const hs = flashOn ? 1.0 : 0.35;
    hazardFlash.scale.set(hs, hs, hs);
    if (_hazardMat) {
      _hazardMat.color.setHex(flashOn ? SCENE.hazardOrange : SCENE.hazardYellow);
      _hazardMat.opacity = flashOn ? 0.72 : 0.18;
    }
  }

  /** 旁车切入贴地双 V 警示翼（依据车体航向与法线贴合切入侧边缘） */
  function _updateCutinWarning(store, now) {
    if (!store.ego || !store.entities) return;

    const ego = store.ego;
    const [efx, efy] = forwardENU(ego.heading || 0);
    const [egoFwdX, , egoFwdZ] = worldToThree(efx, efy, 0);
    const [egoNormX, egoNormZ] = tangentToNormal(egoFwdX, egoFwdZ);

    // 平滑三角脉冲（零裸 sin/cos）
    const phase = ((now || 0) % 700) / 700;
    const tri = 1.0 - Math.abs(2.0 * phase - 1.0);
    const smoothPulse = tri * tri * (3.0 - 2.0 * tri);

    const activeIds = new Set();
    for (let i = 0; i < store.entities.length; i++) {
      const ent = store.entities[i];
      if (!ent || ent.type === 'ego' || ent.type === 'tl' || ent.parked) continue;

      const speed = Number(ent.speed) || 0;
      if (speed < 0.5) continue;

      const dist = distanceENU(ego.x, ego.y, ent.x || 0, ent.y || 0);
      if (dist > CUTIN_MAX_DIST_M) continue;

      const entHeading = ent.heading || 0;
      const [nfx, nfy] = forwardENU(entHeading);
      const hasVelVec = Number.isFinite(ent.vx) && Number.isFinite(ent.vy) &&
                        (Math.abs(ent.vx) > 1e-3 || Math.abs(ent.vy) > 1e-3);
      const vx = hasVelVec ? ent.vx : nfx * speed;
      const vy = hasVelVec ? ent.vy : nfy * speed;

      const [tvx, , tvz] = worldToThree(vx, vy, 0);
      const lateralVel = tvx * egoNormX + tvz * egoNormZ;
      if (Math.abs(lateralVel) < CUTIN_LATERAL_SPEED) continue;

      activeIds.add(ent.id);

      let chevron = cutinArrows.get(ent.id);
      if (!chevron) {
        const geo = _buildCutinChevronGeometry();
        const mat = new THREE.MeshBasicMaterial({
          color: SCENE.cutinChevron,
          transparent: true,
          opacity: 0.72,
          side: THREE.DoubleSide,
          depthTest: true,
          depthWrite: false,
          polygonOffset: true,
          polygonOffsetFactor: -3,
          polygonOffsetUnits: -6,
        });
        chevron = new THREE.Mesh(geo, mat);
        chevron.renderOrder = 23;
        group.add(chevron);
        cutinArrows.set(ent.id, chevron);
      }

      const [fwdX, , fwdZ] = worldToThree(nfx, nfy, 0);
      const [normX, normZ] = tangentToNormal(fwdX, fwdZ);
      const cutSignEgo = lateralVel >= 0 ? 1 : -1;
      const cutDirX = cutSignEgo * egoNormX;
      const cutDirZ = cutSignEgo * egoNormZ;
      const sideSign = (cutDirX * normX + cutDirZ * normZ) >= 0 ? 1 : -1;

      const entGroundZ = (ent.z != null) ? ent.z : roadHeightAt(store, ent.x || 0, ent.y || 0);
      const [cx, cy, cz] = worldToThree(ent.x || 0, ent.y || 0, entGroundZ + CUTIN_LIFT_M);
      const halfW = (ent.width || 2.0) * 0.5;
      const [px, , pz] = offsetAlongNormal(cx, cz, normX, normZ, sideSign * (halfW + 0.14));

      chevron.position.set(px, cy, pz);
      chevron.rotation.y = headingToRotationY(entHeading);
      chevron.scale.set(1.0, 1.0, sideSign);
      chevron.material.opacity = 0.42 + 0.44 * smoothPulse;
    }

    for (const [id, mesh] of cutinArrows) {
      if (!activeIds.has(id)) {
        group.remove(mesh);
        mesh.geometry?.dispose?.();
        mesh.material?.dispose?.();
        cutinArrows.delete(id);
      }
    }
  }

  /** 雨天双后轮柔和雾化尾迹（仅在 rain/storm 且车速 > 2 m/s 时显示） */
  function _updateWeatherWake(ego, store) {
    const weather = store.env && store.env.weather;
    const isRain = (weather === 'rain' || weather === 'storm');
    const speed = Math.abs(ego.speed || 0);

    if (!isRain || speed <= 2.0) {
      wakeGroup.visible = false;
      return;
    }

    wakeGroup.visible = true;
    const heading = ego.heading || 0;
    const [fx, fy] = forwardENU(heading);
    const rearEnuX = ego.x - fx * WAKE_REAR_OFFSET_M;
    const rearEnuY = ego.y - fy * WAKE_REAR_OFFSET_M;
    const groundZ = (ego.z != null) ? ego.z : roadHeightAt(store, rearEnuX, rearEnuY);
    const [rx, ry, rz] = worldToThree(rearEnuX, rearEnuY, groundZ + WAKE_LIFT_M);

    const [fwdX, , fwdZ] = worldToThree(fx, fy, 0);
    const [normX, normZ] = tangentToNormal(fwdX, fwdZ);
    const [lx, , lz] = offsetAlongNormal(rx, rz, normX, normZ, -WAKE_TRACK_HALF_M);
    const [rtx, , rtz] = offsetAlongNormal(rx, rz, normX, normZ, WAKE_TRACK_HALF_M);

    const rotY = headingToRotationY(heading);
    const lenScale = Math.min(1.8, 0.7 + speed * 0.045);

    wakeLeft.position.set(lx, ry, lz);
    wakeLeft.rotation.y = rotY;
    wakeLeft.scale.set(lenScale, 1.0, 1.0);

    wakeRight.position.set(rtx, ry, rtz);
    wakeRight.rotation.y = rotY;
    wakeRight.scale.set(lenScale, 1.0, 1.0);

    wakeMat.opacity = Math.min(0.28, 0.05 + speed * 0.012);
  }

  function update(store, now = 0) {
    if (!store || !store.ego) return;
    _updateBrakeGlow(store.ego, store);
    _updateHazardFlash(store.ego, store, now);
    _updateCutinWarning(store, now);
    _updateWeatherWake(store.ego, store);
  }

  return { update, clear, dispose };
}
