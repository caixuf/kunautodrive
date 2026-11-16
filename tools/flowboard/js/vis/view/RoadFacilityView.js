/**
 * RoadFacilityView.js — low-cost scenario-specific road furniture.
 *
 * Derives markings from the existing road/entity contract:
 * - traffic lights -> lane-direction stop lines
 * - parking roads + stationary cars -> occupied/empty parking bays and exam poles
 * - urban/exam roads -> directional arrows
 *
 * All road paint shares one InstancedMesh and all poles share another, keeping
 * the upgrade at two draw calls regardless of scene length.
 */

import {
  distanceENU, forwardENU, headingBetweenPoints, headingToRotationY,
  tangentToNormal, worldToThree,
} from '../math/Coord.js';
import { isGarageOrRampEdge } from '../core/Constants.js';
import { SCENE } from '../theme/tokens.js';

const PAINT_Y = 0.162;
const PAINT_H = 0.018;
const LINE_W = 0.12;
const BAY_LENGTH = 6.2;
const BAY_WIDTH = 2.8;
const ARROW_SPACING = 120;
const MAX_ARROWS_PER_EDGE = 16;
const CROSSWALK_STRIPE_W = 0.48; // 条纹宽（跨道路方向，GB 5768.3 40~45cm）
const CROSSWALK_GAP = 0.38;      // 条带间距（跨道路方向，规范 60cm 上下）
const CROSSWALK_LENGTH = 2.5;    // 条纹沿道路长度（交叉口 ≥2m，路段 ≥3m）

function roadNodes(edge) {
  if (!Array.isArray(edge?.nodes)) return [];
  return edge.nodes.map((node) => Array.isArray(node)
    ? { x: Number(node[0]) || 0, y: Number(node[1]) || 0, z: Number(node[2]) || 0 }
    : { x: Number(node?.x) || 0, y: Number(node?.y) || 0, z: Number(node?.z) || 0 });
}

function samplePolyline(nodes, spacing, limit) {
  const out = [];
  if (nodes.length < 2) return out;
  let next = spacing * 0.5;
  let accumulated = 0;
  for (let i = 1; i < nodes.length && out.length < limit; i++) {
    const a = nodes[i - 1], b = nodes[i];
    const segment = distanceENU(a.x, a.y, b.x, b.y);
    if (segment < 1e-6) continue;
    const slope = Math.atan((b.z - a.z) / segment);
    while (next <= accumulated + segment && out.length < limit) {
      const t = (next - accumulated) / segment;
      out.push({
        x: a.x + (b.x - a.x) * t,
        y: a.y + (b.y - a.y) * t,
        z: a.z + (b.z - a.z) * t,
        pitch: slope,
        heading: headingBetweenPoints(a.x, a.y, b.x, b.y),
      });
      next += spacing;
    }
    accumulated += segment;
  }
  return out;
}

function nearestRoadPoint(edges, x, y, edgeFilter = () => true) {
  let best = null;
  edges.forEach((edge, edgeIndex) => {
    if (!edgeFilter(edge)) return;
    const nodes = roadNodes(edge);
    let edgeS = 0;
    for (let i = 1; i < nodes.length; i++) {
      const a = nodes[i - 1], b = nodes[i];
      const dx = b.x - a.x, dy = b.y - a.y;
      const length2 = dx * dx + dy * dy;
      if (length2 < 1e-9) continue;
      const segmentLength = Math.sqrt(length2);
      const t = Math.max(0, Math.min(1, ((x - a.x) * dx + (y - a.y) * dy) / length2));
      const refX = a.x + dx * t, refY = a.y + dy * t;
      const refZ = a.z + (b.z - a.z) * t;
      const distance = distanceENU(x, y, refX, refY);
      const heading = headingBetweenPoints(a.x, a.y, b.x, b.y);
      const [fx, fy] = forwardENU(heading);
      const [nx, ny] = tangentToNormal(fx, fy);
      if (!best || distance < best.distance) {
        best = {
          edge, edgeIndex, distance, heading,
          refX, refY, refZ,
          pitch: Math.atan((b.z - a.z) / segmentLength),
          s: edgeS + segmentLength * t,
          lateral: (x - refX) * nx + (y - refY) * ny,
        };
      }
      edgeS += segmentLength;
    }
  });
  return best;
}

function addMark(layout, x, y, heading, length, width = LINE_W, z = 0, pitch = 0) {
  layout.marks.push({ x, y, z, heading, pitch, length, width });
}

function offsetPoint(x, y, heading, along, lateral) {
  const [fx, fy] = forwardENU(heading);
  const [nx, ny] = tangentToNormal(fx, fy);
  return { x: x + fx * along + nx * lateral, y: y + fy * along + ny * lateral };
}

