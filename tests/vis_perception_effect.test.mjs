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
  ok('包含角标框、贴地关联线与视场脉冲弧 3 组 LineSegments', lineObjs.length === 3);
  const rayAndSweepLines = lineObjs.filter(c => c.material.vertexColors === true);
  eq('传感器关联线与视场线均启用 depthTest 且 opacity <= 0.22',
    rayAndSweepLines.every(c => c.material.depthTest === true && c.material.opacity <= 0.22), true);

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

done();
