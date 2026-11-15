/**
 * ETCGateView.js — ETC 收费广场门架
 *
 * 从 utils.js _buildETCGate 搬迁：4 车道收费广场
 *   - 广场地面（深沥青）
 *   - 入口大型标志门架（跨全部车道，"ETC"发光字）
 *   - 4 个收费亭（黄黑警示岛 + 小屋 + 屋顶 + 窗 + 显示屏）
 *   - 4 套 ETC 门架（2 立柱 + 横梁 + 标志板 + 栏杆）
 *   - 4 个栏杆 boom：根据 entity.state 控制抬起角度
 *
 * update(store) 扫描 entities 里 type='etc_gate' 的实体，
 *   state='open'/'closed' 或 1/0 → boom.rotation.y 抬起/落下
 *
 * 单个 ETC 广场 mesh 内含全部 4 车道，position 取 entity.x/y。
 */

import { getStdMaterial, createEmissiveMaterial } from '../core/AssetFactory.js';
import { worldToThree, headingToRotationY } from '../math/Coord.js';
import { roadHeightAt } from '../math/RoadHeight.js';

const N_BOOTHS = 4;
const LANE_W = 3.5;

export function createETCGateView(scene) {
  // entity id → { group, booms: [boom×4] }
  const pool = new Map();

  function _createETCGate() {
    const g = new THREE.Group();
    const totalWidth = N_BOOTHS * LANE_W;
    const halfW = totalWidth / 2;

    // ── 共享材质 ──
    const plazaMat    = getStdMaterial(0x1a1a1a, 0.92, 0.0);
    const islandMat   = createEmissiveMaterial(0xffcc00, 0.18);
    islandMat.color.setHex(0xffcc00); islandMat.emissive.setHex(0x332200);
    const boothMat    = getStdMaterial(0xe8e8ec, 0.55, 0.1);
    const boothRoofMat= createEmissiveMaterial(0x2255aa, 0.18);
    boothRoofMat.color.setHex(0x2255aa); boothRoofMat.emissive.setHex(0x113355);
    const glassMat    = createEmissiveMaterial(0x0a1a2a, 0.12);
    glassMat.color.setHex(0x0a1a2a); glassMat.emissive.setHex(0x081020);
    const poleMat     = getStdMaterial(0x445566, 0.35, 0.6);
    const signMat     = createEmissiveMaterial(0x2266aa, 0.55);
    signMat.color.setHex(0x2266aa); signMat.emissive.setHex(0x1166aa);
    const boomMat     = createEmissiveMaterial(0xffcc00, 0.35);
    boomMat.color.setHex(0xffcc00); boomMat.emissive.setHex(0x442200);
    const screenMat   = createEmissiveMaterial(0x00ff66, 0.9);
    const tipMat      = createEmissiveMaterial(0xff2222, 0.9);

    // ── 广场地面 ──
    const plaza = new THREE.Mesh(
      new THREE.PlaneGeometry(10.0, totalWidth + 2.5),
      plazaMat
    );
    plaza.rotation.x = -Math.PI / 2;
    plaza.position.y = 0.02;
    plaza.receiveShadow = true;
    g.add(plaza);

    // ── 入口大型标志门架 ──
    const megaPoleGeo = new THREE.CylinderGeometry(0.16, 0.20, 7.0, 12);
    const megaPole1 = new THREE.Mesh(megaPoleGeo, poleMat);
    megaPole1.position.set(-3.5, 3.5, -halfW - 0.7);
    megaPole1.castShadow = true;
    g.add(megaPole1);
    const megaPole2 = new THREE.Mesh(megaPoleGeo, poleMat);
    megaPole2.position.set(-3.5, 3.5, halfW + 0.7);
    megaPole2.castShadow = true;
    g.add(megaPole2);
    const megaBeam = new THREE.Mesh(
      new THREE.BoxGeometry(0.22, 0.22, totalWidth + 1.6),
      poleMat
    );
    megaBeam.position.set(-3.5, 6.85, 0);
    megaBeam.castShadow = true;
    g.add(megaBeam);
    const megaSign = new THREE.Mesh(
      new THREE.BoxGeometry(0.10, 0.85, totalWidth * 0.7),
      signMat
    );
    megaSign.position.set(-3.38, 6.85, 0);
    g.add(megaSign);
    const etcText = new THREE.Mesh(
      new THREE.BoxGeometry(0.04, 0.18, totalWidth * 0.45),
      screenMat
    );
    etcText.position.set(-3.30, 6.40, 0);
    g.add(etcText);

    // ── 共享几何体 ──
    const poleGeo   = new THREE.CylinderGeometry(0.08, 0.10, 5.0, 10);
    const beamGeo   = new THREE.BoxGeometry(0.12, 0.12, LANE_W - 0.15);
    const signGeo   = new THREE.BoxGeometry(0.06, 0.32, (LANE_W - 0.15) * 0.75);
    const islandGeo = new THREE.BoxGeometry(2.0, 0.20, 0.45);
    const boothGeo  = new THREE.BoxGeometry(1.7, 2.3, 1.05);
    const roofGeo   = new THREE.BoxGeometry(1.85, 0.12, 1.2);
    const winGeo    = new THREE.BoxGeometry(0.04, 0.7, 0.85);
    const screenGeo = new THREE.BoxGeometry(0.04, 0.18, 0.45);

    const booms = [];

    // ── 4 个并列收费口 ──
    for (let i = 0; i < N_BOOTHS; i++) {
      const zCenter = (i - (N_BOOTHS - 1) / 2) * LANE_W;
      const poleZ1 = zCenter - LANE_W / 2 + 0.15;
      const poleZ2 = zCenter + LANE_W / 2 - 0.15;
      const islandZ = zCenter + LANE_W / 2 - 0.25;

      // 收费岛
      const island = new THREE.Mesh(islandGeo, islandMat);
      island.position.set(0, 0.10, islandZ);
      island.castShadow = true; island.receiveShadow = true;
      g.add(island);

      // 收费亭
      const booth = new THREE.Mesh(boothGeo, boothMat);
      booth.position.set(0, 1.35, islandZ);
      booth.castShadow = true;
      g.add(booth);
      const roof = new THREE.Mesh(roofGeo, boothRoofMat);
      roof.position.set(0, 2.56, islandZ);
      g.add(roof);
      const win = new THREE.Mesh(winGeo, glassMat);
      win.position.set(-0.86, 1.5, islandZ);
      g.add(win);
      const screen = new THREE.Mesh(screenGeo, screenMat);
      screen.position.set(-0.88, 1.05, islandZ);
      g.add(screen);

      // ETC 门架
      const pole1 = new THREE.Mesh(poleGeo, poleMat);
      pole1.position.set(0, 2.5, poleZ1);
      pole1.castShadow = true;
      g.add(pole1);
      const pole2 = new THREE.Mesh(poleGeo, poleMat);
      pole2.position.set(0, 2.5, poleZ2);
      pole2.castShadow = true;
      g.add(pole2);
      const beam = new THREE.Mesh(beamGeo, poleMat);
      beam.position.set(0, 4.85, zCenter);
      beam.castShadow = true;
      g.add(beam);
      const laneSign = new THREE.Mesh(signGeo, signMat);
      laneSign.position.set(0.09, 4.85, zCenter);
      g.add(laneSign);

      // 栏杆 boom（pivot 在 pole1 端，绕 Y 抬起）
      const boomLen = LANE_W - 0.35;
      const boomGeo = new THREE.BoxGeometry(0.07, 0.07, boomLen);
      boomGeo.translate(0, 0, boomLen / 2);
      const boom = new THREE.Mesh(boomGeo, boomMat);
      boom.position.set(0, 4.45, poleZ1);
      boom.castShadow = true;
      g.add(boom);
      booms.push(boom);

      // 末端红色警示灯
      const tipLight = new THREE.Mesh(
        new THREE.SphereGeometry(0.06, 8, 6),
        tipMat
      );
      tipLight.position.set(0, 4.45, poleZ1 + boomLen);
      g.add(tipLight);
    }

    g.userData.booms = booms;
    scene.add(g);
    return { group: g, booms };
  }

  /** 判断闸门是停车场/匝道单车道道闸，还是高速公路 4 车道 ETC 收费广场 */
  function _isParkingBarrier(store, ent) {
    if (ent && ent.gate_style === 'etc_plaza') return false;
    if (ent && ent.gate_style === 'parking_barrier') return true;
    const edges = (store && store.roadNetwork && Array.isArray(store.roadNetwork.edges))
      ? store.roadNetwork.edges
      : [];
    let bestDist = Infinity;
    let bestEdge = null;
    const ex = Number(ent && ent.x) || 0;
    const ey = Number(ent && ent.y) || 0;
    for (const edge of edges) {
      const nodes = Array.isArray(edge && edge.nodes) ? edge.nodes : [];
      for (let i = 1; i < nodes.length; i++) {
        const a = nodes[i - 1], b = nodes[i];
        const ax = Array.isArray(a) ? a[0] : a.x;
        const ay = Array.isArray(a) ? a[1] : a.y;
        const bx = Array.isArray(b) ? b[0] : b.x;
        const by = Array.isArray(b) ? b[1] : b.y;
        const dx = bx - ax, dy = by - ay;
        const len2 = dx * dx + dy * dy || 1e-9;
        const t = Math.max(0, Math.min(1, ((ex - ax) * dx + (ey - ay) * dy) / len2));
        const d = Math.hypot(ex - (ax + t * dx), ey - (ay + t * dy));
        if (d < bestDist) {
          bestDist = d;
          bestEdge = edge;
        }
      }
    }
    if (!bestEdge) return true;
    const t = String(bestEdge.type || '').toLowerCase();
    const n = String(bestEdge.name || '').toLowerCase();
    if (t === 'open_ramp' || t === 'garage_hall' || t === 'ramp' || t === 'parking') return true;
    if (n.includes('ramp') || n.includes('lot') || n.includes('park') || n.includes('garage')) return true;
    return (Number(bestEdge.lanes) || 2) <= 2;
  }

  /** 停车场/坡道出口标准道闸（安全岛 + 黄色道闸机箱 + 红白条纹起落杆 + 车牌识别一体机 + LED 屏） */
  function _createParkingBarrierGate() {
    const g = new THREE.Group();

    const islandMat = getStdMaterial(0x3a4048, 0.8, 0.08);
    const hazardMat = createEmissiveMaterial(0xf5b800, 0.22);
    const cabinetMat = createEmissiveMaterial(0xf59e0b, 0.20);
    const darkMetalMat = getStdMaterial(0x1f2937, 0.45, 0.4);
    const poleMat = getStdMaterial(0x64748b, 0.38, 0.45);
    const ledRedMat = createEmissiveMaterial(0xef4444, 0.9);
    const ledGreenMat = createEmissiveMaterial(0x22c55e, 0.9);
    const whiteBarMat = getStdMaterial(0xf8fafc, 0.35, 0.05);
    const redBarMat = createEmissiveMaterial(0xdc2626, 0.28);
    const lensMat = createEmissiveMaterial(0x38bdf8, 0.65);

    // 1. 右侧安全岛（z = +1.85m，沿道路方向 2.8m × 0.58m × 0.16m）
    const island = new THREE.Mesh(new THREE.BoxGeometry(2.8, 0.16, 0.58), islandMat);
    island.position.set(0, 0.08, 1.88);
    island.castShadow = true;
    island.receiveShadow = true;
    g.add(island);

    for (const sx of [-1.0, -0.33, 0.33, 1.0]) {
      const stripe = new THREE.Mesh(new THREE.BoxGeometry(0.32, 0.165, 0.60), hazardMat);
      stripe.position.set(sx, 0.08, 1.88);
      g.add(stripe);
    }

    // 2. 道闸机箱（黄色主机箱 + 顶部红/绿状态灯）
    const cabinet = new THREE.Mesh(new THREE.BoxGeometry(0.42, 1.08, 0.36), cabinetMat);
    cabinet.position.set(0.0, 0.70, 1.88);
    cabinet.castShadow = true;
    g.add(cabinet);

    const cap = new THREE.Mesh(new THREE.BoxGeometry(0.46, 0.10, 0.40), darkMetalMat);
    cap.position.set(0.0, 1.28, 1.88);
    g.add(cap);

    const statusLed = new THREE.Mesh(new THREE.BoxGeometry(0.28, 0.06, 0.41), ledRedMat);
    statusLed.position.set(0.0, 1.22, 1.88);
    g.add(statusLed);

    // 3. 红白相间起落杆（pivot 在机箱左侧 z=+1.68, y=0.92，向 -z 横跨车道 3.45m）
    const boomPivot = new THREE.Group();
    boomPivot.position.set(0.0, 0.92, 1.68);
    const boomLen = 3.45;
    const segCount = 8;
    const segLen = boomLen / segCount;
    for (let i = 0; i < segCount; i++) {
      const seg = new THREE.Mesh(
        new THREE.BoxGeometry(0.06, 0.085, segLen),
        i % 2 === 0 ? redBarMat : whiteBarMat
      );
      seg.position.set(0, 0, -(i + 0.5) * segLen);
      seg.castShadow = true;
      boomPivot.add(seg);
    }
    const rubberEdge = new THREE.Mesh(
      new THREE.BoxGeometry(0.05, 0.03, boomLen),
      darkMetalMat
    );
    rubberEdge.position.set(0, -0.055, -boomLen * 0.5);
    boomPivot.add(rubberEdge);
    const tipLight = new THREE.Mesh(new THREE.SphereGeometry(0.055, 8, 6), ledRedMat);
    tipLight.position.set(0, 0, -boomLen);
    boomPivot.add(tipLight);
    g.add(boomPivot);

    // 4. 车牌识别一体机（位于道闸前方 1.0m 安全岛上）
    const kioskPole = new THREE.Mesh(new THREE.BoxGeometry(0.20, 1.52, 0.18), darkMetalMat);
    kioskPole.position.set(-1.0, 0.92, 1.88);
    kioskPole.castShadow = true;
    g.add(kioskPole);

    const ledPanel = new THREE.Mesh(new THREE.BoxGeometry(0.04, 0.42, 0.34), ledGreenMat);
    ledPanel.position.set(-1.11, 1.38, 1.88);
    g.add(ledPanel);

    const camHousing = new THREE.Mesh(new THREE.BoxGeometry(0.24, 0.16, 0.16), poleMat);
    camHousing.position.set(-1.02, 1.68, 1.78);
    camHousing.rotation.y = 0.25;
    g.add(camHousing);

    const camLens = new THREE.Mesh(new THREE.CylinderGeometry(0.04, 0.04, 0.06, 8), lensMat);
    camLens.rotation.z = Math.PI / 2;
    camLens.position.set(-1.14, 1.68, 1.75);
    g.add(camLens);

    // 5. 限高/出入口龙门架（单跨 4.2m 窄钢架，清晰区分于高速 ETC 广场）
    const gantryPoleGeo = new THREE.CylinderGeometry(0.07, 0.085, 3.2, 10);
    for (const gz of [-1.95, 1.95]) {
      const gp = new THREE.Mesh(gantryPoleGeo, poleMat);
      gp.position.set(-0.6, 1.6, gz);
      gp.castShadow = true;
      g.add(gp);
    }
    const gantryBeam = new THREE.Mesh(new THREE.BoxGeometry(0.16, 0.18, 4.1), poleMat);
    gantryBeam.position.set(-0.6, 3.15, 0);
    g.add(gantryBeam);
    const gantrySign = new THREE.Mesh(new THREE.BoxGeometry(0.08, 0.36, 2.4), hazardMat);
    gantrySign.position.set(-0.56, 3.15, 0);
    g.add(gantrySign);

    scene.add(g);
    return {
      group: g,
      booms: [boomPivot],
      isParkingBarrier: true,
      statusLed,
      ledRedMat,
      ledGreenMat,
    };
  }

  /** boom 抬起角度：state='open'/1 → 抬起；'opening' → 半抬；'closed'/0 → 0° 落下 */
  function _setBoomState(entry, state) {
    const isOpen = state === 'open' || state === 1 || state === true;
    const isOpening = state === 'opening';
    if (entry.isParkingBarrier) {
      const targetAngle = isOpen ? Math.PI * 0.46 : (isOpening ? Math.PI * 0.22 : 0);
      for (const boom of entry.booms) {
        boom.rotation.x = targetAngle;
      }
      if (entry.statusLed) {
        entry.statusLed.material = isOpen ? entry.ledGreenMat : entry.ledRedMat;
      }
      return;
    }
    const targetAngle = isOpen ? -Math.PI / 3 : (isOpening ? -Math.PI / 6 : 0);
    for (const boom of entry.booms) {
      boom.rotation.x = targetAngle;
    }
  }

  /** 主更新入口：扫描 entities 找 type='etc_gate' */
  function update(store) {
    const all = (store.entities || []).filter(e => e.type === 'etc_gate');

    const aliveIds = new Set(all.map(e => e.id));
    for (const [id, entry] of pool.entries()) {
      if (!aliveIds.has(id)) {
        scene.remove(entry.group);
        pool.delete(id);
      }
    }

    for (const ent of all) {
      const wantParking = _isParkingBarrier(store, ent);
      let entry = pool.get(ent.id);
      if (entry && Boolean(entry.isParkingBarrier) !== wantParking) {
        scene.remove(entry.group);
        pool.delete(ent.id);
        entry = null;
      }
      if (!entry) {
        entry = wantParking ? _createParkingBarrierGate() : _createETCGate();
        pool.set(ent.id, entry);
      }
      const z = (typeof ent.z === 'number' && Number.isFinite(ent.z))
        ? ent.z
        : roadHeightAt(store, ent.x, ent.y);
      entry.group.position.set(...worldToThree(ent.x || 0, ent.y || 0, z));
      entry.group.rotation.y = headingToRotationY(ent.heading || 0);
      _setBoomState(entry, ent.state);
    }
  }

  function clear() {
    for (const [, entry] of pool) scene.remove(entry.group);
    pool.clear();
  }

  return { update, clear };
}