function addParkingBay(layout, x, y, heading, empty) {
  for (const lateral of [-BAY_WIDTH * 0.5, BAY_WIDTH * 0.5]) {
    const p = offsetPoint(x, y, heading, 0, lateral);
    addMark(layout, p.x, p.y, heading, BAY_LENGTH);
  }
  for (const along of [-BAY_LENGTH * 0.5, BAY_LENGTH * 0.5]) {
    const p = offsetPoint(x, y, heading, along, 0);
    addMark(layout, p.x, p.y, heading + Math.PI * 0.5, BAY_WIDTH);
  }
  if (empty) {
    for (const along of [-BAY_LENGTH * 0.5, BAY_LENGTH * 0.5]) {
      for (const lateral of [-BAY_WIDTH * 0.5, BAY_WIDTH * 0.5]) {
        const p = offsetPoint(x, y, heading, along, lateral);
        layout.poles.push(p);
      }
    }
  }
}

function addStraightArrow(layout, x, y, heading, z = 0, pitch = 0) {
  const tanP = Math.tan(pitch);
  const stem = offsetPoint(x, y, heading, -0.2, 0);
  addMark(layout, stem.x, stem.y, heading, 2.8, 0.28, z - 0.2 * tanP, pitch);
  for (const side of [-1, 1]) {
    const head = offsetPoint(x, y, heading, 0.85, side * 0.25);
    const barbRel = Math.PI - side * Math.PI * 0.20;
    const [barbCos] = forwardENU(barbRel);
    addMark(layout, head.x, head.y,
      heading + barbRel, 1.1, 0.26, z + 0.85 * tanP, pitch * barbCos);
  }
  layout.arrows++;
}

function addLeftArrow(layout, x, y, heading, z = 0, pitch = 0) {
  const tanP = Math.tan(pitch);
  const stem = offsetPoint(x, y, heading, -0.65, 0);
  addMark(layout, stem.x, stem.y, heading, 1.5, 0.26, z - 0.65 * tanP, pitch);
  const diag = offsetPoint(x, y, heading, 0.35, 0.35);
  addMark(layout, diag.x, diag.y, heading + Math.PI * 0.25, 1.0, 0.26, z + 0.35 * tanP, pitch * 0.707);
  const horiz = offsetPoint(x, y, heading, 0.70, 0.95);
  addMark(layout, horiz.x, horiz.y, heading + Math.PI * 0.5, 0.85, 0.26, z + 0.70 * tanP, 0);
  const tip = offsetPoint(x, y, heading, 0.70, 1.40);
  for (const side of [-1, 1]) {
    const barbHeading = heading + Math.PI * 0.5 + Math.PI - side * Math.PI * 0.20;
    const barb = offsetPoint(tip.x, tip.y, barbHeading, 0.42, 0);
    addMark(layout, barb.x, barb.y, barbHeading, 0.85, 0.24, z + 0.70 * tanP, 0);
  }
  layout.arrows++;
}

function collectJunctionHubs(edges) {
  const clusters = [];
  for (const edge of edges) {
    if (isGarageOrRampEdge(edge)) continue;
    const nodes = roadNodes(edge);
    if (nodes.length < 2) continue;
    for (const end of [nodes[0], nodes[nodes.length - 1]]) {
      const hit = clusters.find(c => distanceENU(c.x, c.y, end.x, end.y) < 4.0);
      if (hit) {
        hit.x = (hit.x * hit.count + end.x) / (hit.count + 1);
        hit.y = (hit.y * hit.count + end.y) / (hit.count + 1);
        hit.count++;
      } else {
        clusters.push({ x: end.x, y: end.y, count: 1 });
      }
    }
  }
  return clusters.filter(c => c.count >= 3);
}

/**
 * 每条可行驶车道的横向偏移 + 行进朝向。
 * offsetPoint 的 lateral 在 ENU 里：heading=0（朝东）时正值=+y（北/左）。
 * 中国右侧通行：沿 edge heading 的车道在道路右侧（负 lateral）；对向在左侧。
 * 双向路绝不在 lateral=0（双黄线）落箭头。
 */
function laneArrowSlots(edge) {
  const lanes = Math.max(1, Number(edge.lanes) || 2);
  const laneW = Number(edge.lane_width) || 3.5;
  if (edge.oneway === true) {
    // 无 lane_data 时与 OpenDRIVE / RoadAxis 一致：单向车道分布在 spine 右侧 [-1..-lanes]
    return Array.from({ length: lanes }, (_, k) => ({
      lateral: -(k + 0.5) * laneW,
      headingFlip: 0,
    }));
  }
  const nPerSide = Math.max(1, Math.floor(lanes / 2));
  const slots = [];
  for (let k = 0; k < nPerSide; k++) {
    slots.push({ lateral: -(k + 0.5) * laneW, headingFlip: 0 });
    slots.push({ lateral: (k + 0.5) * laneW, headingFlip: Math.PI });
  }
  return slots;
}

