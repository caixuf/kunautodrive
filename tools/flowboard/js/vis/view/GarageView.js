/**
 * GarageView.js — B1 地下车库剖切结构体 + 敞开式出库坡道挡墙 + 地面寻位停车场沥青广场
 *
 * 严格遵循 FlowBoard 规范：
 *   - 全量配色来自 theme/tokens.js (SCENE.*)
 *   - ENU → THREE 映射仅通过 Coord.js
 *   - 材质 metalness <= 0.50
 */

import { getStdMaterial, createEmissiveMaterial } from '../core/AssetFactory.js';
import { EDGE_TYPE } from '../core/Constants.js';
import {
  worldToThree,
  headingBetweenPoints,
  headingToRotationY,
  tangentToNormal,
  forwardENU,
} from '../math/Coord.js';
import { SCENE } from '../theme/tokens.js';
import { makeSignTexture, makeStripeTexture } from '../utils/CanvasTextureFactory.js';

function _toCss(hexNum) {
  return '#' + (hexNum >>> 0).toString(16).padStart(6, '0');
}

function _safeSignMaterial(text, bgNum, fgNum, borderNum, opts = {}) {
  if (typeof document !== 'undefined') {
    try {
      const map = makeSignTexture(text, _toCss(bgNum), _toCss(fgNum), _toCss(borderNum), opts);
      return new THREE.MeshStandardMaterial({
        map,
        emissiveMap: map,
        emissive: new THREE.Color(fgNum),
        emissiveIntensity: opts.emissiveIntensity ?? 0.35,
        roughness: opts.roughness ?? 0.4,
        metalness: opts.metalness ?? 0.08,
        polygonOffset: Boolean(opts.polygonOffset),
        polygonOffsetFactor: opts.polygonOffset ? -1 : 0,
        polygonOffsetUnits: opts.polygonOffset ? -2 : 0,
      });
    } catch (_e) {
      // Fallback for headless environments without full 2D canvas
    }
  }
  return createEmissiveMaterial(bgNum, opts.emissiveIntensity ?? 0.55);
}

function _safeStripeMaterial(c1Num, c2Num, opts = {}) {
  if (typeof document !== 'undefined') {
    try {
      const map = makeStripeTexture(_toCss(c1Num), _toCss(c2Num), opts);
      return new THREE.MeshStandardMaterial({
        map,
        roughness: 0.65,
        metalness: 0.05,
      });
    } catch (_e) {
      // Fallback
    }
  }
  return createEmissiveMaterial(c1Num, 0.18);
}

function _parseNodes(edge) {
  const raw = Array.isArray(edge && edge.nodes) ? edge.nodes : [];
  return raw.map((n) => (Array.isArray(n)
    ? [Number(n[0]) || 0, Number(n[1]) || 0, Number(n[2]) || 0]
    : [Number(n && n.x) || 0, Number(n && n.y) || 0, Number(n && n.z) || 0]));
}

function _buildSlopedWallGeo(nodes, sideOffset, topY, bottomPad) {
  if (!nodes || nodes.length < 2) return null;
  const positions = [];
  const indices = [];

  for (let i = 0; i < nodes.length; i++) {
    const cur = nodes[i];
    const prev = nodes[Math.max(0, i - 1)];
    const next = nodes[Math.min(nodes.length - 1, i + 1)];
    const tx = next[0] - prev[0];
    const ty = next[1] - prev[1];
    const [cx, cy, cz] = worldToThree(cur[0], cur[1], cur[2]);
    const [nx, nz] = tangentToNormal(tx, -ty);
    const wx = cx + nx * sideOffset;
    const wz = cz + nz * sideOffset;
    const yBot = Math.min(cy - bottomPad, topY - 0.12);
    positions.push(wx, yBot, wz, wx, topY, wz);
  }

  for (let i = 0; i < nodes.length - 1; i++) {
    const a = i * 2;
    const b = i * 2 + 1;
    const c = (i + 1) * 2;
    const d = (i + 1) * 2 + 1;
    indices.push(a, c, b, b, c, d);
  }

  const geo = new THREE.BufferGeometry();
  geo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
  geo.setIndex(indices);
  geo.computeVertexNormals();
  return geo;
}

/** 创建标准化垂直车位 U 型标线 + 黄黑条纹止轮器（2.5m × 5.3m）+ 车位编号喷涂 */
function _createStandardBayGroup(mats, w = 2.5, l = 5.3, isEv = false, opts = {}) {
  const g = new THREE.Group();
  const lineW = opts.lineWidth ?? 0.11;
  const stopW = opts.stopW ?? w * 0.72;
  const stopH = opts.stopH ?? 0.11;

  if (isEv) {
    const evPad = new THREE.Mesh(new THREE.PlaneGeometry(l - 0.18, w - 0.18), mats.evPad);
    evPad.rotation.x = -Math.PI / 2;
    evPad.position.set(0, -0.002, 0);
    g.add(evPad);
  }
  const sideGeo = new THREE.BoxGeometry(l, 0.012, lineW);
  const rearGeo = new THREE.BoxGeometry(lineW, 0.012, w + lineW);
  for (const s of [-1, 1]) {
    const line = new THREE.Mesh(sideGeo, mats.line);
    line.position.set(0, 0, s * w * 0.5);
    g.add(line);
  }
  const rear = new THREE.Mesh(rearGeo, mats.line);
  rear.position.set(-l * 0.5, 0, 0);
  g.add(rear);

  if (opts.label && mats.labelBgNum !== undefined) {
    const labelMat = _safeSignMaterial(opts.label, mats.labelBgNum, SCENE.lineWhite, mats.labelBgNum, {
      width: 192,
      height: 96,
      fontSize: 44,
      emissiveIntensity: 0.14,
      roughness: 0.72,
      metalness: 0.02,
      polygonOffset: true,
    });
    const labelGeo = opts.labelGeo || new THREE.PlaneGeometry(1.05, 0.52);
    const labelMesh = new THREE.Mesh(labelGeo, labelMat);
    labelMesh.rotation.order = 'YXZ';
    labelMesh.rotation.x = -Math.PI / 2;
    labelMesh.rotation.z = -Math.PI / 2;
    labelMesh.position.set(l * 0.5 - 0.48, 0.005, 0);
    g.add(labelMesh);
  }

  const stopX = -l * 0.5 + 0.65;
  const stop = new THREE.Mesh(new THREE.BoxGeometry(0.18, stopH, stopW), mats.stopper);
  stop.position.set(stopX, stopH * 0.5, 0);
  g.add(stop);
  for (const s of [-0.32, 0, 0.32]) {
    const st = new THREE.Mesh(new THREE.BoxGeometry(0.186, stopH + 0.005, 0.18), mats.stripe);
    st.position.set(stopX, stopH * 0.5, s * stopW);
    g.add(st);
  }
  return g;
}

