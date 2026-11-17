/**
 * TrafficLightView.js — 交通信号灯
 *
 * 渲染逻辑唯一来源：scene/frame 的 entities 数组里 type='tl' 的实体。
 * 不再从 road_network.edges[*].traffic_lights 二次构造，避免"同个红绿灯
 * 多处实现"导致重复或错位渲染。
 *
 * 坐标/朝向约定：
 *   - 后端 heading 表示灯杆臂的指向（垂直于道路切线，指向道路中心）。
 *   - 前端本地模型：臂默认沿 -z，灯壳/灯泡默认朝 +x（向来车方向）。
 *   - 组旋转：headingToRotationY(heading - PI/2)，使臂与后端 heading 对齐。
 */

import { getStdMaterial, createEmissiveMaterial } from '../core/AssetFactory.js';
import { worldToThree, headingToRotationY } from '../math/Coord.js';
import { SCENE } from '../theme/tokens.js';
import { roadHeightAt } from '../math/RoadHeight.js';
import {
  makeSignTexture,
  makeSignalLensTexture,
  makeSignalCountdownTexture,
} from '../utils/CanvasTextureFactory.js';

const RED = 0xff2828, YELLOW = 0xffaa00, GREEN = 0x00ee66;
const COLOR_KEYS = ['red', 'yellow', 'green'];
const COLOR_HEX = [RED, YELLOW, GREEN];
const LAMP_Y = [5.04, 4.64, 4.24];  // GB 14886 红/黄/绿竖排中心高度 (净空与视距兼顾)
const HEAD_CENTER_Y = 4.64;
const HEAD_H = 1.28;
const ARM_Y = 5.56;
const POLE_H = 5.82;
const MAIN_Z = -4.25;
const LEFT_Z = -4.98;

function _stateIndex(state) {
  return typeof state === 'number' ? state
    : state === 'red' ? 0
    : state === 'yellow' ? 1
    : state === 'green' || state === 'flashing_green' ? 2
    : -1;
}

function _applySignalMaterials(materials, lensKind, state) {
  const idx = _stateIndex(state);
  const flashingOff = state === 'flashing_green' &&
    Math.floor(performance.now() / 250) % 2 === 0;
  materials.forEach((mat, i) => {
    const lit = (i === idx && !flashingOff);
    mat.emissiveIntensity = lit ? 2.0 : 0.05;
    if (mat.userData && mat.userData.lensKind) {
      const targetTex = lit ? mat.userData.activeTex : mat.userData.dimTex;
      if (targetTex && mat.map !== targetTex) {
        mat.map = targetTex;
        mat.emissiveMap = targetTex;
        mat.needsUpdate = true;
      }
    }
  });
}

function _addThreeSideVisor(parent, y, zCenter, housingMat) {
  // GB 14886 深遮阳罩（顶檐 + 左右侧翼，朝 -x 来车方向伸出 0.24m）
  const top = new THREE.Mesh(new THREE.BoxGeometry(0.24, 0.026, 0.34), housingMat);
  top.position.set(-0.28, y + 0.165, zCenter);
  const leftW = new THREE.Mesh(new THREE.BoxGeometry(0.20, 0.28, 0.022), housingMat);
  leftW.position.set(-0.26, y + 0.02, zCenter - 0.165);
  const rightW = new THREE.Mesh(new THREE.BoxGeometry(0.20, 0.28, 0.022), housingMat);
  rightW.position.set(-0.26, y + 0.02, zCenter + 0.165);
  parent.add(top, leftW, rightW);
}

function _buildBackplateAssembly(width, height, zCenter, backplateMat, borderMat) {
  const bpGroup = new THREE.Group();
  const trimT = 0.048;
  const plate = new THREE.Mesh(new THREE.BoxGeometry(0.05, height, width), backplateMat);
  plate.position.set(0.11, HEAD_CENTER_Y, zCenter);
  bpGroup.add(plate);

  // GB 14886 四周连续高反光白色外边框（上下横条 + 左右竖条）
  const topTrim = new THREE.Mesh(new THREE.BoxGeometry(0.06, trimT, width + trimT), borderMat);
  topTrim.position.set(0.105, HEAD_CENTER_Y + height * 0.5, zCenter);
  const botTrim = new THREE.Mesh(new THREE.BoxGeometry(0.06, trimT, width + trimT), borderMat);
  botTrim.position.set(0.105, HEAD_CENTER_Y - height * 0.5, zCenter);
  const leftTrim = new THREE.Mesh(new THREE.BoxGeometry(0.06, height + trimT, trimT), borderMat);
  leftTrim.position.set(0.105, HEAD_CENTER_Y, zCenter - width * 0.5);
  const rightTrim = new THREE.Mesh(new THREE.BoxGeometry(0.06, height + trimT, trimT), borderMat);
  rightTrim.position.set(0.105, HEAD_CENTER_Y, zCenter + width * 0.5);
  bpGroup.add(topTrim, botTrim, leftTrim, rightTrim);
  return bpGroup;
}

