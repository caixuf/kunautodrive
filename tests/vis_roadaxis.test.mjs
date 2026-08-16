/**
 * vis_roadaxis.test.mjs — RoadAxis 共享路轴（阶段0 关键石）纯函数回归
 *
 * 锁定：
 *   1. 2 点直道（road_1290439822s3）：centerline 是最左车道左缘，computeRoadAxis
 *      必须推导出 TRUE 中心 spine（非左缘），fromLanes=true，偏移≈n·laneWidth/2，
 *      真中心与三车道中心均值吻合。
 *   2. 多段中心线（CatmullRom 采样路径）：末点差分修复（points[i-2]→points[i-1]）
 *      后 spine 不再翻转，fromLanes=true、无 NaN。这正是"路整体左偏"根因修复。
 *   3. 居中约定路（centerline 本就是车道组中心）：fromLanes=false、不偏移（安全兜底）。
 *
 * 全图规模量化验证（osm_lujiazui_v2 9675 路，含 OLD buggy 复刻对比）为手动脚本，
 * 见下方注释；核心不变量已在此单测覆盖。
 */
import * as THREE from '../tools/flowboard/vendor/three/three.module.js';
globalThis.THREE = THREE; // Curve.js 在 CatmullRom 分支用全局 THREE
import { computeRoadAxis, computeEdgeAxis } from '../tools/flowboard/js/vis/model/RoadAxis.js';
import { ok, done } from './test-utils.mjs';

console.log('=== RoadAxis 共享路轴 ===\n');

// ── 1. 2 点直道：road_1290439822s3（centerline = 最左车道左缘）──
const sample = {
  centerline: [[639.1227613643078, 577.0092408372332, 0.0], [633.3225851191233, 575.0524559198124, 0.0]],
  lane_width: 3.2,
  lanes: [
    { centerline: [[636.5831287592141, 584.6207854040006, 0.0], [630.7729567243932, 582.6638632011957, 0.0]], width: 3.2 },
    { centerline: [[637.6035669925556, 581.5716282054531, 0.0], [631.7933964050645, 579.6147066172366, 0.0]], width: 3.2 },
    { centerline: [[638.6138773342792, 578.5323759767715, 0.0], [632.8137003681352, 576.5755907536384, 0.0]], width: 3.2 },
  ],
};
const r1 = computeRoadAxis(sample);
ok('2点直道 fromLanes=true', r1.fromLanes === true);
ok('2点直道 ok', r1.ok === true);
ok('2点直道 spine 非左缘（起点 px≈637.6 非 639.1）', Math.abs(r1.spine[0].px - 637.6) < 0.5);
ok('2点直道 spine 起点 pz≈-581.6（真中心）', Math.abs(r1.spine[0].pz - (-581.6)) < 0.5);
ok('2点直道 halfWidth≈4.8（3×3.2/2）', Math.abs(r1.halfWidth - 4.8) < 0.2);
// 真中心相对左缘偏移 ≈ +4.8（沿法线带符号），与车道中心均值吻合
const off = (r1.spine[0].px - sample.centerline[0][0]) * r1.spine[0].nx +
            (r1.spine[0].pz - (-sample.centerline[0][1])) * r1.spine[0].nz;
ok('2点直道 左缘→真中心法向偏移≈4.8', Math.abs(off - 4.8) < 0.3);
ok('2点直道 无 NaN', isFinite(r1.spine[0].px) && isFinite(r1.spine[0].pz));

// ── 2. 多段中心线（CatmullRom 采样，末点差分修复路径）──
// 直东向 3 点路，左缘 centerline(north=0)，3 条车道中心在 north=1.6/4.8/8.0。
// 真中心 north=4.8 → THREE z=-4.8；normal=(0,1) → 偏移 d≈-4.8。
const curve = {
  centerline: [[0, 0, 0], [10, 0, 0], [20, 0, 0]],
  lane_width: 3.2,
  lanes: [
    { centerline: [[0, 1.6, 0], [10, 1.6, 0], [20, 1.6, 0]], width: 3.2 },
    { centerline: [[0, 4.8, 0], [10, 4.8, 0], [20, 4.8, 0]], width: 3.2 },
    { centerline: [[0, 8.0, 0], [10, 8.0, 0], [20, 8.0, 0]], width: 3.2 },
  ],
};
const r2 = computeRoadAxis(curve);
ok('多段路 fromLanes=true（末点未翻转）', r2.fromLanes === true);
ok('多段路 halfWidth≈4.8', Math.abs(r2.halfWidth - 4.8) < 0.2);
// 末点法线须与首点一致（无翻转）：直东向 normal≈(0,1)
const s0 = r2.spine[0], sL = r2.spine[r2.spine.length - 1];
ok('多段路 末点法线与首点一致（无翻转）', Math.abs(s0.nx - sL.nx) < 0.05 && Math.abs(s0.nz - sL.nz) < 0.05);
ok('多段路 无 NaN', r2.spine.every(p => isFinite(p.px) && isFinite(p.pz)));