/** 创建路面直行导向箭头（沿本地 +X 方向） */
function _createLaneArrow(mat, scale = 1.0) {
  const g = new THREE.Group();
  const shaft = new THREE.Mesh(new THREE.BoxGeometry(1.7 * scale, 0.012, 0.24 * scale), mat);
  shaft.position.set(-0.35 * scale, 0, 0);
  g.add(shaft);

  const headShape = new THREE.Shape();
  headShape.moveTo(0.85 * scale, 0);
  headShape.lineTo(0.25 * scale, 0.45 * scale);
  headShape.lineTo(0.25 * scale, -0.45 * scale);
  headShape.closePath();
  const headGeo = new THREE.ShapeGeometry(headShape);
  headGeo.rotateX(-Math.PI / 2);
  const head = new THREE.Mesh(headGeo, mat);
  head.position.set(0.15 * scale, 0.006, 0);
  g.add(head);
  return g;
}

export function createGarageView(scene) {
  const group = new THREE.Group();
  group.name = 'garageCutaway';
  scene.add(group);

  let builtSig = '';
  let lastSlotsSig = '';
  let lotGroup = null;

  function _disposeChildren(target) {
    if (!target) return;
    while (target.children.length) {
      const c = target.children[0];
      target.remove(c);
      if (c.children && c.children.length) _disposeChildren(c);
      if (c.geometry) c.geometry.dispose();
      if (c.material) {
        if (Array.isArray(c.material)) c.material.forEach((m) => m && m.dispose && m.dispose());
        else if (c.material.dispose) c.material.dispose();
      }
    }
  }

  function clear() {
    _disposeChildren(group);
    lotGroup = null;
    builtSig = '';
    lastSlotsSig = '';
  }

  /** 构建地面停车场入口安全岛、多车位沥青广场、树池、充电桩与人行通道 */
  function _buildSurfaceLotPad(slots) {
    if (lotGroup) {
      group.remove(lotGroup);
      _disposeChildren(lotGroup);
      lotGroup = null;
    }
    const surfaceSlots = (Array.isArray(slots) ? slots : [])
      .filter((s) => s && Math.abs(Number(s.z) || 0) < 1.2);
    if (!surfaceSlots.length) return;

    let minX = Infinity, maxX = -Infinity, minY = Infinity;
    for (const s of surfaceSlots) {
      const sx = Number(s.x) || 0;
      const sy = Number(s.y) || 0;
      if (sx < minX) minX = sx;
      if (sx > maxX) maxX = sx;
      if (sy < minY) minY = sy;
    }

    const bayY = Number(surfaceSlots[0].y) || -6.5;
    const bayH = Number(surfaceSlots[0].heading) || 1.5708;

    // 至少覆盖 5 个标准车位（3213.5 .. 3229.5）及东西两侧树池隔离岛
    const bayXs = [minX - 4.0, minX, (minX + maxX) * 0.5, maxX, maxX + 4.0];
    const uniqueBayXs = [];
    for (const bx of bayXs) {
      if (!uniqueBayXs.some((u) => Math.abs(u - bx) < 1.2)) uniqueBayXs.push(bx);
    }
    for (const s of surfaceSlots) {
      const sx = Number(s.x) || 0;
      if (!uniqueBayXs.some((u) => Math.abs(u - sx) < 1.2)) uniqueBayXs.push(sx);
    }
    uniqueBayXs.sort((a, b) => a - b);

    const westBayX = uniqueBayXs[0];
    const eastBayX = uniqueBayXs[uniqueBayXs.length - 1];
    const westPlanterX = westBayX - 5.65;
    const eastPlanterX = eastBayX + 5.85;
    const padMinX = westPlanterX - 3.6;
    const padMaxX = eastPlanterX + 3.6;
    const bayMinY = Math.min(-11.4, minY - 4.9);
    const bayMaxY = -3.35;
    const cx = (padMinX + padMaxX) * 0.5;
    const cy = (bayMinY + bayMaxY) * 0.5;
    const sizeX = padMaxX - padMinX;
    const sizeY = bayMaxY - bayMinY;

    lotGroup = new THREE.Group();
    lotGroup.name = 'surfaceParkingLotPad';

    // 1. 沥青广场垫层 (top = 0.095 + 0.06 = 0.155 > Y_CURB = 0.15，避免与 GroundView Z-fighting)
    const padMat = new THREE.MeshStandardMaterial({
      color: SCENE.lotPad,
      roughness: 0.9,
      metalness: 0.02,
      polygonOffset: true,
      polygonOffsetFactor: -1,
      polygonOffsetUnits: -2,
    });
    const padMesh = new THREE.Mesh(new THREE.BoxGeometry(sizeX, 0.12, sizeY), padMat);
    const [tx, ty, tz] = worldToThree(cx, cy, 0.095);
    padMesh.position.set(tx, ty, tz);
    padMesh.receiveShadow = true;
    lotGroup.add(padMesh);

    // 2. 周边混凝土路缘石
    const curbMat = getStdMaterial(SCENE.rampCoping, 0.78, 0.05);
    const backCurb = new THREE.Mesh(new THREE.BoxGeometry(sizeX, 0.22, 0.28), curbMat);
    const [bcx, bcy, bcz] = worldToThree(cx, bayMinY + 0.14, 0.11);
    backCurb.position.set(bcx, bcy, bcz);
    lotGroup.add(backCurb);

    for (const sideX of [padMinX + 0.14, padMaxX - 0.14]) {
      const sideCurb = new THREE.Mesh(new THREE.BoxGeometry(0.28, 0.22, sizeY), curbMat);
      const [scx, scy, scz] = worldToThree(sideX, cy, 0.11);
      sideCurb.position.set(scx, scy, scz);
      lotGroup.add(sideCurb);
    }

    // 3. 东西两端树池与灌木隔离带（x: 3205.5..3210.2 与 3233.2..3237.8，与最外侧车位留足安全净距）
    const soilMat = getStdMaterial(SCENE.soilBed, 0.95, 0.0);
    const foliageMat = getStdMaterial(SCENE.verge, 0.82, 0.02);
    const trunkMat = getStdMaterial(SCENE.soilBed, 0.88, 0.02);
    const poleMat = getStdMaterial(SCENE.poleSteel, 0.45, 0.42);
    const luminaireMat = createEmissiveMaterial(SCENE.garageLedStrip, 0.85);

    const planterCenters = [westPlanterX, eastPlanterX];
    for (const px of planterCenters) {
      const island = new THREE.Group();
      const [ix, iy, iz] = worldToThree(px, -6.5, 0.16);
      island.position.set(ix, iy, iz);

      const border = new THREE.Mesh(new THREE.BoxGeometry(4.6, 0.24, 5.1), curbMat);
      border.position.set(0, 0.12, 0);
      island.add(border);

      const soil = new THREE.Mesh(new THREE.BoxGeometry(4.2, 0.26, 4.7), soilMat);
      soil.position.set(0, 0.13, 0);
      island.add(soil);

      for (const sx of [-1.15, 1.15]) {
        for (const sz of [-1.45, 1.45]) {
          const shrub = new THREE.Mesh(new THREE.DodecahedronGeometry(0.55, 1), foliageMat);
          shrub.scale.set(1.1, 0.72, 1.1);
          shrub.position.set(sx, 0.48, sz);
          shrub.castShadow = true;
          island.add(shrub);
        }
      }

      const trunk = new THREE.Mesh(new THREE.CylinderGeometry(0.11, 0.15, 1.75, 8), trunkMat);
      trunk.position.set(0, 1.05, 0);
      trunk.castShadow = true;
      island.add(trunk);

      const crown = new THREE.Mesh(new THREE.DodecahedronGeometry(1.15, 1), foliageMat);
      crown.scale.set(1.1, 0.92, 1.1);
      crown.position.set(0, 2.35, 0);
      crown.castShadow = true;
      island.add(crown);

      // 高杆路灯（置于树池后缘，不占车位或车道）
      const mast = new THREE.Mesh(new THREE.CylinderGeometry(0.06, 0.09, 5.4, 10), poleMat);
      mast.position.set(0, 2.7, 2.25);
      mast.castShadow = true;
      island.add(mast);

      const arm = new THREE.Mesh(new THREE.BoxGeometry(0.08, 0.08, 1.25), poleMat);
      arm.position.set(0, 5.36, 1.72);
      island.add(arm);

      const lamp = new THREE.Mesh(new THREE.BoxGeometry(0.28, 0.06, 0.56), luminaireMat);
      lamp.position.set(0, 5.32, 1.2);
      island.add(lamp);

      lotGroup.add(island);
    }

    // 4. 5 个以上标准化垂直车位（2.5m × 5.3m，0.14m 清晰白线 + 车位编号 + 0.18m 高止轮器）
    const bayMats = {
      line: new THREE.MeshBasicMaterial({ color: SCENE.lineWhite }),
      stopper: getStdMaterial(SCENE.stopperRubber, 0.85, 0.1),
      stripe: new THREE.MeshBasicMaterial({ color: SCENE.stopperStripe }),
      evPad: new THREE.MeshStandardMaterial({
        color: SCENE.garageEvZone,
        roughness: 0.55,
        metalness: 0.05,
        polygonOffset: true,
        polygonOffsetFactor: -1,
        polygonOffsetUnits: -1,
      }),
    };
    const sharedBayLabelGeo = new THREE.PlaneGeometry(1.05, 0.52);

    for (let i = 0; i < uniqueBayXs.length; i++) {
      const bx = uniqueBayXs[i];
      const isEv = i >= uniqueBayXs.length - 2;
      const bayLabel = `P${String(i + 1).padStart(2, '0')}`;
      const bay = _createStandardBayGroup({
        ...bayMats,
        labelBgNum: isEv ? SCENE.garageEvZone : SCENE.lotPad,
      }, 2.5, 5.3, isEv, {
        lineWidth: 0.14,
        stopH: 0.18,
        stopW: 1.85,
        label: bayLabel,
        labelGeo: sharedBayLabelGeo,
      });
      const [bpx, bpy, bpz] = worldToThree(bx, bayY, 0.162);
      bay.position.set(bpx, bpy, bpz);
      bay.rotation.y = headingToRotationY(bayH);
      lotGroup.add(bay);
    }

    // 5. 后侧人行通道（y=-9.8..-11.2）+ 黄色安全边缘线 + 防撞柱 + 双色 EV 充电桩
    const walkwayW = padMaxX - padMinX - 0.6;
    const walkwayMat = getStdMaterial(SCENE.sidewalk, 0.86, 0.02);
    const walkway = new THREE.Mesh(new THREE.BoxGeometry(walkwayW, 0.05, 1.4), walkwayMat);
    const [wx, wy, wz] = worldToThree(cx, -10.5, 0.168);
    walkway.position.set(wx, wy, wz);
    walkway.receiveShadow = true;
    lotGroup.add(walkway);

    const edgeStripe = new THREE.Mesh(new THREE.BoxGeometry(walkwayW, 0.015, 0.12), bayMats.stripe);
    const [esx, esy, esz] = worldToThree(cx, -9.84, 0.195);
    edgeStripe.position.set(esx, esy, esz);
    lotGroup.add(edgeStripe);

    const bollardCount = uniqueBayXs.length + 1;
    const bollardGeo = new THREE.CylinderGeometry(0.065, 0.065, 0.78, 10);
    const bollardInst = new THREE.InstancedMesh(bollardGeo, bayMats.stripe, bollardCount);
    const capGeo = new THREE.CylinderGeometry(0.068, 0.068, 0.14, 10);
    const capInst = new THREE.InstancedMesh(capGeo, bayMats.stopper, bollardCount);
    const dummy = new THREE.Object3D();
    for (let i = 0; i < bollardCount; i++) {
      const bx = i < uniqueBayXs.length ? uniqueBayXs[i] - 1.95 : eastBayX + 1.95;
      const [blx, bly, blz] = worldToThree(bx, -9.55, 0.55);
      dummy.position.set(blx, bly, blz);
      dummy.rotation.set(0, 0, 0);
      dummy.updateMatrix();
      bollardInst.setMatrixAt(i, dummy.matrix);

      dummy.position.set(blx, bly + 0.22, blz);
      dummy.updateMatrix();
      capInst.setMatrixAt(i, dummy.matrix);
    }
    lotGroup.add(bollardInst);
    lotGroup.add(capInst);

    // 双色 EV 充电桩（0.42m × 0.28m × 1.55m，含底座、深色机身、金属顶盖与发光状态屏）
    const chargerBodyMat = getStdMaterial(SCENE.evChargerBody, 0.42, 0.35);
    const chargerLedMat = createEmissiveMaterial(SCENE.evChargerLed, 0.95);
    const chargerXs = uniqueBayXs.slice(-3);
    for (const chx of chargerXs) {
      const charger = new THREE.Group();
      const [cgx, cgy, cgz] = worldToThree(chx, -10.05, 0.19);
      charger.position.set(cgx, cgy, cgz);

      const plinth = new THREE.Mesh(new THREE.BoxGeometry(0.48, 0.12, 0.34), curbMat);
      plinth.position.set(0, 0.06, 0);
      charger.add(plinth);

      const tower = new THREE.Mesh(new THREE.BoxGeometry(0.42, 1.30, 0.28), chargerBodyMat);
      tower.position.set(0, 0.77, 0);
      tower.castShadow = true;
      charger.add(tower);

      const topCap = new THREE.Mesh(new THREE.BoxGeometry(0.44, 0.13, 0.30), poleMat);
      topCap.position.set(0, 1.485, 0);
      charger.add(topCap);

      const led = new THREE.Mesh(new THREE.BoxGeometry(0.28, 0.24, 0.29), chargerLedMat);
      led.position.set(0, 1.18, 0);
      charger.add(led);

      lotGroup.add(charger);
    }

    // 6. 入口闸机渠化安全岛（x=3198, y=-4.5）+ 黄黑警示条纹 + 车牌识别立柱 + 空余席位 LED 屏
    const gateIslandX = Math.min(3198.0, padMinX - 6.0);
    const gateGroup = new THREE.Group();
    const [gx, gy, gz] = worldToThree(gateIslandX, -4.5, 0.16);
    gateGroup.position.set(gx, gy, gz);

    const islandBase = new THREE.Mesh(new THREE.BoxGeometry(6.2, 0.22, 1.25), curbMat);
    islandBase.position.set(0, 0.11, 0);
    gateGroup.add(islandBase);

    const stripeSegCount = 12;
    const stripeSegGeo = new THREE.BoxGeometry(0.5, 0.10, 1.29);
    const islandYInst = new THREE.InstancedMesh(stripeSegGeo, bayMats.stripe, stripeSegCount / 2);
    const islandBInst = new THREE.InstancedMesh(stripeSegGeo, bayMats.stopper, stripeSegCount / 2);
    const sObj = new THREE.Object3D();
    let iyi = 0, ibi = 0;
    for (let k = 0; k < stripeSegCount; k++) {
      sObj.position.set(-2.75 + k * 0.5, 0.06, 0);
      sObj.updateMatrix();
      if (k % 2 === 0) islandYInst.setMatrixAt(iyi++, sObj.matrix);
      else islandBInst.setMatrixAt(ibi++, sObj.matrix);
    }
    gateGroup.add(islandYInst);
    gateGroup.add(islandBInst);

    const lprPole = new THREE.Mesh(new THREE.BoxGeometry(0.26, 1.48, 0.26), chargerBodyMat);
    lprPole.position.set(-1.8, 0.96, 0);
    lprPole.castShadow = true;
    gateGroup.add(lprPole);

    const lprHead = new THREE.Mesh(new THREE.BoxGeometry(0.36, 0.24, 0.32), chargerBodyMat);
    lprHead.position.set(-1.8, 1.66, -0.05);
    gateGroup.add(lprHead);

    const lprScreen = new THREE.Mesh(new THREE.BoxGeometry(0.06, 0.28, 0.20), chargerLedMat);
    lprScreen.position.set(-1.94, 1.30, 0);
    gateGroup.add(lprScreen);

    // P 空余 1 席 LED 导视牌
    const signPost = new THREE.Mesh(new THREE.CylinderGeometry(0.06, 0.07, 2.75, 10), poleMat);
    signPost.position.set(1.8, 1.48, 0);
    gateGroup.add(signPost);

    const pSignMat = _safeSignMaterial('P  空余 1 席', SCENE.signBlueBg, SCENE.guideLine, SCENE.guideLine, {
      width: 320,
      height: 160,
      fontSize: 44,
      emissiveIntensity: 0.55,
    });
    const pBoard = new THREE.Mesh(new THREE.BoxGeometry(0.10, 0.95, 1.75), pSignMat);
    pBoard.position.set(1.8, 2.75, 0);
    gateGroup.add(pBoard);

    lotGroup.add(gateGroup);

    // 7. 入口渠化引道垫层（连接 Edge 8 y=-3.5 与停车场西缘）+ 进场导向箭头
    const throatLen = Math.max(2.0, padMinX - (gateIslandX + 3.1));
    const throatCX = gateIslandX + 3.1 + throatLen * 0.5;
    const throatApron = new THREE.Mesh(new THREE.BoxGeometry(throatLen + 0.6, 0.12, 2.4), padMat);
    const [thx, thy, thz] = worldToThree(throatCX, -4.5, 0.094);
    throatApron.position.set(thx, thy, thz);
    throatApron.receiveShadow = true;
    lotGroup.add(throatApron);

    const throatGuide = new THREE.Mesh(new THREE.BoxGeometry(throatLen, 0.012, 0.12), bayMats.line);
    const [tgx, tgy, tgz] = worldToThree(throatCX, -5.55, 0.158);
    throatGuide.position.set(tgx, tgy, tgz);
    lotGroup.add(throatGuide);

    const arrow = _createLaneArrow(bayMats.line, 1.15);
    const [arx, ary, arz] = worldToThree(padMinX - 2.8, -1.75, 0.118);
    arrow.position.set(arx, ary, arz);
    lotGroup.add(arrow);

    group.add(lotGroup);
  }

  function build(roadNetwork, frame) {
    const edges = Array.isArray(roadNetwork && roadNetwork.edges) ? roadNetwork.edges : [];
    const garageEdges = edges.filter((e) => e && e.type === EDGE_TYPE.GARAGE_HALL);
    const openRamps = edges.filter((e) => e && e.type === EDGE_TYPE.OPEN_RAMP);
    const slots = (frame && Array.isArray(frame.parking_slots)) ? frame.parking_slots : [];

    const sig = `${garageEdges.length}:${openRamps.length}:`
      + garageEdges.map((e) => e.id).join(',') + '|'
      + openRamps.map((e) => e.id).join(',');
    const slotsSig = slots.map((s) => `${s.x}:${s.y}:${s.z || 0}`).join(';');

    if (sig === builtSig) {
      if (slotsSig !== lastSlotsSig) {
        lastSlotsSig = slotsSig;
        _buildSurfaceLotPad(slots);
      }
      return;
    }

    clear();
    builtSig = sig;
    lastSlotsSig = slotsSig;

    if (!garageEdges.length && !openRamps.length && !slots.length) return;

    const rampEndpoints = [];
    for (const r of openRamps) {
      const ns = _parseNodes(r);
      if (ns.length >= 2) {
        rampEndpoints.push(ns[0], ns[ns.length - 1]);
      }
    }
    const nearRampEnd = (pt) => rampEndpoints.some((p) => Math.hypot(p[0] - pt[0], p[1] - pt[1]) < 6.0);

    // ── 1. B1 地下车库剖切结构体 ──
    for (const gEdge of garageEdges) {
      const pts = _parseNodes(gEdge);
      if (pts.length < 2) continue;
      const p0 = pts[0];
      const p1 = pts[pts.length - 1];
      const length = Math.hypot(p1[0] - p0[0], p1[1] - p0[1]);
      if (length < 5.0) continue;

      const zBase = Math.min(p0[2], p1[2]);
      const hdg = headingBetweenPoints(p0[0], p0[1], p1[0], p1[1]);
      const hallGroup = new THREE.Group();
      const [ax, ay, az] = worldToThree(p0[0], p0[1], zBase);
      hallGroup.position.set(ax, ay, az);
      hallGroup.rotation.y = headingToRotationY(hdg);

      const startPad = nearRampEnd(p0) ? 0.0 : 11.8;
      const endPad = nearRampEnd(p1) ? 0.0 : 11.8;
      const totalLen = length + startPad + endPad;
      const centerS = (length + endPad - startPad) * 0.5;
      const hallHalfW = 11.6;
      const wallH = Math.max(2.6, -zBase - 0.04);
      const wallThick = 0.45;

      // 1a. 环氧地坪 + 深色行车道分区
      const floorMat = new THREE.MeshStandardMaterial({
        color: SCENE.garageFloor,
        roughness: 0.42,
        metalness: 0.08,
      });
      const floorMesh = new THREE.Mesh(new THREE.BoxGeometry(totalLen, 0.24, hallHalfW * 2), floorMat);
      floorMesh.position.set(centerS, -0.14, 0);
      floorMesh.receiveShadow = true;
      hallGroup.add(floorMesh);

      const aisleMat = getStdMaterial(SCENE.garageAisle, 0.78, 0.04);
      const aisleMesh = new THREE.Mesh(new THREE.BoxGeometry(totalLen - 0.5, 0.012, 7.0), aisleMat);
      aisleMesh.position.set(centerS, -0.014, 0);
      aisleMesh.receiveShadow = true;
      hallGroup.add(aisleMesh);

      // 行车道中心黄色虚线与直行箭头
      const centerDashMat = new THREE.MeshBasicMaterial({ color: SCENE.lineYellow });
      const b1WhiteMat = new THREE.MeshBasicMaterial({ color: SCENE.lineWhite });
      const dashCount = Math.floor((length - 10) / 5);
      if (dashCount > 0) {
        const dashGeo = new THREE.BoxGeometry(2.2, 0.008, 0.12);
        const dashInst = new THREE.InstancedMesh(dashGeo, centerDashMat, dashCount);
        const dObj = new THREE.Object3D();
        for (let i = 0; i < dashCount; i++) {
          dObj.position.set(4 + i * 5, 0.004, 0);
          dObj.updateMatrix();
          dashInst.setMatrixAt(i, dObj.matrix);
        }
        hallGroup.add(dashInst);
      }

      for (const arrowS of [28, 56, 84, 108]) {
        if (arrowS > length - 8) continue;
        const arr = _createLaneArrow(b1WhiteMat, 1.0);
        arr.position.set(arrowS, 0.005, 1.75);
        hallGroup.add(arr);
      }

      // 1b. 四周结构墙体（南侧全高 + 北侧 y>0 剖切矮墙保证剖视相机无遮挡）
      const wallMat = getStdMaterial(SCENE.garageWall, 0.85, 0.05);
      const copingMat = getStdMaterial(SCENE.rampCoping, 0.72, 0.05);

      // 南侧（z = +hallHalfW）全高挡土墙
      const southWall = new THREE.Mesh(new THREE.BoxGeometry(totalLen, wallH, wallThick), wallMat);
      southWall.position.set(centerS, wallH * 0.5 - 0.05, hallHalfW);
      southWall.castShadow = true;
      southWall.receiveShadow = true;
      hallGroup.add(southWall);

      const southCoping = new THREE.Mesh(new THREE.BoxGeometry(totalLen + 0.3, 0.14, wallThick + 0.24), copingMat);
      southCoping.position.set(centerS, wallH - 0.04, hallHalfW);
      hallGroup.add(southCoping);

      // 南侧墙面蓝色导视色带 + 底部黄黑警示踢脚线
      const wallZoneMat = getStdMaterial(SCENE.signBlueBg, 0.55, 0.08);
      const southBand = new THREE.Mesh(new THREE.BoxGeometry(totalLen - 0.2, 0.38, 0.03), wallZoneMat);
      southBand.position.set(centerS, 1.35, hallHalfW - wallThick * 0.5 - 0.015);
      hallGroup.add(southBand);

      const skirtYMat = new THREE.MeshBasicMaterial({ color: SCENE.garageHazardBand });
      const skirtBMat = getStdMaterial(SCENE.stopperRubber, 0.85, 0.1);
      const skirtSegW = 1.5;
      const skirtCount = Math.max(2, Math.floor((totalLen - 0.4) / skirtSegW));
      const skirtGeo = new THREE.BoxGeometry(skirtSegW, 0.30, 0.03);
      const skirtYInst = new THREE.InstancedMesh(skirtGeo, skirtYMat, Math.ceil(skirtCount / 2));
      const skirtBInst = new THREE.InstancedMesh(skirtGeo, skirtBMat, Math.floor(skirtCount / 2));
      const skObj = new THREE.Object3D();
      let syi = 0, sbi = 0;
      const skirtStartS = centerS - totalLen * 0.5 + 0.2 + skirtSegW * 0.5;
      for (let i = 0; i < skirtCount; i++) {
        skObj.position.set(skirtStartS + i * skirtSegW, 0.12, hallHalfW - wallThick * 0.5 - 0.015);
        skObj.updateMatrix();
        if (i % 2 === 0) skirtYInst.setMatrixAt(syi++, skObj.matrix);
        else skirtBInst.setMatrixAt(sbi++, skObj.matrix);
      }
      hallGroup.add(skirtYInst);
      hallGroup.add(skirtBInst);

      // 北侧（z = -hallHalfW，即 ENU y > 0）保留 0.65m 剖切矮墙，保障 p2p_preview_2 全览无遮挡
      const cutawayH = 0.65;
      const northWall = new THREE.Mesh(new THREE.BoxGeometry(totalLen, cutawayH, wallThick), wallMat);
      northWall.position.set(centerS, cutawayH * 0.5 - 0.05, -hallHalfW);
      northWall.receiveShadow = true;
      hallGroup.add(northWall);

      const northCoping = new THREE.Mesh(new THREE.BoxGeometry(totalLen + 0.3, 0.12, wallThick + 0.2), copingMat);
      northCoping.position.set(centerS, cutawayH - 0.02, -hallHalfW);
      hallGroup.add(northCoping);

      if (!nearRampEnd(p0)) {
        const backWall = new THREE.Mesh(new THREE.BoxGeometry(wallThick, wallH, hallHalfW * 2), wallMat);
        backWall.position.set(-startPad, wallH * 0.5 - 0.05, 0);
        hallGroup.add(backWall);
        const backCoping = new THREE.Mesh(new THREE.BoxGeometry(wallThick + 0.24, 0.14, hallHalfW * 2 + 0.4), copingMat);
        backCoping.position.set(-startPad, wallH - 0.04, 0);
        hallGroup.add(backCoping);

        const backBand = new THREE.Mesh(new THREE.BoxGeometry(0.03, 0.38, hallHalfW * 2 - 0.4), wallZoneMat);
        backBand.position.set(-startPad + wallThick * 0.5 + 0.015, 1.35, 0);
        hallGroup.add(backBand);

        const backSkirtCount = Math.floor((hallHalfW * 2 - 0.4) / skirtSegW);
        const backSkirtGeo = new THREE.BoxGeometry(0.03, 0.30, skirtSegW);
        const backYInst = new THREE.InstancedMesh(backSkirtGeo, skirtYMat, Math.ceil(backSkirtCount / 2));
        const backBInst = new THREE.InstancedMesh(backSkirtGeo, skirtBMat, Math.floor(backSkirtCount / 2));
        let byi = 0, bbi = 0;
        for (let i = 0; i < backSkirtCount; i++) {
          skObj.position.set(-startPad + wallThick * 0.5 + 0.015, 0.12, -hallHalfW + 0.2 + skirtSegW * 0.5 + i * skirtSegW);
          skObj.updateMatrix();
          if (i % 2 === 0) backYInst.setMatrixAt(byi++, skObj.matrix);
          else backBInst.setMatrixAt(bbi++, skObj.matrix);
        }
        hallGroup.add(backYInst);
        hallGroup.add(backBInst);
      }

      if (nearRampEnd(p1)) {
        const halfPortal = ((Number(gEdge.lanes) || 2) * (Number(gEdge.lane_width) || 3.5)) * 0.5 + 0.85;
        const wingW = Math.max(0.5, hallHalfW - halfPortal);
        const southWing = new THREE.Mesh(new THREE.BoxGeometry(wallThick, wallH, wingW), wallMat);
        southWing.position.set(length, wallH * 0.5 - 0.05, halfPortal + wingW * 0.5);
        hallGroup.add(southWing);
        const northWing = new THREE.Mesh(new THREE.BoxGeometry(wallThick, cutawayH, wingW), wallMat);
        northWing.position.set(length, cutawayH * 0.5 - 0.05, -(halfPortal + wingW * 0.5));
        hallGroup.add(northWing);
      } else {
        const frontWall = new THREE.Mesh(new THREE.BoxGeometry(wallThick, wallH, hallHalfW * 2), wallMat);
        frontWall.position.set(length + endPad, wallH * 0.5 - 0.05, 0);
        hallGroup.add(frontWall);
      }

      // 1c. 北侧剖切电梯厅/楼梯间核心筒（s=68..78, z=-11.6，含玻璃门 + 发光 EXIT + 红色消火栓箱）
      if (length > 82) {
        const lobby = new THREE.Group();
        lobby.position.set(73, 0, -hallHalfW);
        const lobbyH = 3.05;
        const lobbyShell = new THREE.Mesh(new THREE.BoxGeometry(8.6, lobbyH, 0.36), wallMat);
        lobbyShell.position.set(0, lobbyH * 0.5, -0.9);
        lobby.add(lobbyShell);
        for (const sx of [-4.15, 4.15]) {
          const sideP = new THREE.Mesh(new THREE.BoxGeometry(0.35, lobbyH, 1.8), wallMat);
          sideP.position.set(sx, lobbyH * 0.5, 0);
          lobby.add(sideP);
        }
        const lobbyRoof = new THREE.Mesh(new THREE.BoxGeometry(8.9, 0.16, 2.1), copingMat);
        lobbyRoof.position.set(0, lobbyH, 0);
        lobby.add(lobbyRoof);

        const glassMat = new THREE.MeshStandardMaterial({
          color: SCENE.garageGlass,
          roughness: 0.15,
          metalness: 0.15,
          transparent: true,
          opacity: 0.52,
        });
        const doorGlass = new THREE.Mesh(new THREE.BoxGeometry(2.6, 2.25, 0.06), glassMat);
        doorGlass.position.set(0, 1.15, 0.02);
        lobby.add(doorGlass);

        const frameMat = getStdMaterial(SCENE.poleSteel, 0.45, 0.45);
        const surroundMat = getStdMaterial(SCENE.garagePillar, 0.55, 0.08);
        const topTrim = new THREE.Mesh(new THREE.BoxGeometry(3.1, 0.16, 0.24), surroundMat);
        topTrim.position.set(0, 2.36, 0.06);
        lobby.add(topTrim);
        for (const mx of [-1.45, 1.45]) {
          const sideTrim = new THREE.Mesh(new THREE.BoxGeometry(0.18, 2.4, 0.24), surroundMat);
          sideTrim.position.set(mx, 1.2, 0.06);
          lobby.add(sideTrim);
        }
        for (const mx of [-1.32, 0, 1.32]) {
          const mullion = new THREE.Mesh(new THREE.BoxGeometry(0.07, 2.3, 0.1), frameMat);
          mullion.position.set(mx, 1.15, 0.02);
          lobby.add(mullion);
        }
        for (const hx of [-0.14, 0.14]) {
          const handle = new THREE.Mesh(new THREE.BoxGeometry(0.035, 0.42, 0.08), frameMat);
          handle.position.set(hx, 1.05, 0.09);
          lobby.add(handle);
        }

        const exitMat = _safeSignMaterial('EXIT', SCENE.exitSignBg, SCENE.exitSignFg, SCENE.exitSignFg, {
          width: 192,
          height: 96,
          fontSize: 44,
          emissiveIntensity: 0.85,
        });
        const exitSign = new THREE.Mesh(new THREE.BoxGeometry(1.18, 0.40, 0.10), exitMat);
        exitSign.position.set(0, 2.68, 0.14);
        lobby.add(exitSign);

        const hydrantMat = getStdMaterial(SCENE.garageSprinkler, 0.48, 0.25);
        const hydrantBox = new THREE.Mesh(new THREE.BoxGeometry(0.68, 0.95, 0.24), hydrantMat);
        hydrantBox.position.set(-2.6, 1.25, 0.14);
        lobby.add(hydrantBox);

        hallGroup.add(lobby);
      }

      // 1d. 结构立柱与下缘斜纹警示带（InstancedMesh，避开所有停放车辆与车道）+ 南侧立柱消火栓箱
      const pillarMat = getStdMaterial(SCENE.garagePillar, 0.7, 0.05);
      const bandMat = _safeStripeMaterial(SCENE.garageHazardBand, SCENE.stopperRubber, { stripeCount: 6 });
      const guardMat = getStdMaterial(SCENE.stopperRubber, 0.8, 0.1);
      const pillarSList = [2, 19.1, 31.5, 48, 64, 82, 96, 110, 124].filter((s) => s < length - 2);
      const pillarTotal = pillarSList.length * 2;
      if (pillarTotal > 0) {
        const colInst = new THREE.InstancedMesh(new THREE.BoxGeometry(0.55, wallH - 0.18, 0.55), pillarMat, pillarTotal);
        const bandInst = new THREE.InstancedMesh(new THREE.BoxGeometry(0.58, 0.95, 0.58), bandMat, pillarTotal);
        const guardInst = new THREE.InstancedMesh(new THREE.BoxGeometry(0.62, 0.85, 0.62), guardMat, pillarTotal);
        colInst.castShadow = true;
        const pObj = new THREE.Object3D();
        let idx = 0;
        for (const s of pillarSList) {
          for (const side of [-1, 1]) {
            const pH = side === -1 ? Math.min(2.2, wallH - 0.2) : wallH - 0.18;
            const zPos = side * (hallHalfW - 0.85);
            pObj.position.set(s, pH * 0.5 - 0.02, zPos);
            pObj.scale.set(1, side === -1 ? pH / (wallH - 0.18) : 1, 1);
            pObj.updateMatrix();
            colInst.setMatrixAt(idx, pObj.matrix);

            pObj.scale.set(1, 1, 1);
            pObj.position.set(s, 0.5, zPos);
            pObj.updateMatrix();
            bandInst.setMatrixAt(idx, pObj.matrix);

            pObj.position.set(s, 0.44, zPos);
            pObj.updateMatrix();
            guardInst.setMatrixAt(idx, pObj.matrix);
            idx++;
          }
        }
        hallGroup.add(colInst);
        hallGroup.add(bandInst);
        hallGroup.add(guardInst);

        const colHydrantMat = getStdMaterial(SCENE.garageSprinkler, 0.45, 0.25);
        const colGlassMat = getStdMaterial(SCENE.garageGlass, 0.2, 0.15);
        const colHydrantGeo = new THREE.BoxGeometry(0.42, 0.72, 0.18);
        const colGlassGeo = new THREE.BoxGeometry(0.24, 0.28, 0.03);
        for (const hs of [19.1, 64, 110]) {
          if (hs >= length - 2) continue;
          const hBox = new THREE.Mesh(colHydrantGeo, colHydrantMat);
          hBox.position.set(hs, 1.32, hallHalfW - 0.85 - 0.36);
          hallGroup.add(hBox);
          const hWin = new THREE.Mesh(colGlassGeo, colGlassMat);
          hWin.position.set(hs, 1.44, hallHalfW - 0.85 - 0.46);
          hallGroup.add(hWin);
        }
      }

      // 1e. 坡道口减速带（s=118, s=124）与坡底截水沟格栅（s=128.5）
      const bumpGeo = new THREE.BoxGeometry(0.45, 0.05, 0.52);
      const bumpYMat = new THREE.MeshBasicMaterial({ color: SCENE.stopperStripe });
      const bumpBMat = getStdMaterial(SCENE.stopperRubber, 0.85, 0.1);
      const bumpXs = [118, 124].filter((s) => s < length - 3);
      if (bumpXs.length > 0) {
        const segsPerBump = 12;
        const yCount = bumpXs.length * (segsPerBump / 2);
        const yInst = new THREE.InstancedMesh(bumpGeo, bumpYMat, yCount);
        const bInst = new THREE.InstancedMesh(bumpGeo, bumpBMat, yCount);
        const bObj = new THREE.Object3D();
        let yi = 0, bi = 0;
        for (const bs of bumpXs) {
          for (let k = 0; k < segsPerBump; k++) {
            const bz = -2.86 + k * 0.52;
            bObj.position.set(bs, 0.015, bz);
            bObj.updateMatrix();
            if (k % 2 === 0) yInst.setMatrixAt(yi++, bObj.matrix);
            else bInst.setMatrixAt(bi++, bObj.matrix);
          }
        }
        hallGroup.add(yInst);
        hallGroup.add(bInst);
      }

      if (length > 120) {
        const trenchS = length - 1.5;
        const trough = new THREE.Mesh(new THREE.BoxGeometry(0.56, 0.03, 7.0), aisleMat);
        trough.position.set(trenchS, -0.012, 0);
        hallGroup.add(trough);

        const grateMat = getStdMaterial(SCENE.garageGrate, 0.45, 0.45);
        const barGeo = new THREE.BoxGeometry(0.05, 0.018, 6.8);
        const barInst = new THREE.InstancedMesh(barGeo, grateMat, 6);
        const gObj = new THREE.Object3D();
        for (let i = 0; i < 6; i++) {
          gObj.position.set(trenchS - 0.22 + i * 0.088, 0.002, 0);
          gObj.updateMatrix();
          barInst.setMatrixAt(i, gObj.matrix);
        }
        hallGroup.add(barInst);
      }

      // 1f. 顶部横梁与南侧机电管线（仅置于南侧结构跨 y < -3.5 即 z_three > 3.5，保证剖视相机无遮挡）
      const beamMat = getStdMaterial(SCENE.garageBeam, 0.78, 0.1);
      const ledMat = createEmissiveMaterial(SCENE.garageLedStrip, 0.9);
      const ductMat = getStdMaterial(SCENE.garageDuct, 0.46, 0.42);
      const pipeMat = getStdMaterial(SCENE.garageSprinkler, 0.48, 0.28);
      const beamY = Math.max(3.55, wallH - 0.35);

      for (let s = 8; s <= length - 6; s += 20) {
        const beam = new THREE.Mesh(new THREE.BoxGeometry(0.32, 0.28, hallHalfW - 1.2), beamMat);
        beam.position.set(s, beamY, (hallHalfW - 0.6) * 0.5);
        hallGroup.add(beam);
      }

      // 线性 LED 灯带、镀锌风管、电缆桥架与红色消防喷淋管（全部位于南侧 z_three = +4.6 .. +9.2）
      const mepLen = Math.max(10, length - 10);
      const mepCenterS = length * 0.5;
      const ledStrip = new THREE.Mesh(new THREE.BoxGeometry(mepLen, 0.06, 0.14), ledMat);
      ledStrip.position.set(mepCenterS, beamY - 0.18, 4.8);
      hallGroup.add(ledStrip);

      const hvacDuct = new THREE.Mesh(new THREE.BoxGeometry(mepLen, 0.34, 0.62), ductMat);
      hvacDuct.position.set(mepCenterS, beamY - 0.26, 6.6);
      hallGroup.add(hvacDuct);

      const cableTray = new THREE.Mesh(new THREE.BoxGeometry(mepLen, 0.08, 0.42), ductMat);
      cableTray.position.set(mepCenterS, beamY - 0.22, 8.0);
      hallGroup.add(cableTray);

      const sprinkler = new THREE.Mesh(
        new THREE.CylinderGeometry(0.05, 0.05, mepLen, 8),
        pipeMat
      );
      sprinkler.rotation.z = Math.PI / 2;
      sprinkler.position.set(mepCenterS, beamY - 0.28, 9.0);
      hallGroup.add(sprinkler);

      // 坡道口悬吊式 EXIT / 出口 → 导视牌
      if (length > 100) {
        const hangS = length - 16;
        const signMat = _safeSignMaterial('EXIT  出口 →', SCENE.exitSignBg, SCENE.exitSignFg, SCENE.exitSignFg, {
          width: 320,
          height: 112,
          fontSize: 42,
          emissiveIntensity: 0.55,
        });
        const signBox = new THREE.Mesh(new THREE.BoxGeometry(0.1, 0.55, 2.2), signMat);
        signBox.position.set(hangS, 3.45, 0);
        hallGroup.add(signBox);
        for (const hz of [-0.85, 0.85]) {
          const rod = new THREE.Mesh(new THREE.CylinderGeometry(0.02, 0.02, 0.5, 6), ductMat);
          rod.position.set(hangS, 3.85, hz);
          hallGroup.add(rod);
        }
      }

      for (const ls of [24, 66, 108]) {
        if (ls > length - 4) continue;
        const pLight = new THREE.PointLight(SCENE.garageLedStrip);
        pLight.intensity = 24.0;
        pLight.distance = 52;
        pLight.decay = 1.5;
        pLight.position.set(ls, beamY - 0.45, 0);
        hallGroup.add(pLight);
      }

      // 1g. B1 地下标准垂直车位标线（2.5m × 5.3m）与止轮器（含 ego 起始车位与 EV 车位 + 车位编号）
      const b1Mats = {
        line: b1WhiteMat,
        stopper: b1StopMatFallback(SCENE),
        stripe: new THREE.MeshBasicMaterial({ color: SCENE.stopperStripe }),
        evPad: new THREE.MeshStandardMaterial({
          color: SCENE.garageEvZone,
          roughness: 0.52,
          metalness: 0.05,
        }),
      };
      const b1LabelGeo = new THREE.PlaneGeometry(1.05, 0.52);
      const addB1Stall = (sPos, lateralY, rotY, isEv = false, label = '') => {
        if (sPos < -startPad + 2 || sPos > length - 4) return;
        const sg = _createStandardBayGroup({
          ...b1Mats,
          labelBgNum: isEv ? SCENE.garageEvZone : SCENE.garageFloor,
        }, 2.5, 5.3, isEv, {
          lineWidth: 0.12,
          stopH: 0.14,
          label,
          labelGeo: b1LabelGeo,
        });
        sg.position.set(sPos, 0.012, -lateralY);
        sg.rotation.y = rotY;
        hallGroup.add(sg);
      };
      let b1Idx = 1;
      for (const s of [7.4, 12.0, 16.8, 21.4, 26.2]) {
        addB1Stall(s, -6.8, headingToRotationY(Math.PI * 0.5), s >= 21.4, `B1-${String(b1Idx++).padStart(2, '0')}`);
      }
      for (const s of [36.0, 44.0, 52.0, 60.0]) {
        addB1Stall(s, 6.8, headingToRotationY(-Math.PI * 0.5), s >= 52.0, `B1-${String(b1Idx++).padStart(2, '0')}`);
      }

      group.add(hallGroup);
    }

    // ── 2. 敞开式出库坡道（U 型挡土侧墙 + 顶部压边 + 敞口防眩格栅） ──
    for (const rEdge of openRamps) {
      const pts = _parseNodes(rEdge);
      if (pts.length < 2) continue;
      const lanes = Number(rEdge.lanes) || 2;
      const laneW = Number(rEdge.lane_width) || 3.5;
      const wallOffset = lanes * laneW * 0.5 + 0.12;
      const innerMat = new THREE.MeshStandardMaterial({
        color: SCENE.rampWall,
        roughness: 0.85,
        metalness: 0.05,
        side: THREE.DoubleSide,
      });
      const outerMat = new THREE.MeshStandardMaterial({
        color: SCENE.rampCoping,
        roughness: 0.78,
        metalness: 0.05,
        side: THREE.DoubleSide,
      });

      const rampStart = pts[0];
      const rampEnd = pts[pts.length - 1];
      const rampLen = Math.hypot(rampEnd[0] - rampStart[0], rampEnd[1] - rampStart[1]);
      const rampHdg = headingBetweenPoints(rampStart[0], rampStart[1], rampEnd[0], rampEnd[1]);
      const rampMidX = (rampStart[0] + rampEnd[0]) * 0.5;
      const rampMidY = (rampStart[1] + rampEnd[1]) * 0.5;

      for (const side of [-1, 1]) {
        const innerGeo = _buildSlopedWallGeo(pts, side * wallOffset, 0.05, 0.25);
        if (innerGeo) {
          const wallMesh = new THREE.Mesh(innerGeo, innerMat);
          wallMesh.receiveShadow = true;
          group.add(wallMesh);
        }
        // 水平压顶缘石（贴地齐平 z=0.04，替代旧垂直外墙，消除草坪外白墙穿模）
        const copingBar = new THREE.Mesh(new THREE.BoxGeometry(rampLen, 0.14, 0.42), outerMat);
        const [cosR, sinR] = forwardENU(rampHdg);
        const nxR = -sinR;
        const nyR = cosR;
        const [cxC, , czC] = worldToThree(rampMidX + nxR * side * (wallOffset + 0.18), rampMidY + nyR * side * (wallOffset + 0.18), 0.02);
        copingBar.position.set(cxC, 0.02, czC);
        copingBar.rotation.y = headingToRotationY(rampHdg);
        group.add(copingBar);
      }

      // 隧道洞口敞口防眩格栅（x = p0.x .. p0.x + 16，横跨双侧挡墙顶缘 + 两侧钢梁）
      const p0 = pts[0];
      const pEnd = pts[pts.length - 1];
      const rHdg = headingBetweenPoints(p0[0], p0[1], pEnd[0], pEnd[1]);
      const louverCount = 11;
      const louverGeo = new THREE.BoxGeometry(0.24, 0.22, wallOffset * 2 + 0.4);
      const louverInst = new THREE.InstancedMesh(louverGeo, outerMat, louverCount);
      const lObj = new THREE.Object3D();
      const [cosH, sinH] = forwardENU(rHdg);
      for (let i = 0; i < louverCount; i++) {
        const dist = 0.8 + i * 1.52;
        const [lx, , lz] = worldToThree(p0[0] + cosH * dist, p0[1] + sinH * dist, 0.12);
        lObj.position.set(lx, 0.12, lz);
        lObj.rotation.set(0, headingToRotationY(rHdg), 0);
        lObj.updateMatrix();
        louverInst.setMatrixAt(i, lObj.matrix);
      }
      louverInst.castShadow = true;
      group.add(louverInst);

      const frameMat = getStdMaterial(SCENE.poleSteel, 0.45, 0.42);
      const [nx, nz] = tangentToNormal(cosH, -sinH);
      const beamLen = 16.4;
      const [mx, , mz] = worldToThree(p0[0] + cosH * 8.4, p0[1] + sinH * 8.4, 0.16);
      for (const side of [-1, 1]) {
        const edgeBeam = new THREE.Mesh(new THREE.BoxGeometry(beamLen, 0.22, 0.24), frameMat);
        edgeBeam.position.set(mx + nx * side * wallOffset, 0.14, mz + nz * side * wallOffset);
        edgeBeam.rotation.y = headingToRotationY(rHdg);
        group.add(edgeBeam);
      }
    }

    // ── 3. 地面停车场沥青铺装垫层 ──
    _buildSurfaceLotPad(slots);
  }

  function update(store) {
    if (!store) return;
    const slots = Array.isArray(store.parkingSlots) ? store.parkingSlots : [];
    if (!slots.length) return;
    const slotsSig = slots.map((s) => `${s.x}:${s.y}:${s.z || 0}`).join(';');
    if (slotsSig !== lastSlotsSig) {
      lastSlotsSig = slotsSig;
      _buildSurfaceLotPad(slots);
    }
  }

  return { build, update, clear, getGroup: () => group };
}

function b1StopMatFallback(sceneTokens) {
  return getStdMaterial(sceneTokens.stopperRubber, 0.85, 0.1);
}