function addCrosswalk(layout, center, heading, width, z = 0, pitch = 0) {
  /* GB 5768.3 5.8：斑马线条纹平行于道路中心线（与车同向），沿道路长度短
   * （交叉口 ≥2m / 路段 ≥3m）；跨道路方向铺多条条带。旧实现条纹长轴垂直
   * 道路（横跨半幅路面）→ 方向反了。 */
  const step = CROSSWALK_STRIPE_W + CROSSWALK_GAP;
  const count = Math.max(2, Math.round(width / step));
  for (let i = 0; i < count; i++) {
    const across = (i - (count - 1) / 2) * step;
    const p = offsetPoint(center.x, center.y, heading + Math.PI * 0.5, across, 0);
    addMark(layout, p.x, p.y, heading, CROSSWALK_LENGTH, CROSSWALK_STRIPE_W, z, pitch);
  }
  layout.crosswalks++;
}

export function inferRoadFacilities(roadNetwork, entities, scenarioName = '', options = {}) {
  const layout = {
    marks: [], poles: [], parkingBays: 0, stopLines: 0, crosswalks: 0, arrows: 0,
  };
  const edges = Array.isArray(roadNetwork?.edges) ? roadNetwork.edges : [];
  const allEntities = Array.isArray(entities) ? entities : [];
  const junctionHubs = collectJunctionHubs(edges);

  /* 大地图注入 lane_data 后，路口箭头由 ConnectorView 按 fork 画。
   * 这里再沿路每 120m 铺一轮会与路口标线叠成白网。 */
  const hasLaneData = roadNetwork && roadNetwork.lane_data
    && typeof roadNetwork.lane_data === 'object'
    && Object.keys(roadNetwork.lane_data).length > 0;

  if (!hasLaneData) {
    for (const edge of edges) {
      if (isGarageOrRampEdge(edge)) continue;
      const slots = laneArrowSlots(edge);
      for (const point of samplePolyline(roadNodes(edge), ARROW_SPACING, MAX_ARROWS_PER_EDGE)) {
        if (junctionHubs.some(h => distanceENU(point.x, point.y, h.x, h.y) < 26.0)) continue;
        for (const slot of slots) {
          const p = offsetPoint(point.x, point.y, point.heading, 0, slot.lateral);
          const arrowPitch = (slot.headingFlip ? -1 : 1) * (point.pitch || 0);
          addStraightArrow(layout, p.x, p.y, point.heading + slot.headingFlip, point.z || 0, arrowPitch);
        }
      }
    }
  }

  const lights = allEntities.filter(e => e && (e.type === 'tl' || e.type === 'traffic_light'));
  for (const light of lights) {
    const x = Number(light.stop_x ?? light.x) || 0;
    const y = Number(light.stop_y ?? light.y) || 0;
    const projection = nearestRoadPoint(edges, x, y);
    const edge = projection?.edge || edges[0] || {};
    const heading = projection?.heading || 0;
    const markZ = projection?.refZ || 0;
    const markPitch = projection?.pitch || 0;
    const lanes = Math.max(1, Number(edge.lanes) || 4);
    const laneWidth = Number(edge.lane_width) || 3.5;
    const isOneWay = edge.oneway === true;
    const laneDirection = isOneWay ? 0 : (projection?.lateral || -1) < 0 ? -1 : 1;
    const stopLineWidth = isOneWay ? lanes * laneWidth : lanes * laneWidth * 0.5;
    const stopLateral = isOneWay
      ? (hasLaneData ? 0 : -stopLineWidth * 0.5)
      : laneDirection * stopLineWidth * 0.5;
    const stopCenter = offsetPoint(
      projection?.refX ?? x,
      projection?.refY ?? y,
      heading,
      0,
      stopLateral,
    );
    addMark(layout, stopCenter.x, stopCenter.y, heading + Math.PI * 0.5, stopLineWidth, 0.38, markZ, 0);
    layout.stopLines++;
    const apprHeading = laneDirection > 0 ? heading + Math.PI : heading;
    if (light.crosswalk !== false) {
      const crossCenter = offsetPoint(stopCenter.x, stopCenter.y, apprHeading, 2.2, 0);
      addCrosswalk(layout, crossCenter, heading, stopLineWidth, markZ, markPitch);
    }
    // 左转待转区独立箭头：只要箭头落点（stopCenter 后方约 12m）不在交叉口核心铺装内，
    // 即便 stopCenter 距路口几何中心 < 35m 也应绘出左转导流箭头与直行↑组合标线。
    if (light.has_left_arrow) {
      const innerLat = (isOneWay && hasLaneData) ? (lanes * 0.5 - 0.5) * laneWidth : -0.5 * laneWidth;
      const arrowPt = offsetPoint(projection?.refX ?? x, projection?.refY ?? y, apprHeading, -12.0, innerLat);
      const insideHub = junctionHubs.some(
        (h) => distanceENU(arrowPt.x, arrowPt.y, h.x, h.y) < ((h.radius || 14.0) + 3.0)
      );
      if (!insideHub) {
        addLeftArrow(layout, arrowPt.x, arrowPt.y, apprHeading, markZ, laneDirection > 0 ? -markPitch : markPitch);
      }
    }
  }

  const parkingScene = !options.skipParkingBays && String(scenarioName).toLowerCase().includes('parking');
  const parkingFilter = edge => !isGarageOrRampEdge(edge) && !options.skipParkingBays && (parkingScene ||
    String(edge?.name || '').toLowerCase().includes('parking'));
  const parkingGroups = new Map();
  if (!options.skipParkingBays) for (const vehicle of allEntities) {
    if (!vehicle || !['car', 'suv', 'truck'].includes(vehicle.type) ||
        Math.abs(Number(vehicle.speed) || 0) >= 0.2) continue;
    const x = Number(vehicle.x) || 0, y = Number(vehicle.y) || 0;
    const projection = nearestRoadPoint(edges, x, y, parkingFilter);
    if (!projection) continue;
    const halfRoad = (Number(projection.edge.lanes) || 4) *
      (Number(projection.edge.lane_width) || 3.5) * 0.5;
    if (projection.distance > halfRoad + 4.0) continue;
    const laneBucket = Math.round(projection.lateral /
      (Number(projection.edge.lane_width) || 3.5));
    const key = `${projection.edgeIndex}:${laneBucket}`;
    if (!parkingGroups.has(key)) parkingGroups.set(key, []);
    parkingGroups.get(key).push({ vehicle, projection });
  }
  for (const parked of parkingGroups.values()) {
    parked.sort((a, b) => a.projection.s - b.projection.s);
    for (const item of parked) {
      const vehicle = item.vehicle;
      addParkingBay(layout, Number(vehicle.x) || 0, Number(vehicle.y) || 0,
        Number(vehicle.heading) || item.projection.heading, false);
      layout.parkingBays++;
    }
    for (let i = 1; i < parked.length; i++) {
      const a = parked[i - 1], b = parked[i];
      const gap = b.projection.s - a.projection.s;
      if (gap < 9 || gap > 32) continue;
      const av = a.vehicle, bv = b.vehicle;
      addParkingBay(layout,
        ((Number(av.x) || 0) + (Number(bv.x) || 0)) * 0.5,
        ((Number(av.y) || 0) + (Number(bv.y) || 0)) * 0.5,
        a.projection.heading, true);
      layout.parkingBays++;
    }
  }
  return layout;
}