export function createTrafficLightView(scene) {
  // entity id → { group, lamps, singleBackplate, leftGroup, leftMaterials, countdownMesh, cdKey }
  const pool = new Map();

  function _createOpticalLens(parent, y, colorIdx, zCenter, lensKind) {
    const colorHex = COLOR_HEX[colorIdx];
    const colorKey = COLOR_KEYS[colorIdx];
    const activeTex = makeSignalLensTexture(lensKind, colorKey, true);
    const dimTex = makeSignalLensTexture(lensKind, colorKey, false);

    const mat = createEmissiveMaterial(colorHex, 0.05);
    mat.userData = { lensKind, activeTex, dimTex };
    if (dimTex) {
      mat.color.setHex(0xffffff);
      mat.map = dimTex;
      mat.emissiveMap = dimTex;
    }

    if (activeTex) {
      const lens = new THREE.Mesh(new THREE.PlaneGeometry(0.32, 0.32), mat);
      lens.position.set(-0.165, y, zCenter);
      lens.rotation.y = -Math.PI / 2;
      parent.add(lens);
      return { mesh: lens, mat };
    }

    // 无 DOM 的 Node 单元测试环境回退：保持真实 Three 几何网格与发光材质语义
    const backingMat = getStdMaterial(SCENE.tlHousing, 0.8, 0.2);
    const bezel = new THREE.Mesh(new THREE.CylinderGeometry(0.145, 0.145, 0.03, 16), backingMat);
    bezel.rotation.z = Math.PI / 2;
    bezel.position.set(-0.16, y, zCenter);
    parent.add(bezel);

    if (lensKind === 'left') {
      const shaft = new THREE.Mesh(new THREE.BoxGeometry(0.03, 0.042, 0.16), mat);
      shaft.position.set(-0.175, y, zCenter);
      parent.add(shaft);
      for (const sign of [-1, 1]) {
        const barb = new THREE.Mesh(new THREE.BoxGeometry(0.03, 0.036, 0.095), mat);
        barb.position.set(-0.175, y + sign * 0.032, zCenter - 0.04);
        barb.rotation.x = sign * (Math.PI / 4);
        parent.add(barb);
      }
      return { mesh: shaft, mat };
    }

    const lamp = new THREE.Mesh(new THREE.SphereGeometry(0.115, 16, 12), mat);
    lamp.position.set(-0.165, y, zCenter);
    parent.add(lamp);
    return { mesh: lamp, mat };
  }

  function _createTrafficLight() {
    const g = new THREE.Group();

    const poleMat = getStdMaterial(0x5a626d, 0.42, 0.58);
    const housingMat = getStdMaterial(SCENE.tlHousing, 0.72, 0.28);
    const backplateMat = getStdMaterial(SCENE.tlBackplate, 0.88, 0.08);
    const borderMat = getStdMaterial(SCENE.tlBackplateBorder, 0.40, 0.20);

    // 悬臂立柱与底座法兰
    const baseFlange = new THREE.Mesh(new THREE.CylinderGeometry(0.26, 0.28, 0.16, 12), poleMat);
    baseFlange.position.y = 0.08;
    const pole = new THREE.Mesh(new THREE.CylinderGeometry(0.125, 0.165, POLE_H, 12), poleMat);
    pole.position.y = POLE_H * 0.5;
    pole.castShadow = true;
    g.add(baseFlange, pole);

    // 基础悬臂横梁（覆盖主灯柱及右缘倒计时器）
    const baseArmLen = 4.90;
    const arm = new THREE.Mesh(new THREE.BoxGeometry(0.14, 0.14, baseArmLen), poleMat);
    arm.position.set(0, ARM_Y, -baseArmLen * 0.5);
    g.add(arm);

    // 主信号灯垂吊立管与壳体（GB 14886 满盘机动车信号灯）
    const mainDrop = new THREE.Mesh(new THREE.BoxGeometry(0.09, 0.34, 0.09), housingMat);
    mainDrop.position.set(0, ARM_Y - 0.17, MAIN_Z);
    const housing = new THREE.Mesh(new THREE.BoxGeometry(0.32, HEAD_H, 0.44), housingMat);
    housing.position.set(0, HEAD_CENTER_Y, MAIN_Z);
    housing.castShadow = true;
    g.add(mainDrop, housing);

    const singleBackplate = _buildBackplateAssembly(0.68, 1.50, MAIN_Z, backplateMat, borderMat);
    g.add(singleBackplate);

    const lamps = [];
    for (let i = 0; i < 3; i++) {
      _addThreeSideVisor(g, LAMP_Y[i], MAIN_Z, housingMat);
      const { mesh } = _createOpticalLens(g, LAMP_Y[i], i, MAIN_Z, 'circle');
      lamps.push(mesh);
    }

    // ── GB 14886-2016 常规组合 2：左转方向指示灯 + 满盘主灯 一体化双拼龙门组 ──
    const leftGroup = new THREE.Group();
    const leftArmExt = new THREE.Mesh(new THREE.BoxGeometry(0.14, 0.14, 1.15), poleMat);
    leftArmExt.position.set(0, ARM_Y, -5.32);
    const leftDrop = new THREE.Mesh(new THREE.BoxGeometry(0.09, 0.34, 0.09), housingMat);
    leftDrop.position.set(0, ARM_Y - 0.17, LEFT_Z);
    leftGroup.add(leftArmExt, leftDrop);

    const leftHousing = new THREE.Mesh(new THREE.BoxGeometry(0.32, HEAD_H, 0.44), housingMat);
    leftHousing.position.set(0, HEAD_CENTER_Y, LEFT_Z);
    leftHousing.castShadow = true;
    leftGroup.add(leftHousing);

    // 一体化双列黑底白边防眩背板（将 ← 左转列与 ● 满盘主灯列整框合拢）
    const dualCenterZ = (MAIN_Z + LEFT_Z) * 0.5;
    const dualBackplate = _buildBackplateAssembly(1.44, 1.50, dualCenterZ, backplateMat, borderMat);
    leftGroup.add(dualBackplate);

    // 背板顶部国标辅标（“← 左转 | 直行 ●”）
    if (typeof document !== 'undefined') {
      const capTex = makeSignTexture('← 左转  |  机动车主灯 ●', '#0f1620', '#ffffff', '#e2e8f0', {
        width: 320, height: 64, fontSize: 24, borderWidth: 3,
      });
      const capBoard = new THREE.Mesh(
        new THREE.PlaneGeometry(1.18, 0.24),
        new THREE.MeshBasicMaterial({ map: capTex })
      );
      capBoard.position.set(-0.08, HEAD_CENTER_Y + 0.88, dualCenterZ);
      capBoard.rotation.y = -Math.PI / 2;
      leftGroup.add(capBoard);
    }

    const leftMaterials = [];
    for (let i = 0; i < 3; i++) {
      _addThreeSideVisor(leftGroup, LAMP_Y[i], LEFT_Z, housingMat);
      const { mat } = _createOpticalLens(leftGroup, LAMP_Y[i], i, LEFT_Z, 'left');
      leftMaterials.push(mat);
    }

    leftGroup.visible = false;
    g.add(leftGroup);

    // GA/T 508 数显倒计时屏（挂于主灯右侧横臂下）
    let countdownMesh = null;
    if (typeof document !== 'undefined') {
      const cdMat = new THREE.MeshBasicMaterial({ color: 0xffffff });
      countdownMesh = new THREE.Mesh(new THREE.PlaneGeometry(0.46, 0.56), cdMat);
      countdownMesh.position.set(-0.04, HEAD_CENTER_Y + 0.08, MAIN_Z + 0.68);
      countdownMesh.rotation.y = -Math.PI / 2;
      countdownMesh.visible = false;
      g.add(countdownMesh);
    }

    g.userData.lamps = lamps;
    g.userData.lampColors = COLOR_HEX;
    g.userData.leftGroup = leftGroup;
    g.userData.leftMaterials = leftMaterials;
    scene.add(g);
    return {
      group: g,
      lamps,
      singleBackplate,
      leftGroup,
      leftMaterials,
      countdownMesh,
      cdKey: '',
    };
  }

  /** 切换灯状态（含独立左转箭头头、统一背板及倒计时牌）。 */
  function _setLight(entry, ent) {
    const state = ent.state;
    const leftState = ent.left_state;
    const hasLeft = Boolean(ent.has_left_arrow || ent.left_state != null);

    _applySignalMaterials(entry.lamps.map(l => l.material), 'circle', state);
    entry.leftGroup.visible = hasLeft;
    entry.singleBackplate.visible = !hasLeft;
    if (hasLeft) {
      const lState = leftState != null ? leftState : state;
      _applySignalMaterials(entry.leftMaterials, 'left', lState);
    }

    if (entry.countdownMesh) {
      const hasMainRem = typeof ent.remain_s === 'number' && Number.isFinite(ent.remain_s);
      const hasLeftRem = hasLeft && typeof ent.left_remain_s === 'number' && Number.isFinite(ent.left_remain_s);
      if (!hasMainRem && !hasLeftRem) {
        entry.countdownMesh.visible = false;
      } else {
        const rows = [];
        if (hasLeft) {
          const lSec = hasLeftRem ? Math.ceil(Math.max(0, ent.left_remain_s)) : -1;
          const lCol = String(leftState != null ? leftState : state || 'red').toLowerCase();
          rows.push({ prefix: '←', sec: lSec, color: lCol === 'flashing_green' ? 'green' : lCol });
        }
        const mSec = hasMainRem ? Math.ceil(Math.max(0, ent.remain_s)) : -1;
        const mCol = String(state || 'red').toLowerCase();
        rows.push({ prefix: hasLeft ? '●' : '', sec: mSec, color: mCol === 'flashing_green' ? 'green' : mCol });
        const key = `${hasLeft ? 1 : 0}|` + rows.map(r => `${r.prefix}:${r.sec}:${r.color}`).join(';');
        if (key !== entry.cdKey) {
          /* Countdown textures change every second; do not use the global
           * cache (it would keep up to 300 stale GPU textures) and dispose the
           * previous map once it is no longer referenced by another light. */
          const oldMap = entry.countdownMesh.material.map;
          const tex = makeSignalCountdownTexture(rows, false);
          if (tex) {
            entry.countdownMesh.material.map = tex;
            entry.countdownMesh.material.needsUpdate = true;
            entry.countdownMesh.scale.y = rows.length > 1 ? 1.15 : 0.72;
            if (oldMap && oldMap !== tex) {
              let shared = false;
              for (const [, other] of pool) {
                if (other !== entry && other.countdownMesh &&
                    other.countdownMesh.material.map === oldMap) {
                  shared = true;
                  break;
                }
              }
              if (!shared) oldMap.dispose();
            }
          }
          entry.cdKey = key;
        }
        entry.countdownMesh.visible = true;
      }
    }
  }

  /** 主更新入口：只认 scene/frame entities 里的 type='tl' */
  function update(store) {
    const all = (store.entities || []).filter(e => e && e.type === 'tl');

    // 删除消失的
    const aliveIds = new Set(all.map(e => e.id));
    for (const [id, entry] of pool.entries()) {
      if (!aliveIds.has(id)) {
        const map = entry.countdownMesh && entry.countdownMesh.material &&
          entry.countdownMesh.material.map;
        scene.remove(entry.group);
        pool.delete(id);
        if (map) {
          let shared = false;
          for (const [, other] of pool) {
            if (other.countdownMesh && other.countdownMesh.material.map === map) {
              shared = true;
              break;
            }
          }
          if (!shared) map.dispose();
        }
      }
    }

    // 创建/更新
    for (const ent of all) {
      let entry = pool.get(ent.id);
      if (!entry) {
        entry = _createTrafficLight();
        pool.set(ent.id, entry);
      }
      const z = roadHeightAt(store, ent.x, ent.y);
      entry.group.position.set(...worldToThree(ent.x, ent.y, z));
      // 后端 heading 是臂指向；本地臂默认沿 -z。
      // 用 headingToRotationY(h - PI/2) = PI/2 - h 对齐。
      entry.group.rotation.y = headingToRotationY((ent.heading || 0) - Math.PI / 2);
      _setLight(entry, ent);
    }
  }

  function clear() {
    for (const [, entry] of pool) {
      const map = entry.countdownMesh && entry.countdownMesh.material &&
        entry.countdownMesh.material.map;
      if (map) map.dispose();
      scene.remove(entry.group);
    }
    pool.clear();
  }

  return { update, clear };
}
