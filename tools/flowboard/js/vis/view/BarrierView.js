/**
 * BarrierView.js — 护栏（普通道路场景）
 *
 * Step 4 重构：新增模块。Phase 3 View 清单 9 个中补的第 9 个。
 *
 * 仿真侧 scene_pub.cpp 只发道路几何，不发护栏数据 —— 必须 3D 层
 * 从 roadNetwork.edges 自动布局。
 *
 * 设计：
 *   - 3 个 InstancedMesh：1 个 post + 2 个 beam 层（上下横梁）
 *   - POST_SPACING = 3m，道路两侧对称放置
 *   - offset = halfWidth + 0.5m（紧贴路肩）
 *   - 跳过 type='viaduct_highway'（高架护栏由 ViaductView 内置）
 *
 * 坐标约定：
 *   edge.nodes 经 sampleEdgeNodes 采样后输出 [x, y_up, z_north, ...]
 *   （ENU→THREE 交换由 sampleEdgeNodes 内部完成，详见 Curve.js）
 *   所以这里直接用 p.x / p.z，不要再调 worldToThree。
 */

import { getStdMaterial } from '../core/AssetFactory.js';
import { EDGE_TYPE } from '../core/Constants.js';
import { directionToRotationY } from '../math/Coord.js';
import { computeEdgeAxis } from '../model/RoadAxis.js';
import { getTopology } from '../model/TopologyModel.js';

/* 波形梁护栏观感（2026-08-14 升级）：旧版 0.18m 双板间距 0.45m（0.75/0.30），
 * 远看像两条互不相干的漂浮白条；立柱 0.08m 细得不可见。按真实 W 板收敛：
 * 板顶 ~0.71m，双板 0.66/0.46 收紧模拟 W 阴影缝，立柱 2m 间距读作连续护栏。 */
const POST_SPACING   = 2.0;  // 立柱间距（米）
const POST_OFFSET    = 0.5;  // 护栏距路缘外距离（米）
const POST_H         = 0.75; // 立柱高度
const POST_W         = 0.13; // 立柱截面（沿道路）
const BEAM_THICKNESS = 0.05; // 横梁厚度
const BEAM_W         = 0.13; // 横梁高度（单波）
const BEAM_UPPER_Y   = 0.66; // 上波中心高度
const BEAM_LOWER_Y   = 0.46; // 下波中心高度
const SEGMENT_LEN    = 2.0;  // 横梁分段长度（与立柱间距对齐）

const COLOR_POST = 0x8a9095;   // 镀锌钢立柱（略暗）
const COLOR_BEAM = 0x9aa0a4;   // 镀锌钢波形板（metalness 0.85 金属光泽）

