/**
 * WorldPropsView.js — 世界引擎程序化设施与声明式精修道具 3D 视图
 *
 * 消费 WorldEngine（或前端兜底 WorldCompiler）输出的统一布局：
 *   1. worldLayout.signs     — GB 5768 交通标志（圆形限速牌 / 八角 STOP / 警告牌）
 *   2. worldLayout.furniture — 街道家具（室外消火栓 / 休闲长椅 / 公交候车亭 / 充电桩 / 防撞柱）
 *   3. worldLayout.props     — 场景精修自定义道具（跨路欢迎龙门架 gantry / 城市地标拱门 arch /
 *                              塔标 landmark_tower / 休闲亭 plaza_pavilion / bollard / ev_charger）
 *
 * 所有定位严格通过 math/Coord.js 转换，金属度受控（<=0.35）以保持 PBR 暗场可读性。
 */

import { getStdMaterial, getBox, getCylinder } from '../core/AssetFactory.js';
import { SCENE } from '../theme/tokens.js';
import { worldToThree, headingToRotationY, forwardENU } from '../math/Coord.js';
import {
  makeSpeedSignTexture,
  makeStopSignTexture,
  makeWarningSignTexture,
  makeGantryBannerTexture,
  makeSignTexture,
} from '../utils/CanvasTextureFactory.js';
import { compileWorld } from '../world/WorldCompiler.js';

