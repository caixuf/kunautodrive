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

const PAINT_Y = 0.155;
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
    ? { x: Number(node[0]) || 0, y: Number(node[1]) || 0 }
    : { x: Number(node?.x) || 0, y: Number(node?.y) || 0 });
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
    while (next <= accumulated + segment && out.length < limit) {
      const t = (next - accumulated) / segment;
      out.push({
        x: a.x + (b.x - a.x) * t,
        y: a.y + (b.y - a.y) * t,
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
      const distance = distanceENU(x, y, refX, refY);
      const heading = headingBetweenPoints(a.x, a.y, b.x, b.y);
      const [fx, fy] = forwardENU(heading);
      const [nx, ny] = tangentToNormal(fx, fy);
      if (!best || distance < best.distance) {
        best = {
          edge, edgeIndex, distance, heading,
          refX, refY,
          s: edgeS + segmentLength * t,
          lateral: (x - refX) * nx + (y - refY) * ny,
        };
      }
      edgeS += segmentLength;
    }
  });
  return best;
}

function addMark(layout, x, y, heading, length, width = LINE_W) {
  layout.marks.push({ x, y, heading, length, width });
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

function addCrosswalk(layout, center, heading, width) {
  /* GB 5768.3 5.8：斑马线条纹平行于道路中心线（与车同向），沿道路长度短
   * （交叉口 ≥2m / 路段 ≥3m）；跨道路方向铺多条条带。旧实现条纹长轴垂直
   * 道路（横跨半幅路面）→ 方向反了。 */
  const step = CROSSWALK_STRIPE_W + CROSSWALK_GAP;
  const count = Math.max(2, Math.round(width / step));
  for (let i = 0; i < count; i++) {
    const across = (i - (count - 1) / 2) * step;
    const p = offsetPoint(center.x, center.y, heading + Math.PI * 0.5, across, 0);
    addMark(layout, p.x, p.y, heading, CROSSWALK_LENGTH, CROSSWALK_STRIPE_W);
  }
  layout.crosswalks++;
}

export function inferRoadFacilities(roadNetwork, entities, scenarioName = '') {
  const layout = {
    marks: [], poles: [], parkingBays: 0, stopLines: 0, crosswalks: 0, arrows: 0,
  };
  const edges = Array.isArray(roadNetwork?.edges) ? roadNetwork.edges : [];
  const allEntities = Array.isArray(entities) ? entities : [];

  for (const edge of edges) {
    const name = String(edge?.name || '').toLowerCase();
    const type = String(edge?.type || '').toLowerCase();
    if (!(name.includes('exam') || name.includes('parking') || type === 'urban')) continue;
    for (const point of samplePolyline(roadNodes(edge), ARROW_SPACING, MAX_ARROWS_PER_EDGE)) {
      const stem = offsetPoint(point.x, point.y, point.heading, -0.8, 0);
      addMark(layout, stem.x, stem.y, point.heading, 2.8, 0.28);
      for (const side of [-1, 1]) {
        const head = offsetPoint(point.x, point.y, point.heading, 0.8, side * 0.45);
        addMark(layout, head.x, head.y,
          point.heading + side * Math.PI * 0.25, 1.5, 0.28);
      }
      layout.arrows++;
    }
  }

  const lights = allEntities.filter(e => e && (e.type === 'tl' || e.type === 'traffic_light'));
  for (const light of lights) {
    const x = Number(light.stop_x ?? light.x) || 0;
    const y = Number(light.stop_y ?? light.y) || 0;
    const projection = nearestRoadPoint(edges, x, y);
    const edge = projection?.edge || edges[0] || {};
    const heading = projection?.heading || 0;
    const lanes = Math.max(1, Number(edge.lanes) || 4);
    const laneWidth = Number(edge.lane_width) || 3.5;
    const isOneWay = edge.oneway === true;
    const laneDirection = isOneWay ? 0 : (projection?.lateral || -1) < 0 ? -1 : 1;
    const stopLineWidth = isOneWay ? lanes * laneWidth : lanes * laneWidth * 0.5;
    const stopCenter = offsetPoint(
      projection?.refX ?? x,
      projection?.refY ?? y,
      heading,
      0,
      isOneWay ? 0 : laneDirection * stopLineWidth * 0.5,
    );
    // A stop line is one solid bar across the approaching carriageway. The
    // old repeated stripes resembled an incorrectly positioned crosswalk.
    addMark(layout, stopCenter.x, stopCenter.y, heading + Math.PI * 0.5, stopLineWidth, 0.38);
    layout.stopLines++;
    if (light.crosswalk !== false) {
      addCrosswalk(layout, stopCenter, heading, stopLineWidth);
    }
  }

  const parkingScene = String(scenarioName).toLowerCase().includes('parking');
  const parkingFilter = edge => parkingScene ||
    String(edge?.name || '').toLowerCase().includes('parking');
  const parkingGroups = new Map();
  for (const vehicle of allEntities) {
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
  let lastSignature = '';
  let stats = { marks: 0, poles: 0, parkingBays: 0, stopLines: 0, crosswalks: 0, arrows: 0 };

  function clear() {
    while (group.children.length) {
      const child = group.children[0];
      group.remove(child);
      child.geometry?.dispose?.();
      child.material?.dispose?.();
    }
  }

  function update(store) {
    const entities = store.entities || [];
    const fixed = entities.filter(e =>
      e && (e.type === 'tl' || e.type === 'traffic_light' ||
        (['car', 'suv', 'truck'].includes(e.type) && Math.abs(e.speed || 0) < 0.2)));
    const signature = `${store.scenarioName || ''}|${store.roadHash || ''}|` + fixed.map(e =>
      `${e.type}:${e.id}:${Math.round((e.x || 0) * 10)}:${Math.round((e.y || 0) * 10)}:` +
      `${Math.round((e.stop_x || 0) * 10)}:${Math.round((e.stop_y || 0) * 10)}:` +
      `${Math.round((e.heading || 0) * 100)}`).join(',');
    if (signature === lastSignature) return;
    lastSignature = signature;
    clear();

    const layout = inferRoadFacilities(store.roadNetwork, fixed, store.scenarioName);
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
      });
      const mesh = new THREE.InstancedMesh(geometry, material, layout.marks.length);
      const dummy = new THREE.Object3D();
      layout.marks.forEach((mark, index) => {
        const [x, y, z] = worldToThree(mark.x, mark.y, PAINT_Y);
        dummy.position.set(x, y, z); // Coord.worldToThree
        dummy.rotation.y = headingToRotationY(mark.heading);
        dummy.scale.set(mark.length, 1, mark.width);
        dummy.updateMatrix();
        mesh.setMatrixAt(index, dummy.matrix);
      });
      mesh.instanceMatrix.needsUpdate = true;
      mesh.receiveShadow = true;
      group.add(mesh);
    }

    if (layout.poles.length) {
      const geometry = new THREE.CylinderGeometry(0.05, 0.07, 0.8, 8);
      const material = new THREE.MeshStandardMaterial({
        color: 0xff6b00, roughness: 0.45, metalness: 0.1,
      });
      const mesh = new THREE.InstancedMesh(geometry, material, layout.poles.length);
      const dummy = new THREE.Object3D();
      layout.poles.forEach((pole, index) => {
        const [x, y, z] = worldToThree(pole.x, pole.y, 0.4);
        dummy.position.set(x, y, z); // Coord.worldToThree
        dummy.updateMatrix();
        mesh.setMatrixAt(index, dummy.matrix);
      });
      mesh.instanceMatrix.needsUpdate = true;
      group.add(mesh);
    }
  }

  return { update, clear, getGroup: () => group, getStats: () => ({ ...stats }) };
}