export function createBarrierView(scene) {
  let group = new THREE.Group();
  scene.add(group);

  let postMesh, upperBeamMesh, lowerBeamMesh;

  function clear() {
    while (group.children.length) {
      const c = group.children[0];
      group.remove(c);
      if (c.geometry) c.geometry.dispose();
      if (c.material) c.material.dispose();
    }
    postMesh = upperBeamMesh = lowerBeamMesh = null;
  }

  function build(roadNetwork) {
    clear();
    if (!roadNetwork || !roadNetwork.edges || roadNetwork.edges.length === 0) return;

    /* P0（2026-08-14）：拓扑单一事实源。旧版护栏从 edge 首通铺到尾、毫无路口
     * 感知——路口/弯道接缝处多条 edge 的护栏互相穿插成圈（用户报障"栏杆在弯道
     * 地方交叉圆圈"）。现在 spine 点落在路口圆内即断链，护栏在路口边界终止。 */
    const topo = getTopology(roadNetwork);

    const posts = [];
    const upperBeamSegs = [];
    const lowerBeamSegs = [];
    for (const edge of roadNetwork.edges) {
      if (edge.type === EDGE_TYPE.VIADUCT_HIGHWAY || edge.name === EDGE_TYPE.VIADUCT_HIGHWAY) continue;
      /* 只有高速（highway）才需要金属护栏。城市道路（urban）有路缘石 +
       * 人行道隔离，加金属护栏既不符合真实城市（路口护栏会四向交叉重叠），
       * 也让十字路口视觉一团糟。 */
      if (edge.type !== EDGE_TYPE.HIGHWAY && String(edge.name || '').toLowerCase().indexOf('highway') === -1) continue;

      /* 共享路轴（单一事实源）：road.centerline 是最左车道左缘，须由 computeEdgeAxis
       * 推导 TRUE 中心 spine + 车道组半宽。护栏相对 TRUE 中心偏移，才不会压路 /
       * 与 RoadView 错位。lane_data 缺失时 fromLanes=false、居中回退（零回归）。 */
      const axis = computeEdgeAxis(edge, roadNetwork.lane_data);
      if (!axis.ok || axis.spine.length < 2) continue;
      const spine = axis.spine;
      for (let i = 0; i < spine.length; i++) spine[i].cum = axis.cum[i];
      const halfWidth = axis.halfWidth;

      const totalLen = axis.cum[axis.cum.length - 1];
      const postCount = Math.floor(totalLen / POST_SPACING);
      if (postCount === 0) continue;

      // 两侧对称放置
      for (const side of [+1, -1]) {
        const sideOffset = (halfWidth + POST_OFFSET) * side;
        let prev = null;   // {x, z} 上一根保留立柱位置；路口/打结处断链（横梁不跨缺口）
        for (let i = 0; i <= postCount; i++) {
          const targetArc = i * POST_SPACING;
          let j = 1;
          while (j < spine.length && spine[j].cum < targetArc) j++;
          if (j >= spine.length) j = spine.length - 1;
          const s = spine[j];

          // 路口裁剪：spine 点在路口圆内 → 断链（护栏终止于路口边界，
          // 横梁绝不跨路口——否则路口处多路护栏互相穿插成圈）
          if (topo && topo.pointInJunction(edge.id, s.px, s.pz)) { prev = null; continue; }

          const x = s.px + s.nx * sideOffset;
          const z = s.pz + s.nz * sideOffset;

          /* 弯道打结防护：相邻偏移点弦长 / 弧长（POST_SPACING）< 0.35 → 内侧
           * 偏移曲线在急弯处收缩打结（曲率半径 R < 偏移量时偏移曲线自交）。
           * 跳过此柱并断链留缝——留缝读作护栏端部开口，好过穿插扭结。 */
          if (prev) {
            const chord = Math.hypot(x - prev.x, z - prev.z);
            if (chord < POST_SPACING * 0.35) { prev = null; continue; }
          }

          // 立柱旋转：让 box 沿道路切向
          // 切线 = (tx, tz) = (nz, -nx)（从法线 nx=-tz/l, nz=tx/l 反推）
          const rotY = directionToRotationY(s.nz, -s.nx);
          const py = s.py || 0;   // 高架护栏随路面高程
          posts.push({ x, y: py + POST_H / 2, z, rotY });

          // 横梁段（除链首外，每根立柱连一段到前一根）
          if (prev) {
            const midX = (prev.x + x) / 2;
            const midZ = (prev.z + z) / 2;
            const dx = x - prev.x;
            const dz = z - prev.z;
            const len = Math.sqrt(dx * dx + dz * dz);
            if (len > 0.01) {
              const beamRotY = directionToRotationY(dx, dz);
              upperBeamSegs.push({ x: midX, z: midZ, len, rotY: beamRotY, py });
              lowerBeamSegs.push({ x: midX, z: midZ, len, rotY: beamRotY, py });
            }
          }
          prev = { x, z };
        }
      }
    }

    if (posts.length === 0) return;

    const N = posts.length;
    const postGeo = new THREE.BoxGeometry(POST_W, POST_H, BEAM_THICKNESS * 2);
    const postMat = getStdMaterial(COLOR_POST, 0.45, 0.8);
    postMesh = new THREE.InstancedMesh(postGeo, postMat, N);

    const M = upperBeamSegs.length;
    const beamGeo = new THREE.BoxGeometry(SEGMENT_LEN, BEAM_W, BEAM_THICKNESS);
    const beamMat = getStdMaterial(COLOR_BEAM, 0.35, 0.85);
    upperBeamMesh = new THREE.InstancedMesh(beamGeo, beamMat, M);
    lowerBeamMesh = new THREE.InstancedMesh(beamGeo, beamMat, M);

    const dummy = new THREE.Object3D();
    for (let i = 0; i < N; i++) {
      const p = posts[i];
      dummy.position.set(p.x, p.y, p.z);
      dummy.rotation.set(0, p.rotY, 0);
      dummy.scale.set(1, 1, 1);
      dummy.updateMatrix();
      postMesh.setMatrixAt(i, dummy.matrix);
    }
    for (let i = 0; i < M; i++) {
      const b = upperBeamSegs[i];
      const scale = b.len / SEGMENT_LEN;
      dummy.position.set(b.x, (b.py || 0) + BEAM_UPPER_Y, b.z);
      dummy.rotation.set(0, b.rotY, 0);
      dummy.scale.set(scale, 1, 1);
      dummy.updateMatrix();
      upperBeamMesh.setMatrixAt(i, dummy.matrix);

      const lb = lowerBeamSegs[i];
      dummy.position.set(lb.x, (lb.py || 0) + BEAM_LOWER_Y, lb.z);
      dummy.rotation.set(0, lb.rotY, 0);
      dummy.scale.set(lb.len / SEGMENT_LEN, 1, 1);
      dummy.updateMatrix();
      lowerBeamMesh.setMatrixAt(i, dummy.matrix);
    }
    postMesh.instanceMatrix.needsUpdate = true;
    upperBeamMesh.instanceMatrix.needsUpdate = true;
    lowerBeamMesh.instanceMatrix.needsUpdate = true;

    group.add(postMesh, upperBeamMesh, lowerBeamMesh);
  }

  function getGroup() { return group; }

  return { build, clear, getGroup };
}