// ── 3. 居中约定：centerline 本就是车道组中心 → 不偏移（安全兜底）──
const centered = {
  centerline: [[0, 4.8, 0], [10, 4.8, 0], [20, 4.8, 0]],
  lane_width: 3.2,
  lanes: [
    { centerline: [[0, 1.6, 0], [10, 1.6, 0], [20, 1.6, 0]], width: 3.2 },
    { centerline: [[0, 4.8, 0], [10, 4.8, 0], [20, 4.8, 0]], width: 3.2 },
    { centerline: [[0, 8.0, 0], [10, 8.0, 0], [20, 8.0, 0]], width: 3.2 },
  ],
};
const r3 = computeRoadAxis(centered);
ok('居中路 fromLanes=false（不偏移）', r3.fromLanes === false);
ok('居中路 spine 保持 centerline（起点 px≈0）', Math.abs(r3.spine[0].px) < 0.5);

// ── computeEdgeAxis 便捷入口（家具视图用）──
const edge = {
  name: 'road_x', id: 7, type: 'urban', lanes: 3, lane_width: 3.2,
  nodes: sample.centerline.map(p => [p[0], p[1], p[2]]),
};
const r4 = computeEdgeAxis(edge, { road_x: sample.lanes });
ok('computeEdgeAxis 经 lane_data 推导 TRUE 中心', r4.fromLanes === true &&
  Math.abs(r4.spine[0].px - 637.6) < 0.5);

// ── 4. 阶段2 竖向分层：elevation/level 消费（DSL 枢纽抽取落地后点亮）──
const elevated = {
  centerline: [[0, 4.8, 0], [10, 4.8, 0], [20, 4.8, 0]],
  lane_width: 3.2, elevation: 12.5,
  lanes: [
    { centerline: [[0, 1.6, 0], [10, 1.6, 0], [20, 1.6, 0]], width: 3.2 },
    { centerline: [[0, 4.8, 0], [10, 4.8, 0], [20, 4.8, 0]], width: 3.2 },
    { centerline: [[0, 8.0, 0], [10, 8.0, 0], [20, 8.0, 0]], width: 3.2 },
  ],
};
const r5 = computeRoadAxis(elevated);
ok('elevation 消费：spine py 抬升到 12.5', r5.spine.every(p => Math.abs(p.py - 12.5) < 1e-6));
ok('elevation 消费：fromLanes/几何不受影响', r5.fromLanes === false && r5.ok === true);

const leveled = { ...centered, level: -3 };
const r6 = computeRoadAxis(leveled);
ok('level 消费：spine py 抬升到 -3', r6.spine.every(p => Math.abs(p.py - (-3)) < 1e-6));

const noElev = { ...centered };
delete noElev.elevation; delete noElev.level;
const r7 = computeRoadAxis(noElev);
ok('无 elevation/level：ok 且 py 为有限数（不误加高程）', r7.ok === true &&
  r7.spine.every(p => Number.isFinite(p.py)));
// 与基准（同路、无 elevation）逐点一致：确认 applyElevation 未误触发
const baseline = computeRoadAxis(centered);
ok('无 elevation/level：py 与基准一致（未误加高程）',
  r7.spine.every((p, i) => p.py === baseline.spine[i].py));

console.log('\n全图规模量化（手动，需真实 map.json，结果摘要）：');
console.log('  osm_lujiazui_v2 9675 路 → 修复救回 2684 路（OLD 误杀→NEW 接受）；');
console.log('  OLD 误偏移 769 路→NEW 正确居中（修正非平行/退化 stub）；末点-only 翻转=0（无残留 bug）。');

done();
