/**
 * vis_perception_effect.test.mjs — PerceptionView & EffectView 真实几何与视觉门禁单测
 *
 * 兼容 run_vis_smoke.mjs (three-preload) 与 three-real-preload：
 * 先将 globalThis.THREE 切换为真实 three.module.js，再动态 import 目标视图，
 * 使模块顶层注入的 `const THREE = globalThis.THREE` 绑定到真实 Three 几何实现。
 */

import * as RealTHREE from '../tools/flowboard/vendor/three/three.module.js';
import { ok, eq, done } from './test-utils.mjs';

globalThis.THREE = RealTHREE;

const { createPerceptionView } = await import('../tools/flowboard/js/vis/view/PerceptionView.js');
const { createEffectView } = await import('../tools/flowboard/js/vis/view/EffectView.js');
const { createSceneStore } = await import('../tools/flowboard/js/vis/store/SceneStore.js');

const THREE = RealTHREE;

console.log('=== PerceptionView & EffectView 车规级几何契约测试 ===\n');

// ── 1. PerceptionView: LiDAR 点尺度、无亮黄巨块、深度测试与多模态光带 ──
{
  const scene = new THREE.Scene();
  const perc = createPerceptionView(scene);
  const store = createSceneStore();

  store.ego = { x: 20, y: -1.75, z: 0, heading: 0, speed: 14, length: 4.6, width: 2.0 };
  store.entities = [
    { id: 1, type: 'car', x: 45, y: -1.75, z: 0, heading: 0, speed: 12, length: 4.6, width: 2.0, height: 1.5 },
    { id: 2, type: 'car', x: 38, y: -5.25, z: 0, heading: 0.16, speed: 15, length: 4.6, width: 2.0, height: 1.5 },
    { id: 99, type: 'tl', x: 60, y: 8.0, z: 0, heading: 0, speed: 0 },
  ];
  store.predictions = [
    {
      id: 2,
      branches: [
        { mode: 'cut_in_left', probability: 0.78, points: [[38, -5.25], [48, -3.5], [60, -1.75]] },
        { mode: 'lane_keep', probability: 0.22, points: [[38, -5.25], [48, -5.25], [60, -5.25]] },
      ],
    },
  ];

  perc.update(store, 1000);

  const pointsObj = scene.children.find(c => c.isPoints);
  ok('PerceptionView 创建了 Points 对象', !!pointsObj);
  ok('LiDAR 点尺寸为厘米级 (size <= 0.20m，杜绝 2.2m 方块)',
    pointsObj.material.size > 0.05 && pointsObj.material.size <= 0.20);
  eq('LiDAR 点启用透视衰减与深度测试', pointsObj.material.sizeAttenuation && pointsObj.material.depthTest, true);
  ok('合成受照轮廓点非空', pointsObj.geometry.drawRange.count >= 20);

  const colArr = pointsObj.geometry.attributes.color.array;
  const ptCount = pointsObj.geometry.drawRange.count;
  let hasBrightYellow = false;
  for (let i = 0; i < ptCount; i++) {
    const r = colArr[i * 3], g = colArr[i * 3 + 1], b = colArr[i * 3 + 2];
    if (r > 0.8 && g > 0.7 && b < 0.3) hasBrightYellow = true;
  }
  eq('合成车辆轮廓点零亮黄颜色污染', hasBrightYellow, false);

  const lineObjs = scene.children.filter(c => c.isLineSegments);
  ok('包含角标框、完整线框、贴地关联线与视场脉冲弧 4 组 LineSegments', lineObjs.length === 4);
  const wireObj = lineObjs.find(c => c.material.vertexColors === true && c.material.opacity >= 0.5);
  ok('完整 3D 线框存在且每目标 12 边（24 顶点）',
    !!wireObj && wireObj.geometry.drawRange.count === 2 * 24);
  const rayAndSweepLines = lineObjs.filter(c => c.material.vertexColors === true && c.material.opacity <= 0.22);
  eq('传感器关联线与视场线均启用 depthTest 且 opacity <= 0.22',
    rayAndSweepLines.length === 2 && rayAndSweepLines.every(c => c.material.depthTest === true && c.material.opacity <= 0.22), true);

  const predGroup = scene.children.find(c => c.name === 'predictionTrajectories');
  ok('predictionTrajectories 分组存在且可见', !!predGroup && predGroup.visible === true);
  const predMesh = predGroup.children[0];
  ok('多模态预测为 3D 三角带 Mesh 且索引非空',
    !!predMesh && predMesh.isMesh === true && predMesh.geometry.drawRange.count > 60);

  const predPos = predMesh.geometry.attributes.position.array;
  ok('预测光带离地高度稳居路面标线之上 (y ≈ +0.175m)', Math.abs(predPos[1] - 0.175) < 0.03);

  // 验证后端 prediction_node 真实 C++ 契约 (trajectories / intent / prob / world_waypoints[wx,wy,speed_mps])
  store.predictions = [
    {
      object_id: 1,
      confidence: 0.9,
      wx: 45,
      wy: -1.75,
      trajectories: [
        {
          prob: 0.86,
          intent: 'lane_keep',
          world_waypoints: [
            [50.5, -1.75, 12.0],
            [56.5, -1.75, 12.0],
            [62.5, -1.75, 12.0],
          ],
        },
        {
          prob: 0.05,
          intent: 'cut_in_left',
          world_waypoints: [
            [50.5, -0.9, 12.0],
            [56.5, 0.0, 12.0],
          ],
        },
      ],
    },
  ];
  perc.update(store, 1100);
  ok('真实 C++ trajectories/world_waypoints 契约渲染非空光带', predMesh.geometry.drawRange.count > 30);
  ok('world_waypoints 第 3 维 speed_mps(12.0) 未被误判为海拔高度 Z',
    Math.abs(predMesh.geometry.attributes.position.array[1] - 0.175) < 0.03);

  // 验证局部 Frenet 参考线域外截断幻影过滤（wx=-300m 但首点被截断至 x=20m 时必须剔除）
  store.predictions = [
    {
      object_id: 999,
      wx: -300,
      wy: 25,
      trajectories: [
        { prob: 0.85, intent: 'cut_in_right', world_waypoints: [[20, -40, 10], [50, -1.75, 10]] },
      ],
    },
  ];
  perc.update(store, 1150);
  eq('超出局部 Frenet 域的截断幻影预测光带被干净剔除', predMesh.geometry.drawRange.count, 0);

  // 验证路灯杆 (pole, h=6.5) 与行道树 (tree, h=4.6) 即使经 perception/obstacles 标为 unknown，
  // 也能按 height / facilities kind 渲染出完整高度 3D 角标框（而非 1m 矮方框）
  store.entities = [];
  store.predictions = [];
  store.perceived = [
    { id: 8110, type: 'unknown', x: 35, y: -8.5, length: 0.45, width: 0.45, height: 6.5 },
  ];
  store.facilities = [
    { kind: 'tree', x: 42, y: -10.0, z: 0, length: 0.9, width: 0.9, height: 4.6, heading: 0 },
  ];
  perc.update(store, 1200);
  const boxMesh = scene.children.find(c => c.isLineSegments && c.material.vertexColors === false);
  const bPos = boxMesh.geometry.attributes.position.array;
  const bCnt = boxMesh.geometry.drawRange.count;
  let maxBoxY = -Infinity;
  for (let i = 0; i < bCnt; i++) {
    const yy = bPos[i * 3 + 1];
    if (yy > maxBoxY) maxBoxY = yy;
  }
  ok('感知路灯/树木角标框渲染完整高度 (maxBoxY >= 6.0m)', maxBoxY >= 6.0);

  perc.dispose();
  eq('PerceptionView dispose 干净移除全部子节点', scene.children.length, 0);
}