export function createRoadFacilityView(scene) {
  const group = new THREE.Group();
  group.name = 'roadFacilities';
  scene.add(group);

  const staticGroup = new THREE.Group();
  staticGroup.name = 'staticFacilities';
  group.add(staticGroup);

  const parkingGoalGroup = new THREE.Group();
  parkingGoalGroup.name = 'parkingGoal';
  group.add(parkingGoalGroup);

  const frictionZonesGroup = new THREE.Group();
  frictionZonesGroup.name = 'frictionZones';
  group.add(frictionZonesGroup);

  const unknownStaticGroup = new THREE.Group();
  unknownStaticGroup.name = 'unknownStatics';
  group.add(unknownStaticGroup);

  let lastSignature = '';
  let lastParkingSig = '';
  let lastFrictionSig = '';
  let lastUnknownSig = '';
  let parkingPulseTime = 0;
  let parkingPulseMesh = null;
  let lastNow = 0;
  let stats = { marks: 0, poles: 0, parkingBays: 0, stopLines: 0, crosswalks: 0, arrows: 0 };

  function disposeGroup(targetGroup) {
    while (targetGroup.children.length) {
      const child = targetGroup.children[0];
      targetGroup.remove(child);
      if (child.children && child.children.length > 0) {
        disposeGroup(child);
      }
      child.geometry?.dispose?.();
      if (Array.isArray(child.material)) {
        child.material.forEach(m => m?.dispose?.());
      } else {
        child.material?.dispose?.();
      }
    }
  }

  function clear() {
    disposeGroup(staticGroup);
    disposeGroup(parkingGoalGroup);
    disposeGroup(frictionZonesGroup);
    disposeGroup(unknownStaticGroup);
    lastSignature = '';
    lastParkingSig = '';
    lastFrictionSig = '';
    lastUnknownSig = '';
    parkingPulseMesh = null;
  }

  function _drawBayAnchor(targetGroup, spec) {
    const {
      x, y, z = 0, heading = 0, width = 2.6, length = 5.6,
      colorHex = SCENE.parkingBox, withStopper = true, withPulse = false,
      openFrontOnly = false, withBayFixtures = false,
    } = spec;
    const anchor = new THREE.Group();
    const [ax, ay, az] = worldToThree(x, y, z + PAINT_Y + 0.005);
    anchor.position.set(ax, ay, az); // Coord.worldToThree
    anchor.rotation.y = headingToRotationY(heading);

    const boxMat = new THREE.MeshBasicMaterial({
      color: colorHex,
      transparent: true,
      opacity: withPulse ? 0.92 : 0.72,
      polygonOffset: true,
      polygonOffsetFactor: -3,
      polygonOffsetUnits: -6,
    });
    const sideGeom = new THREE.BoxGeometry(length, 0.012, LINE_W);
    const endGeom = new THREE.BoxGeometry(LINE_W, 0.012, width);

    const leftLine = new THREE.Mesh(sideGeom, boxMat);
    leftLine.position.set(0, 0, width * 0.5);
    anchor.add(leftLine);

    const rightLine = new THREE.Mesh(sideGeom, boxMat);
    rightLine.position.set(0, 0, -width * 0.5);
    anchor.add(rightLine);

    const rearLine = new THREE.Mesh(endGeom, boxMat);
    rearLine.position.set(-length * 0.5, 0, 0);
    anchor.add(rearLine);

    if (!openFrontOnly && withStopper) {
      const stopperGroup = new THREE.Group();
      const stopperLen = 0.16;
      const stopperH = 0.11;
      const stopperW = width * 0.72;
      const rubberMat = new THREE.MeshStandardMaterial({
        color: SCENE.stopperRubber,
        roughness: 0.85,
        metalness: 0.1,
      });
      const stopperBody = new THREE.Mesh(
        new THREE.BoxGeometry(stopperLen, stopperH, stopperW),
        rubberMat
      );
      stopperBody.position.set(0, stopperH * 0.5, 0);
      stopperGroup.add(stopperBody);

      const stripeMat = new THREE.MeshBasicMaterial({ color: SCENE.stopperStripe });
      const stripeGeom = new THREE.BoxGeometry(stopperLen + 0.004, stopperH + 0.004, 0.18);
      const stripeL = new THREE.Mesh(stripeGeom, stripeMat);
      stripeL.position.set(0, stopperH * 0.5, -stopperW * 0.25);
      const stripeR = new THREE.Mesh(stripeGeom, stripeMat);
      stripeR.position.set(0, stopperH * 0.5, stopperW * 0.25);
      stopperGroup.add(stripeL);
      stopperGroup.add(stripeR);

      stopperGroup.position.set(-length * 0.5 + 0.65, 0, 0);
      anchor.add(stopperGroup);

      if (withBayFixtures) {
        // Rear EV charger + corner protective bollards matching flowsim collider offsets
        const charger = new THREE.Group();
        const chMat = new THREE.MeshStandardMaterial({
          color: SCENE.evChargerBody ?? 0x26323e, roughness: 0.42, metalness: 0.35,
        });
        const ledMat = new THREE.MeshBasicMaterial({ color: SCENE.evChargerLed ?? 0x22e39a });
        const plinth = new THREE.Mesh(new THREE.BoxGeometry(0.54, 0.12, 0.44), rubberMat);
        plinth.position.set(0, 0.06, 0);
        charger.add(plinth);
        const tower = new THREE.Mesh(new THREE.BoxGeometry(0.42, 1.43, 0.34), chMat);
        tower.position.set(0, 0.12 + 0.715, 0);
        charger.add(tower);
        const led = new THREE.Mesh(new THREE.BoxGeometry(0.05, 0.24, 0.22), ledMat);
        led.position.set(0.20, 1.18, 0);
        charger.add(led);
        charger.position.set(-length * 0.5 - 0.55, 0, 0);
        anchor.add(charger);

        const bolBack = -length * 0.5 - 0.05;
        const bolLat = width * 0.5 + 0.30;
        const bolGeo = new THREE.CylinderGeometry(0.075, 0.085, 0.78, 10);
        const capGeo = new THREE.CylinderGeometry(0.088, 0.088, 0.10, 10);
        for (const side of [-1, 1]) {
          const bolGroup = new THREE.Group();
          const post = new THREE.Mesh(bolGeo, stripeMat);
          post.position.set(0, 0.39, 0);
          bolGroup.add(post);
          const cap = new THREE.Mesh(capGeo, rubberMat);
          cap.position.set(0, 0.73, 0);
          bolGroup.add(cap);
          bolGroup.position.set(bolBack, 0, side * bolLat);
          anchor.add(bolGroup);
        }
      }
    }

    if (withPulse) {
      const pulseGeom = new THREE.BoxGeometry(length * 0.96, 0.004, width * 0.94);
      const pulseMat = new THREE.MeshBasicMaterial({
        color: colorHex,
        transparent: true,
        opacity: 0.28,
        depthWrite: false,
        polygonOffset: true,
        polygonOffsetFactor: -3,
        polygonOffsetUnits: -6,
      });
      parkingPulseMesh = new THREE.Mesh(pulseGeom, pulseMat);
      parkingPulseMesh.position.set(0, 0.002, 0);
      anchor.add(parkingPulseMesh);
    }

    targetGroup.add(anchor);
  }

  function _syncParkingGoal(store, dt = 0.016) {
    const goal = store.parkingGoal;
    const parkOut = store.parkOut;
    const slots = Array.isArray(store.parkingSlots) ? store.parkingSlots : [];
    const hasGoal = Boolean(goal && goal.enabled);
    const hasParkOut = Boolean(parkOut && parkOut.enabled);

    if (!hasGoal && !hasParkOut && !slots.length) {
      parkingGoalGroup.visible = false;
      return;
    }
    parkingGoalGroup.visible = true;

    const staticCars = (Array.isArray(store.entities) ? store.entities : []).filter((e) =>
      e && ['car', 'suv', 'truck'].includes(e.type) && Math.abs(Number(e.speed) || 0) < 0.45);
    const pd = store.planningDebug || null;
    let selectedIdx = (pd && Number.isFinite(pd.selected_slot_index)) ? Number(pd.selected_slot_index) : -1;

    const slotStates = slots.map((s) => {
      const sx = Number(s.x) || 0;
      const sy = Number(s.y) || 0;
      const sz = Number(s.z) || 0;
      const occupied = Boolean(s.occupied) || staticCars.some((c) =>
        Math.hypot((Number(c.x) || 0) - sx, (Number(c.y) || 0) - sy) < 2.2
        && Math.abs((Number(c.z) || 0) - sz) < 2.0);
      return {
        x: sx,
        y: sy,
        z: sz,
        heading: Number(s.heading) || 0,
        width: Number(s.width) || 2.8,
        length: Number(s.length) || 6.0,
        occupied,
      };
    });

    if (slotStates.length && (selectedIdx < 0 || selectedIdx >= slotStates.length || slotStates[selectedIdx].occupied)) {
      const goalIdx = hasGoal ? slotStates.findIndex((s) =>
        !s.occupied && Math.hypot(s.x - (Number(goal.x) || 0), s.y - (Number(goal.y) || 0)) < 1.5) : -1;
      selectedIdx = goalIdx >= 0 ? goalIdx : slotStates.findIndex((s) => !s.occupied);
    }

    const goalSig = hasGoal
      ? `${(Number(goal.x) || 0).toFixed(2)}:${(Number(goal.y) || 0).toFixed(2)}:${(Number(goal.z) || 0).toFixed(2)}:${(Number(goal.heading) || 0).toFixed(3)}`
      : 'nog';
    const poSig = hasParkOut
      ? `${(Number(parkOut.slot_x) || 0).toFixed(2)}:${(Number(parkOut.slot_y) || 0).toFixed(2)}:${(Number(parkOut.slot_z) || 0).toFixed(2)}`
      : 'nopo';
    const slotsSig = slotStates.map((s) => `${s.x.toFixed(1)},${s.y.toFixed(1)},${s.z.toFixed(1)},${s.occupied ? 1 : 0}`).join('|');
    const sig = `${goalSig}#${poSig}#${selectedIdx}#${slotsSig}`;

    if (sig !== lastParkingSig) {
      lastParkingSig = sig;
      disposeGroup(parkingGoalGroup);
      parkingPulseMesh = null;

      if (hasParkOut) {
        _drawBayAnchor(parkingGoalGroup, {
          x: Number(parkOut.slot_x) || 0,
          y: Number(parkOut.slot_y) || 0,
          z: Number(parkOut.slot_z) || 0,
          heading: Number(parkOut.slot_heading) || 0,
          width: 2.8,
          length: 5.8,
          colorHex: SCENE.parkOutBox,
          withStopper: false,
          withPulse: false,
          openFrontOnly: true,
        });
      }

      if (slotStates.length) {
        slotStates.forEach((s, idx) => {
          const isSel = idx === selectedIdx;
          _drawBayAnchor(parkingGoalGroup, {
            ...s,
            colorHex: isSel ? SCENE.slotSelected : (s.occupied ? SCENE.slotOccupied : SCENE.slotCandidate),
            withStopper: true,
            withPulse: isSel,
          });
        });
      } else if (hasGoal) {
        _drawBayAnchor(parkingGoalGroup, {
          x: Number(goal.x) || 0,
          y: Number(goal.y) || 0,
          z: Number(goal.z) || 0,
          heading: Number(goal.heading) || 0,
          width: Number(goal.width) || 2.4,
          length: Number(goal.length) || 5.3,
          colorHex: SCENE.parkingBox,
          withStopper: true,
          withPulse: true,
          withBayFixtures: Math.abs(Number(goal.z) || 0) < 1.2,
        });
      }
    }

    parkingPulseTime += dt;
    if (parkingPulseMesh && parkingPulseMesh.material) {
      const pulse = 0.22 + 0.14 * Math.sin(parkingPulseTime * 3.2); // exempt: breathing pulse animation
      parkingPulseMesh.material.opacity = pulse;
    }
  }

  function _syncFrictionZones(store) {
    const zones = Array.isArray(store.frictionZones) ? store.frictionZones : [];
    if (zones.length === 0) {
      frictionZonesGroup.visible = false;
      return;
    }
    frictionZonesGroup.visible = true;

    const sig = zones.map(z =>
      `${z.x_min}:${z.x_max}:${z.y_min}:${z.y_max}:${z.surface || 'wet'}`).join(';');
    if (sig === lastFrictionSig) return;
    lastFrictionSig = sig;
    disposeGroup(frictionZonesGroup);

    zones.forEach((z) => {
      const xMin = Number(z.x_min) || 0;
      const xMax = Number(z.x_max) || 0;
      const yMin = Number(z.y_min) || 0;
      const yMax = Number(z.y_max) || 0;
      const cx = (xMin + xMax) * 0.5;
      const cy = (yMin + yMax) * 0.5;
      const lenX = Math.max(0.1, Math.abs(xMax - xMin));
      const lenY = Math.max(0.1, Math.abs(yMax - yMin));

      const isIce = (z.surface === 'ice');
      const mat = new THREE.MeshStandardMaterial({
        color: isIce ? SCENE.iceSurface : SCENE.wetSurface,
        transparent: true,
        opacity: isIce ? 0.65 : 0.55,
        roughness: isIce ? 0.10 : 0.16,
        metalness: isIce ? 0.20 : 0.08,
        polygonOffset: true,
        polygonOffsetFactor: -3,
        polygonOffsetUnits: -6,
      });
      const geom = new THREE.BoxGeometry(lenX, 0.008, lenY);
      const mesh = new THREE.Mesh(geom, mat);
      const [tx, ty, tz] = worldToThree(cx, cy, 0.145);
      mesh.position.set(tx, ty, tz); // Coord.worldToThree
      frictionZonesGroup.add(mesh);
    });
  }

  function _syncUnknownStatics(store) {
    const KNOWN_DYNAMIC_OR_INFRA = new Set([
      'ego', 'car', 'suv', 'truck', 'bus', 'van', 'pedestrian',
      'cyclist', 'bicycle', 'tl', 'traffic_light', 'etc_gate',
    ]);
    const list = (Array.isArray(store.entities) ? store.entities : []).filter(
      (e) => e && e.type && !KNOWN_DYNAMIC_OR_INFRA.has(e.type),
    );
    const sig = list.map((e) =>
      `${e.id ?? ''}:${e.type}:${Number(e.x || 0).toFixed(2)},${Number(e.y || 0).toFixed(2)},${Number(e.z || 0).toFixed(2)}`,
    ).join('|');
    if (sig === lastUnknownSig) return;
    lastUnknownSig = sig;
    disposeGroup(unknownStaticGroup);
    if (!list.length) return;
    const bodyMat = new THREE.MeshStandardMaterial({
      color: 0x3b4252, roughness: 0.6, metalness: 0.2,
    });
    const hazardMat = new THREE.MeshBasicMaterial({ color: 0xff8c1a });
    for (const ent of list) {
      const len = Math.max(0.15, Number(ent.length) || 0.8);
      const wid = Math.max(0.15, Number(ent.width) || 0.8);
      const hgt = Math.max(0.15, Number(ent.height) || 1.0);
      const gz = Number.isFinite(Number(ent.z)) ? Number(ent.z) : 0;
      const [px, py, pz] = worldToThree(Number(ent.x) || 0, Number(ent.y) || 0, gz);
      const crate = new THREE.Group();
      crate.position.set(px, py, pz); // Coord.worldToThree
      crate.rotation.y = headingToRotationY(Number(ent.heading) || 0);
      const box = new THREE.Mesh(new THREE.BoxGeometry(len, hgt * 0.82, wid), bodyMat);
      box.position.set(0, hgt * 0.41, 0);
      crate.add(box);
      const cap = new THREE.Mesh(new THREE.BoxGeometry(len * 1.02, hgt * 0.18, wid * 1.02), hazardMat);
      cap.position.set(0, hgt * 0.91, 0);
      crate.add(cap);
      unknownStaticGroup.add(crate);
    }
  }

  function update(store, now = 0) {
    const dt = lastNow ? Math.min(0.1, Math.max(0.001, (now - lastNow) / 1000)) : 0.016;
    lastNow = now;

    // 1. 同步泊车目标与路面摩擦系数区域
    _syncParkingGoal(store, dt);
    _syncFrictionZones(store);
    _syncUnknownStatics(store);

    // 2. 静态标线与设施
    const entities = store.entities || [];
    const fixed = entities.filter(e =>
      e && (e.type === 'tl' || e.type === 'traffic_light' ||
        (['car', 'suv', 'truck'].includes(e.type) && Math.abs(e.speed || 0) < 0.2)));
    const signature = `${store.scenarioName || ''}|${store.roadHash || ''}|` + fixed.map(e =>
      `${e.type}:${e.id}:${Math.round(e.x || 0)}:${Math.round(e.y || 0)}:` +
      `${Math.round(e.stop_x || 0)}:${Math.round(e.stop_y || 0)}:` +
      `${Math.round((e.heading || 0) * 10)}`).join(',');
    if (signature === lastSignature) return;
    lastSignature = signature;
    disposeGroup(staticGroup);

    const hasExplicitSlots = Boolean((store.parkOut && store.parkOut.enabled)
      || (Array.isArray(store.parkingSlots) && store.parkingSlots.length > 0));
    const layout = inferRoadFacilities(
      store.roadNetwork, fixed, store.scenarioName, { skipParkingBays: hasExplicitSlots }
    );
    stats = {
      marks: layout.marks.length,
      poles: layout.poles.length,
      parkingBays: layout.parkingBays,
      stopLines: layout.stopLines,
      crosswalks: layout.crosswalks,
      arrows: layout.arrows,
    };
    if (layout.marks.length) {
      const geometry = new THREE.BoxGeometry(1, PAINT_H, 1);
      const material = new THREE.MeshStandardMaterial({
        color: 0xf5f5e8, roughness: 0.65, metalness: 0,
        polygonOffset: true, polygonOffsetFactor: -2.5, polygonOffsetUnits: -5,
      });
      const mesh = new THREE.InstancedMesh(geometry, material, layout.marks.length);
      const dummy = new THREE.Object3D();
      layout.marks.forEach((mark, index) => {
        const [x, y, z] = worldToThree(mark.x, mark.y, (mark.z || 0) + PAINT_Y);
        dummy.position.set(x, y, z); // Coord.worldToThree
        dummy.rotation.order = 'YXZ';
        dummy.rotation.set(0, headingToRotationY(mark.heading), mark.pitch || 0);
        dummy.scale.set(mark.length, 1, mark.width);
        dummy.updateMatrix();
        mesh.setMatrixAt(index, dummy.matrix);
      });
      mesh.instanceMatrix.needsUpdate = true;
      mesh.receiveShadow = true;
      staticGroup.add(mesh);
    }

    if (layout.poles.length) {
      const geometry = new THREE.CylinderGeometry(0.05, 0.07, 0.8, 8);
      const material = new THREE.MeshStandardMaterial({
        color: 0xff6b00, roughness: 0.45, metalness: 0.1,
      });
      const mesh = new THREE.InstancedMesh(geometry, material, layout.poles.length);
      const dummy = new THREE.Object3D();
      layout.poles.forEach((pole, index) => {
        const [x, y, z] = worldToThree(pole.x, pole.y, (pole.z || 0) + 0.4);
        dummy.position.set(x, y, z); // Coord.worldToThree
        dummy.updateMatrix();
        mesh.setMatrixAt(index, dummy.matrix);
      });
      mesh.instanceMatrix.needsUpdate = true;
      staticGroup.add(mesh);
    }
  }

  return { update, clear, getGroup: () => group, getStats: () => ({ ...stats }) };
}
