/**
 * StreetlightView.js — 路灯（普通道路场景）
 *
 * Step 4 重构：新增模块。Phase 3 View 清单 9 个中补的第 8 个。
 *
 * 仿真侧 scene_pub.cpp 只发道路几何（edges/nodes），不发路灯数据 —— 必须
 * 3D 层从 roadNetwork.edges 自动布局。
 *
 * 设计：
 *   - 4 个 InstancedMesh：pole / arm / head / glow（共 4 draw call）
 *   - LAMP_SPACING = 40m，道路两侧交替放置
 *   - offset = halfWidth + 1.5m（路灯在路肩外）
 *   - 跳过 type='viaduct_highway' 的 edge（高架场景路灯由 ViaductView 内置）
 *
 * 坐标约定：
 *   edge.nodes 经 sampleEdgeNodes 采样后输出 [x, y_up, z_north, ...]
 *   （ENU→THREE 交换由 sampleEdgeNodes 内部完成，详见 Curve.js）
 *   所以这里直接用 p.x / p.z，不要再调 worldToThree。
 */

import { getStdMaterial, createEmissiveMaterial } from '../core/AssetFactory.js';
import { EDGE_TYPE } from '../core/Constants.js';
import { directionToRotationY } from '../math/Coord.js';
import { computeEdgeAxis } from '../model/RoadAxis.js';
import { getTopology } from '../model/TopologyModel.js';

const LAMP_SPACING = 40;   // 路灯间距（米）
const LAMP_OFFSET  = 1.5;  // 路灯距路缘外距离（米）
const POLE_H       = 7.0;  // 杆高
const ARM_LEN      = 1.8;  // 悬臂长度
const HEAD_SIZE    = 0.4;  // 灯头尺寸

const COLOR_POLE  = 0x3a3a3a;
const COLOR_HEAD  = 0xfff4d6;
const COLOR_GLOW  = 0xfff4d6;