// ── 2. EffectView: 移除头顶悬浮方块、无重复方形雨雪点、切入贴地警示翼法线定位 ──
{
  const scene = new THREE.Scene();
  const effect = createEffectView(scene);
  const store = createSceneStore();

  store.env.weather = 'clear';
  store.ego = { x: 20, y: -1.75, z: 0, heading: 0, speed: 14, brake: 0, lights: 0 };
  store.entities = [
    { id: 'cutin_car', type: 'car', x: 34, y: -5.25, z: 0, heading: 0.18, speed: 14, vx: 13.8, vy: 2.4, width: 2.0 },
  ];

  effect.update(store, 500);
  const effectGroup = scene.children.find(c => c.name === 'effectGroup');
  ok('effectGroup 已挂载', !!effectGroup);

  let hasPoints = false;
  effectGroup.traverse(o => { if (o.isPoints) hasPoints = true; });
  eq('EffectView 不重复创建方形天气 Points 粒子', hasPoints, false);

  let hasHighFloatingMarker = false;
  effectGroup.traverse(o => {
    if (o !== effectGroup && o.isMesh && o.visible && o.position.y > 2.0) {
      hasHighFloatingMarker = true;
    }
  });
  eq('EffectView 无头顶 y=2.5m 悬浮黄橙方块', hasHighFloatingMarker, false);

  const chevrons = effectGroup.children.filter(c => c.isMesh);
  eq('旁车切入触发 1 枚贴地警示翼', chevrons.length, 1);
  if (chevrons[0]) {
    ok('切入警示翼高于路面沥青层 (y ≈ 0.17m)', Math.abs(chevrons[0].position.y - 0.17) < 0.03);
    ok('切入警示翼偏移在车辆切入侧 (左侧 THREE z < 5.25)', chevrons[0].position.z < 5.25 - 0.8);
  }

  const wakeGroup = effectGroup.children.find(c => c.isGroup);
  eq('晴天双后轮水雾隐藏', wakeGroup.visible, false);
  store.env.weather = 'rain';
  effect.update(store, 600);
  eq('雨天高速双后轮水雾开启', wakeGroup.visible, true);

  effect.dispose();
  eq('EffectView dispose 干净移除分组', scene.children.length, 0);
}