export function createWorldPropsView(scene) {
  const group = new THREE.Group();
  group.name = 'worldProps';
  if (scene) scene.add(group);

  const ownedGeometries = [];
  const ownedMaterials = [];

  function trackGeo(g) { ownedGeometries.push(g); return g; }
  function trackMat(m) { ownedMaterials.push(m); return m; }

  function clear() {
    while (group.children.length > 0) {
      const child = group.children.pop();
      group.remove(child);
    }
    while (ownedGeometries.length > 0) {
      const g = ownedGeometries.pop();
      if (g && typeof g.dispose === 'function') g.dispose();
    }
    while (ownedMaterials.length > 0) {
      const m = ownedMaterials.pop();
      if (m && typeof m.dispose === 'function') m.dispose();
    }
  }

  function _addStaticBox(w, h, d, color, roughness, metalness, px, py, pz, rotY = 0, extraOpts = null) {
    const geo = getBox(w, h, d);
    const mat = trackMat(new THREE.MeshStandardMaterial({
      color,
      roughness,
      metalness,
      ...(extraOpts || {}),
    }));
    const mesh = new THREE.Mesh(geo, mat);
    mesh.position.set(px, py, pz);
    mesh.rotation.y = rotY;
    group.add(mesh);
    return mesh;
  }

  function _addStaticCyl(rTop, rBot, h, segs, color, roughness, metalness, px, py, pz, rotY = 0) {
    const geo = getCylinder(rTop, rBot, h, segs);
    const mat = trackMat(new THREE.MeshStandardMaterial({ color, roughness, metalness }));
    const mesh = new THREE.Mesh(geo, mat);
    mesh.position.set(px, py, pz);
    mesh.rotation.y = rotY;
    group.add(mesh);
    return mesh;
  }

  function _buildSigns(signs) {
    if (!Array.isArray(signs) || signs.length === 0) return;

    const postGeo = getCylinder(0.045, 0.05, 2.6, 8);
    const postMat = getStdMaterial(SCENE.poleSteel, { roughness: 0.45, metalness: 0.35 });
    const posts = new THREE.InstancedMesh(postGeo, postMat, signs.length);
    posts.name = 'world_signs_posts';

    const dummy = new THREE.Object3D();
    for (let i = 0; i < signs.length; i++) {
      const s = signs[i];
      const heading = Number(s.heading) || 0;
      const [fx, fy] = forwardENU(heading);
      const [tx, ty, tz] = worldToThree(s.x || 0, s.y || 0, s.z || 0);
      const [ftx, , ftz] = worldToThree((s.x || 0) - fx * 0.03, (s.y || 0) - fy * 0.03, s.z || 0);
      const rotY = headingToRotationY(heading);

      dummy.position.set(tx, ty + 1.3, tz);
      dummy.rotation.set(0, rotY, 0);
      dummy.updateMatrix();
      posts.setMatrixAt(i, dummy.matrix);

      // 标识面板（正面朝向来车方向，微前移 3cm 防止与背板 z-fighting）
      const kind = s.kind || 'speed_limit';
      let faceTex = null;
      let faceColor = SCENE.signWhite;
      let boardGeo = null;
      if (kind === 'speed_limit') {
        faceTex = makeSpeedSignTexture(s.speed_kph || 60);
        boardGeo = trackGeo(new THREE.CircleGeometry(0.42, 24));
      } else if (kind === 'stop' || kind === 'stop_sign') {
        faceTex = makeStopSignTexture(s.label || 'STOP');
        faceColor = SCENE.signRed;
        boardGeo = trackGeo(new THREE.CircleGeometry(0.44, 8));
      } else if (kind === 'warning' || kind === 'crosswalk_warn' ||
                 kind === 'curve_warn' || kind === 'slope_warn') {
        const warnLabel = s.label ||
          (kind === 'crosswalk_warn' ? 'XING'
            : kind === 'curve_warn' ? 'CURV'
            : kind === 'slope_warn' ? 'SLOPE' : '!');
        faceTex = makeWarningSignTexture(warnLabel);
        faceColor = SCENE.signYellow;
        boardGeo = trackGeo(new THREE.PlaneGeometry(0.76, 0.76));
      } else if (kind === 'exit_sign') {
        faceTex = makeSignTexture(s.label || 'EXIT', '#0f766e', '#ffffff', '#ccfbf1');
        faceColor = '#0f766e';
        boardGeo = trackGeo(new THREE.PlaneGeometry(0.95, 0.50));
      } else {
        faceTex = makeSignTexture(s.label || 'CITY', '#155799', '#ffffff', '#e2e8f0');
        faceColor = SCENE.signBlueBg;
        boardGeo = trackGeo(new THREE.PlaneGeometry(0.95, 0.56));
      }

      const faceMat = trackMat(new THREE.MeshStandardMaterial({
        map: faceTex || null,
        color: faceTex ? 0xffffff : faceColor,
        roughness: 0.45,
        metalness: 0.1,
      }));
      const faceMesh = new THREE.Mesh(boardGeo, faceMat);
      faceMesh.position.set(ftx, ty + 2.35, ftz);
      faceMesh.rotation.y = rotY - Math.PI * 0.5;
      group.add(faceMesh);

      const backMat = getStdMaterial(SCENE.tlHousing, { roughness: 0.65, metalness: 0.25 });
      const backMesh = new THREE.Mesh(getBox(0.04, 0.66, 0.66), backMat);
      backMesh.position.set(tx, ty + 2.35, tz);
      backMesh.rotation.y = rotY;
      group.add(backMesh);
    }
    posts.instanceMatrix.needsUpdate = true;
    group.add(posts);
  }

  function _buildFurniture(furniture) {
    if (!Array.isArray(furniture) || furniture.length === 0) return;

    const hydrants = furniture.filter(f => f && f.kind === 'hydrant');
    const benches = furniture.filter(f => f && f.kind === 'bench');
    const shelters = furniture.filter(f => f && f.kind === 'bus_shelter');
    const chargers = furniture.filter(f => f && f.kind === 'ev_charger');

    const dummy = new THREE.Object3D();

    if (hydrants.length > 0) {
      const bodyGeo = getCylinder(0.14, 0.16, 0.78, 10);
      const capGeo = getCylinder(0.11, 0.14, 0.18, 10);
      const hydMat = getStdMaterial(SCENE.hydrantRed, { roughness: 0.45, metalness: 0.2 });
      const bodyInst = new THREE.InstancedMesh(bodyGeo, hydMat, hydrants.length);
      const capInst = new THREE.InstancedMesh(capGeo, hydMat, hydrants.length);
      hydrants.forEach((f, idx) => {
        const [tx, ty, tz] = worldToThree(f.x, f.y, f.z || 0);
        dummy.position.set(tx, ty + 0.39, tz);
        dummy.rotation.set(0, 0, 0);
        dummy.updateMatrix();
        bodyInst.setMatrixAt(idx, dummy.matrix);
        dummy.position.set(tx, ty + 0.87, tz);
        dummy.updateMatrix();
        capInst.setMatrixAt(idx, dummy.matrix);
      });
      bodyInst.instanceMatrix.needsUpdate = true;
      capInst.instanceMatrix.needsUpdate = true;
      group.add(bodyInst, capInst);
    }

    if (benches.length > 0) {
      const seatGeo = getBox(1.75, 0.12, 0.56);
      const legGeo = getBox(0.12, 0.42, 0.52);
      const seatMat = getStdMaterial(SCENE.benchWood, { roughness: 0.72, metalness: 0.05 });
      const legMat = getStdMaterial(SCENE.poleSteel, { roughness: 0.5, metalness: 0.35 });
      const seatInst = new THREE.InstancedMesh(seatGeo, seatMat, benches.length);
      const legInst = new THREE.InstancedMesh(legGeo, legMat, benches.length * 2);
      benches.forEach((f, idx) => {
        const [tx, ty, tz] = worldToThree(f.x, f.y, f.z || 0);
        const rotY = headingToRotationY(Number(f.heading) || 0);
        const [fx, fy] = forwardENU(Number(f.heading) || 0);
        dummy.position.set(tx, ty + 0.46, tz);
        dummy.rotation.set(0, rotY, 0);
        dummy.updateMatrix();
        seatInst.setMatrixAt(idx, dummy.matrix);
        for (let k = 0; k < 2; k++) {
          const sign = k === 0 ? -0.68 : 0.68;
          const [lx, ly, lz] = worldToThree(f.x + fx * sign, f.y + fy * sign, (f.z || 0) + 0.21);
          dummy.position.set(lx, ly, lz);
          dummy.rotation.set(0, rotY, 0);
          dummy.updateMatrix();
          legInst.setMatrixAt(idx * 2 + k, dummy.matrix);
        }
      });
      seatInst.instanceMatrix.needsUpdate = true;
      legInst.instanceMatrix.needsUpdate = true;
      group.add(seatInst, legInst);
    }

    if (shelters.length > 0) {
      const postGeo = getCylinder(0.06, 0.06, 2.35, 8);
      const roofGeo = getBox(3.4, 0.14, 1.4);
      const glassGeo = getBox(3.2, 1.85, 0.06);
      const steelMat = getStdMaterial(SCENE.poleSteel, { roughness: 0.42, metalness: 0.35 });
      const roofMat = getStdMaterial(SCENE.shelterRoof, { roughness: 0.58, metalness: 0.2 });
      const glassMat = trackMat(new THREE.MeshStandardMaterial({
        color: SCENE.shelterGlass,
        roughness: 0.2,
        metalness: 0.1,
        transparent: true,
        opacity: 0.45,
      }));
      const postInst = new THREE.InstancedMesh(postGeo, steelMat, shelters.length * 2);
      const roofInst = new THREE.InstancedMesh(roofGeo, roofMat, shelters.length);
      const glassInst = new THREE.InstancedMesh(glassGeo, glassMat, shelters.length);
      shelters.forEach((f, idx) => {
        const heading = Number(f.heading) || 0;
        const [fx, fy] = forwardENU(heading);
        const rotY = headingToRotationY(heading);
        const [tx, ty, tz] = worldToThree(f.x, f.y, f.z || 0);
        for (let k = 0; k < 2; k++) {
          const off = k === 0 ? -1.45 : 1.45;
          const [px, py, pz] = worldToThree(f.x + fx * off, f.y + fy * off, (f.z || 0) + 1.175);
          dummy.position.set(px, py, pz);
          dummy.rotation.set(0, rotY, 0);
          dummy.updateMatrix();
          postInst.setMatrixAt(idx * 2 + k, dummy.matrix);
        }
        dummy.position.set(tx, ty + 2.38, tz);
        dummy.rotation.set(0, rotY, 0);
        dummy.updateMatrix();
        roofInst.setMatrixAt(idx, dummy.matrix);

        dummy.position.set(tx, ty + 1.12, tz);
        dummy.updateMatrix();
        glassInst.setMatrixAt(idx, dummy.matrix);
      });
      postInst.instanceMatrix.needsUpdate = true;
      roofInst.instanceMatrix.needsUpdate = true;
      glassInst.instanceMatrix.needsUpdate = true;
      group.add(postInst, roofInst, glassInst);
    }

    if (chargers.length > 0) {
      const boxGeo = getBox(0.55, 1.55, 0.38);
      const ledGeo = getBox(0.57, 0.12, 0.4);
      const bodyMat = getStdMaterial(SCENE.evChargerBody, { roughness: 0.45, metalness: 0.25 });
      const ledMat = getStdMaterial(SCENE.evChargerLed, {
        roughness: 0.25,
        metalness: 0.1,
        emissive: SCENE.evChargerLed,
        emissiveIntensity: 0.7,
      });
      const bodyInst = new THREE.InstancedMesh(boxGeo, bodyMat, chargers.length);
      const ledInst = new THREE.InstancedMesh(ledGeo, ledMat, chargers.length);
      chargers.forEach((f, idx) => {
        const [tx, ty, tz] = worldToThree(f.x, f.y, f.z || 0);
        const rotY = headingToRotationY(Number(f.heading) || 0);
        dummy.position.set(tx, ty + 0.775, tz);
        dummy.rotation.set(0, rotY, 0);
        dummy.updateMatrix();
        bodyInst.setMatrixAt(idx, dummy.matrix);
        dummy.position.set(tx, ty + 1.5, tz);
        dummy.updateMatrix();
        ledInst.setMatrixAt(idx, dummy.matrix);
      });
      bodyInst.instanceMatrix.needsUpdate = true;
      ledInst.instanceMatrix.needsUpdate = true;
      group.add(bodyInst, ledInst);
    }
  }

  function _buildCustomProps(props) {
    if (!Array.isArray(props) || props.length === 0) return;

    for (let i = 0; i < props.length; i++) {
      const p = props[i];
      if (!p) continue;
      const kind = p.kind || 'bollard';
      const ex = Number(p.x) || 0;
      const ey = Number(p.y) || 0;
      const ez = Number(p.z) || 0;
      const heading = Number(p.heading) || 0;
      const rotY = headingToRotationY(heading);
      const [fx, fy] = forwardENU(heading);
      const lx = -fy;
      const ly = fx;
      const [tx, ty, tz] = worldToThree(ex, ey, ez);

      if (kind === 'welcome_gantry' || kind === 'gantry') {
        const spanW = Math.max(8.0, Number(p.width) || 16.0);
        const clearanceH = Math.max(5.5, Number(p.height) || 6.8);
        const halfSpan = spanW * 0.5;
        for (const side of [-1, 1]) {
          const [cx, cy, cz] = worldToThree(ex + lx * side * halfSpan, ey + ly * side * halfSpan, ez + clearanceH * 0.5);
          _addStaticBox(0.48, clearanceH, 0.48, SCENE.gantrySteel, 0.42, 0.35, cx, cy, cz, rotY);
        }
        // 顶部门架横梁
        _addStaticBox(0.55, 0.58, spanW + 0.8, SCENE.gantrySteel, 0.42, 0.35, tx, ty + clearanceH - 0.25, tz, rotY);
        // 中央横幅标牌（正反双面）
        const bannerW = Math.max(5.2, spanW * 0.52);
        const bannerTex = makeGantryBannerTexture(p.label || 'FLOWENGINE CITY', p.subtitle || 'PROCEDURAL WORLD ENGINE');
        const bannerMat = trackMat(new THREE.MeshStandardMaterial({
          map: bannerTex || null,
          color: bannerTex ? 0xffffff : SCENE.gantryBlue,
          roughness: 0.4,
          metalness: 0.15,
        }));
        const bannerMesh = new THREE.Mesh(getBox(0.18, 1.45, bannerW), bannerMat);
        bannerMesh.position.set(tx, ty + clearanceH - 0.1, tz);
        bannerMesh.rotation.y = rotY;
        group.add(bannerMesh);
      } else if (kind === 'arch_landmark' || kind === 'arch') {
        const spanW = Math.max(9.0, Number(p.width) || 18.0);
        const archH = Math.max(7.0, Number(p.height) || 9.2);
        const halfSpan = spanW * 0.5;
        for (const side of [-1, 1]) {
          const [cx, cy, cz] = worldToThree(ex + lx * side * halfSpan, ey + ly * side * halfSpan, ez + archH * 0.5);
          _addStaticBox(1.15, archH, 1.05, SCENE.landmarkStone, 0.52, 0.2, cx, cy, cz, rotY);
        }
        _addStaticBox(1.1, 1.15, spanW + 1.4, SCENE.landmarkStone, 0.48, 0.22, tx, ty + archH - 0.55, tz, rotY);
        _addStaticBox(1.16, 0.22, spanW * 0.85, SCENE.slotSelected, 0.25, 0.15, tx, ty + archH - 0.05, tz, rotY, {
          emissive: SCENE.slotSelected,
          emissiveIntensity: 0.55,
        });
      } else if (kind === 'landmark_tower') {
        const baseW = Math.max(2.4, Number(p.length) || 4.0);
        const totalH = Math.max(6.0, Number(p.height) || 14.0);
        _addStaticBox(baseW, 0.65, baseW, SCENE.plazaTile, 0.65, 0.08, tx, ty + 0.325, tz, rotY);
        _addStaticCyl(baseW * 0.18, baseW * 0.32, totalH - 1.2, 8, SCENE.landmarkStone, 0.45, 0.25, tx, ty + 0.65 + (totalH - 1.2) * 0.5, tz, rotY);
        _addStaticCyl(0.16, 0.24, 1.0, 8, SCENE.slotSelected, 0.2, 0.2, tx, ty + totalH - 0.4, tz, rotY);
      } else if (kind === 'plaza_pavilion' || kind === 'pavilion') {
        const L = Math.max(3.0, Number(p.length) || 6.0);
        const W = Math.max(3.0, Number(p.width) || 6.0);
        const H = Math.max(3.0, Number(p.height) || 3.8);
        _addStaticBox(L, 0.24, W, SCENE.plazaTile, 0.72, 0.05, tx, ty + 0.12, tz, rotY);
        _addStaticBox(L + 0.5, 0.22, W + 0.5, SCENE.shelterRoof, 0.5, 0.25, tx, ty + H, tz, rotY);
        for (const sx of [-1, 1]) {
          for (const sy of [-1, 1]) {
            const ox = fx * sx * (L * 0.42) + lx * sy * (W * 0.42);
            const oy = fy * sx * (L * 0.42) + ly * sy * (W * 0.42);
            const [cx, cy, cz] = worldToThree(ex + ox, ey + oy, ez + H * 0.5);
            _addStaticCyl(0.11, 0.12, H - 0.15, 8, SCENE.poleSteel, 0.45, 0.35, cx, cy, cz, 0);
          }
        }
      } else if (kind === 'bollard') {
        const r = Math.max(0.12, (Number(p.width) || 0.42) * 0.5);
        const h = Math.max(0.7, Number(p.height) || 1.15);
        _addStaticCyl(r, r * 1.08, h, 10, SCENE.poleSteel, 0.45, 0.35, tx, ty + h * 0.5, tz, 0);
        _addStaticCyl(r * 1.05, r * 1.05, h * 0.18, 10, SCENE.garageHazardBand, 0.4, 0.1, tx, ty + h * 0.78, tz, 0);
      } else if (kind === 'ev_charger') {
        const h = Math.max(1.2, Number(p.height) || 1.8);
        _addStaticBox(Number(p.length) || 0.7, h, Number(p.width) || 0.55, SCENE.evChargerBody, 0.45, 0.28, tx, ty + h * 0.5, tz, rotY);
        _addStaticBox((Number(p.length) || 0.7) + 0.02, 0.14, (Number(p.width) || 0.55) + 0.02, SCENE.evChargerLed, 0.25, 0.1, tx, ty + h - 0.12, tz, rotY);
      } else {
        const L = Math.max(0.5, Number(p.length) || 1.2);
        const W = Math.max(0.5, Number(p.width) || 1.2);
        const H = Math.max(0.5, Number(p.height) || 2.0);
        _addStaticBox(L, H, W, SCENE.landmarkStone, 0.55, 0.2, tx, ty + H * 0.5, tz, rotY);
      }
    }
  }

  /**
   * 构建世界程序化构件（交通标志 / 街道家具 / 场景精修自定义道具）。
   * @param {object} roadNetwork
   * @param {object} [opts]  支持传 `{ worldLayout }`，缺省时用 WorldCompiler 兜底。
   */
  function build(roadNetwork, opts = null) {
    clear();
    const layout = (opts && opts.worldLayout)
      ? opts.worldLayout
      : (opts && Array.isArray(opts.signs) ? opts : compileWorld(roadNetwork));
    if (!layout) return;

    _buildSigns(layout.signs);
    _buildFurniture(layout.furniture);
    _buildCustomProps(layout.props);
  }

  function update() {}
  function dispose() { clear(); }
  function getGroup() { return group; }

  return { build, update, clear, dispose, getGroup };
}