export function createStreetlightView(scene) {
  let group = new THREE.Group();
  scene.add(group);

  /** InstancedMesh 池（build 时一次性创建，clear 时 dispose） */
  let poleMesh, armMesh, headMesh, glowMesh;

  function clear() {
    while (group.children.length) {
      const c = group.children[0];
      group.remove(c);
      if (c.geometry) c.geometry.dispose();
      if (c.material) {
        if (Array.isArray(c.material)) c.material.forEach(m => m.dispose());
        else c.material.dispose();
      }
    }
    poleMesh = armMesh = headMesh = glowMesh = null;
  }

  function build(roadNetwork) {
    clear();
    if (!roadNetwork || !roadNetwork.edges || roadNetwork.edges.length === 0) return;

    /* P0 路口避让（TopologyModel 单一事实源）：路灯立在路口内/边缘会挡道且
     * 视觉上与路口渠化冲突。距路口中心 < radius + 2m 的槽位丢弃（与 TreeView 同 margin）。 */
    const topo = getTopology(roadNetwork);

    // ── 第一遍：收集所有有效路轴供碰撞检测 ──
    const allAxes = [];
    for (const edge of roadNetwork.edges) {
      if (edge.type === EDGE_TYPE.VIADUCT_HIGHWAY || edge.name === EDGE_TYPE.VIADUCT_HIGHWAY) continue;
      const axis = computeEdgeAxis(edge, roadNetwork.lane_data);
      if (axis.ok && axis.spine.length >= 2) {
        for (let i = 0; i < axis.spine.length; i++) axis.spine[i].cum = axis.cum[i];
        let minX = Infinity, maxX = -Infinity, minZ = Infinity, maxZ = -Infinity;
        for (const p of axis.spine) {
          if (p.px < minX) minX = p.px;
          if (p.px > maxX) maxX = p.px;
          if (p.pz < minZ) minZ = p.pz;
          if (p.pz > maxZ) maxZ = p.pz;
        }
        allAxes.push({
          edge,
          axis,
          halfW: axis.halfWidth,
          isOneWay: edge.oneway === true,
          minX, maxX, minZ, maxZ,
        });
      }
    }

    /** 空间几何碰撞检测：判断点 (x, z, py) 是否落在任何同层路段（或本体弯道内侧）的沥青车道或安全外缘内 */
    function isInsideAnyRoad(x, z, py) {
      for (const item of allAxes) {
        const hw = item.halfW + 0.15; // 道路边缘 0.15m 安全红线
        // AABB 粗筛：1 个指令跳过 99.9% 远端不相交道路，瞬时完成万级大图碰撞计算
        if (x < item.minX - hw || x > item.maxX + hw || z < item.minZ - hw || z > item.maxZ + hw) {
          continue;
        }
        const spine = item.axis.spine;
        for (let k = 0; k < spine.length - 1; k++) {
          const p1 = spine[k], p2 = spine[k + 1];
          // 若纵向高程差异 >= 2.5m（立体交叉上跨/下穿），不在同一空间水平面
          const segPy = ((p1.py || 0) + (p2.py || 0)) * 0.5;
          if (Math.abs(py - segPy) >= 2.5) continue;

          const dx = p2.px - p1.px, dz = p2.pz - p1.pz;
          const len2 = dx * dx + dz * dz;
          if (len2 < 1e-4) continue;
          const t = Math.max(0, Math.min(1, ((x - p1.px) * dx + (z - p1.pz) * dz) / len2));
          const projX = p1.px + dx * t;
          const projZ = p1.pz + dz * t;
          const dist2 = (x - projX) * (x - projX) + (z - projZ) * (z - projZ);
          if (dist2 < hw * hw) return true; // 落在任何道路沥青路面内
        }
      }
      return false;
    }

    // ── 第二遍：收集所有合法路灯放置点 ──
    const slots = [];  // [{x, z, nx, nz, side: +1|-1, py}]
    for (const item of allAxes) {
      const { edge, axis, halfW, isOneWay } = item;
      const spine = axis.spine;
      const totalLen = axis.cum[axis.cum.length - 1];

      // 短接驳段/路口弯道（< 28m）不布设路灯，避免堵在路口转角或三角分流岛
      if (totalLen < 28) continue;

      const count = Math.floor(totalLen / LAMP_SPACING);
      if (count === 0) continue;

      for (let i = 0; i < count; i++) {
        const targetArc = (i + 0.5) * LAMP_SPACING;
        let j = 1;
        while (j < spine.length && spine[j].cum < targetArc) j++;
        if (j >= spine.length) j = spine.length - 1;
        const s = spine[j];
        const py = s.py || 0;

        // 单行道/分幅道路：始终布设在右侧路肩（side = -1），严禁插在左侧车道隔离带与对向车道之间
        // 双向道路：优先交替，但若左侧与其他车道重叠，自动回退到右侧
        let side = isOneWay ? -1 : (i % 2 === 0 ? 1 : -1);
        let x = s.px + s.nx * (halfW + LAMP_OFFSET) * side;
        let z = s.pz + s.nz * (halfW + LAMP_OFFSET) * side;

        // 若选定的位置侵入了相邻其他道路或弯道内侧路面
        if (isInsideAnyRoad(x, z, py)) {
          if (!isOneWay && side === 1) {
            // 尝试换到右侧
            side = -1;
            x = s.px + s.nx * (halfW + LAMP_OFFSET) * side;
            z = s.pz + s.nz * (halfW + LAMP_OFFSET) * side;
            if (isInsideAnyRoad(x, z, py)) continue; // 两侧均受阻则放弃
          } else {
            continue; // 单行道右侧仍受阻则丢弃
          }
        }

        // 路口红线避让（距路口边界 < 3.0m 丢弃）
        if (topo && topo.nearJunction(x, z, 3.0)) continue;

        slots.push({ x, z, nx: s.nx, nz: s.nz, side, py });
      }
    }

    if (slots.length === 0) return;

    // ── 第二遍：构建 4 个 InstancedMesh ──
    const N = slots.length;
    const poleGeo = new THREE.CylinderGeometry(0.08, 0.12, POLE_H, 8);
    const armGeo  = new THREE.BoxGeometry(ARM_LEN, 0.08, 0.08);
    const headGeo = new THREE.BoxGeometry(HEAD_SIZE, HEAD_SIZE * 0.5, HEAD_SIZE * 1.5);
    const glowGeo = new THREE.SphereGeometry(HEAD_SIZE * 0.6, 8, 6);

    const poleMat = getStdMaterial(COLOR_POLE, 0.6, 0.3);
    const armMat  = getStdMaterial(COLOR_POLE, 0.6, 0.3);
    const headMat = getStdMaterial(COLOR_HEAD, 0.3, 0.6);
    const glowMat = createEmissiveMaterial(COLOR_GLOW, 1.2);

    poleMesh = new THREE.InstancedMesh(poleGeo, poleMat, N);
    poleMesh.name = 'streetlight_pole';
    armMesh  = new THREE.InstancedMesh(armGeo, armMat, N);
    armMesh.name = 'streetlight_arm';
    headMesh = new THREE.InstancedMesh(headGeo, headMat, N);
    headMesh.name = 'streetlight_head';
    glowMesh = new THREE.InstancedMesh(glowGeo, glowMat, N);
    glowMesh.name = 'streetlight_glow';

    const dummy = new THREE.Object3D();
    for (let i = 0; i < N; i++) {
      const s = slots[i];
      // ── 杆：竖直立在路面高程（s.py，高架/引道路灯随路抬升）──
      // 注意：sampleEdgeNodes 已做 ENU→THREE 交换，p.x / p.z 已是 THREE 坐标，
      // 不要再调 worldToThree（会双交换让灯钻地底）
      dummy.position.set(s.x, s.py + POLE_H / 2, s.z);
      dummy.rotation.set(0, 0, 0);
      dummy.updateMatrix();
      poleMesh.setMatrixAt(i, dummy.matrix);

      // ── 悬臂：从杆顶向道路方向延伸 ──
      // 朝向道路中心 = -法线方向（路灯在路肩外，悬臂伸回路上方）
      const dirX = -s.nx * s.side;
      const dirZ = -s.nz * s.side;
      const armRotY = directionToRotationY(dirX, dirZ);  // 旋转使 BoxGeometry 的 +X 朝向 dir
      dummy.position.set(s.x + dirX * ARM_LEN / 2, s.py + POLE_H, s.z + dirZ * ARM_LEN / 2);
      dummy.rotation.set(0, armRotY, 0);
      dummy.updateMatrix();
      armMesh.setMatrixAt(i, dummy.matrix);

      // ── 灯头：在悬臂末端，朝下照路 ──
      dummy.position.set(s.x + dirX * ARM_LEN, s.py + POLE_H - 0.15, s.z + dirZ * ARM_LEN);
      dummy.rotation.set(0, armRotY, 0);
      dummy.updateMatrix();
      headMesh.setMatrixAt(i, dummy.matrix);

      // ── 灯光辉光：在灯头位置 ──
      dummy.position.set(s.x + dirX * ARM_LEN, s.py + POLE_H - 0.2, s.z + dirZ * ARM_LEN);
      dummy.rotation.set(0, 0, 0);
      dummy.updateMatrix();
      glowMesh.setMatrixAt(i, dummy.matrix);
    }
    poleMesh.instanceMatrix.needsUpdate = true;
    armMesh.instanceMatrix.needsUpdate = true;
    headMesh.instanceMatrix.needsUpdate = true;
    glowMesh.instanceMatrix.needsUpdate = true;

    group.add(poleMesh, armMesh, headMesh, glowMesh);
  }

  function getGroup() { return group; }

  return { build, clear, getGroup };
}