// ── 3. B1 地下层高程隔离 (z=-4.5m 不跳上 z=0m 顶板) & 交通标志角标框 ──
{
  console.log('\n--- 3. B1 预测高程隔离与 sign 感知角标框 ---');
  const scene = new RealTHREE.Scene();
  const perc = createPerceptionView(scene);
  const b1Store = createSceneStore();
  b1Store.roadNetwork = {
    edges: [
      { id: 0, type: 'garage_hall', lanes: 4, lane_width: 3.5, oneway: false,
        nodes: [[-60, 0, -4.5], [20, 0, -4.5]] },
      { id: 1, type: 'urban', lanes: 4, lane_width: 3.5, oneway: false,
        nodes: [[-60, 0, 0.0], [20, 0, 0.0]] },
    ],
  };
  b1Store.ego = { x: -30, y: -1.75, z: -4.5, heading: 0, speed: 6.0, length: 4.6, width: 2.0 };
  b1Store.entities = [
    { id: 'b1_car', type: 'car', x: -10, y: -1.75, z: -4.5, heading: 0, speed: 6.0, length: 4.5, width: 2.0 },
  ];
  b1Store.predictions = [
    {
      obstacle_id: 'b1_car',
      trajectories: [
        {
          intent: 'lane_keep',
          prob: 0.9,
          world_waypoints: [[-10, -1.75, 6.0], [0, -1.75, 6.0], [10, -1.75, 6.0]],
        },
      ],
    },
  ];
  b1Store.perceived = [
    { id: 'sign_1', type: 'sign', x: -5, y: 4.2, z: -4.5, heading: 0, length: 0.55, width: 0.85, height: 2.6 },
  ];
  b1Store.facilities = [];

  perc.update(b1Store, 1000);
  const predGroup = scene.children.find(c => c.name === 'predictionTrajectories');
  const predMesh = predGroup && predGroup.children.find(c => c.isMesh);
  const predCount = predMesh ? predMesh.geometry.drawRange.count : 0;
  ok('B1 预测光带非空', predCount > 0);
  let maxPredY = -Infinity;
  const pArr = predMesh.geometry.attributes.position.array;
  const idxArr = predMesh.geometry.index ? predMesh.geometry.index.array : null;
  for (let i = 0; i < predCount; i++) {
    const vi = idxArr ? idxArr[i] : i;
    const yy = pArr[vi * 3 + 1];
    if (yy > maxPredY) maxPredY = yy;
  }
  ok(`B1 预测光带贴合地下层 (maxY=${maxPredY.toFixed(2)}m < -1.0m，未跳上地面)`, maxPredY < -1.0);

  const lineObjs = scene.children.filter(c => c.isLineSegments);
  const cornerObj = lineObjs[0];
  const cornerCount = cornerObj ? cornerObj.geometry.drawRange.count : 0;
  const cArr = cornerObj.geometry.attributes.position.array;
  let minSignY = Infinity, maxSignY = -Infinity;
  for (let i = 0; i < cornerCount; i++) {
    const yy = cArr[i * 3 + 1];
    if (yy < minSignY) minSignY = yy;
    if (yy > maxSignY) maxSignY = yy;
  }
  ok(`sign 感知角标框渲染非空 (${cornerCount} verts, height=${(maxSignY - minSignY).toFixed(2)}m)`,
     cornerCount > 0 && Math.abs((maxSignY - minSignY) - 2.6) < 0.25);

  perc.dispose();
}

// ── 4. 匝道/高架前视包络高程钳制（禁止向下拉出切入路边的绿色三角面） ──
{
  const scene = new THREE.Scene();
  const perc = createPerceptionView(scene);
  const store = createSceneStore();
  store.roadNetwork = {
    edges: [
      {
        id: 1,
        type: 'ramp_curve',
        lanes: 2,
        lane_width: 3.5,
        oneway: true,
        nodes: [[0, 0, 5.0], [40, 0, 0.0]],
      },
    ],
  };
  store.ego = { x: 10, y: 0, z: 3.75, heading: 0, speed: 12, length: 4.6, width: 2.0 };
  store.entities = [];
  perc.update(store, 1200);

  const sweepMesh = scene.children.find(c => c.isMesh && c.geometry?.attributes?.color && c.material?.blending === THREE.AdditiveBlending);
  ok('前视包络 sweepMesh 存在', !!sweepMesh);
  const pos = sweepMesh.geometry.attributes.position.array;
  let minY = Infinity;
  for (let i = 1; i < pos.length; i += 3) {
    if (pos[i] < minY) minY = pos[i];
  }
  ok(`高架匝道前视包络不跌落至地面 z=0 (minY=${minY.toFixed(2)}m >= 2.5m)`, minY >= 2.5);
  perc.dispose();
}

done();
