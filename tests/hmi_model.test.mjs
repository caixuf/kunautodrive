/**
 * hmi_model.test.mjs — HMI 专区纯模型门禁
 *
 * 只测 `buildHmiState(topo)` 的纯逻辑（不碰 DOM/THREE）：
 *  1. 空 / 缺失 topo 不抛错，全部降级为占位符
 *  2. 满数据正确归约（速度/行为/车道/前车/BEV/感知计数）
 *  3. 机动意图文本随 behavior 变化（巡航 / 左变道）
 *  4. occupancy 字段透传（HMI-2 数据源）
 *
 * 跑法：node --import ./tests/support/three-preload.mjs tests/hmi_model.test.mjs
 */

import { buildHmiState } from '../tools/flowboard/js/vis/hud/HmiModel.js';
import { ok, eq, done } from './test-utils.mjs';

console.log('=== HMI 专区纯模型门禁 ===\n');

/* ── 1. 空 / 缺失：不崩，降级占位 ── */
{
  const a = buildHmiState(undefined);
  const b = buildHmiState({});
  ok('空 topo 不抛错', !!a && !!b);
  eq('空 topo hasData=false', a.hasData, false);
  eq('空 topo 速度 null', a.driving.speedKmh, null);
  eq('空 topo 意图占位', a.planning.intent, '—');
  eq('空 topo 感知计数 0', a.perception.percCount, 0);
  eq('空 topo occupancy null', a.perception.occupancy, null);
}

/* ── 2. 满数据 ── */
{
  const topo = {
    metrics: {
      scene: {
        ego: { x: 0, y: 0, heading: 0, speed: 10, pitch: 0, roll: 0, throttle: 0.3, brake: 0, steer: 0.02, mu_scale: 1.0 },
        lane: { width: 3.5, count: 3 },
        entities: [{ id: 1 }, { id: 2 }, { id: 3 }, { id: 4 }],
        perception_entities: [{ id: 1 }, { id: 2 }],
        traffic_lights: [],
        occupancy: { grid_w: 108, grid_h: 38, cell_size_m: 0.37 },
      },
      behavior: { state: 'CRUISE', target_speed: 12, lane_count: 3, committed_lane: 1, lane_width: 3.5 },
      control_debug: { speed: 10, target_speed: 12, steer: 0.02, throttle: 0.3, brake: 0.0, mode: 'TRACK', hazard: false },
      planning_debug: { active_scenario: 'LANE_FOLLOW', active_stage: 'CRUISE', command_speed: 12 },
      bev: { count: 3 },
    },
  };
  const s = buildHmiState(topo);
  ok('满数据 hasData=true', s.hasData === true);
  ok('速度 km/h ≈ 36', Math.abs(s.driving.speedKmh - 36) < 1e-9);
  eq('巡航目标 km/h = 43（12*3.6=43.2→显示层负责取整，这里保留精度）', Math.round(s.driving.cruiseKmh), 43);
  eq('行为标签 巡航', s.driving.behavior.label, '巡航');
  eq('车道 1/3', s.driving.laneText, '1/3');
  eq('机动码 cruise', s.driving.maneuver, 'cruise');
  eq('意图 车道保持', s.planning.intent, '车道保持');
  eq('感知计数 2', s.perception.percCount, 2);
  eq('真值计数 4', s.perception.gtCount, 4);
  eq('BEV 文本', s.perception.bevText, 'BEV: 3');
  eq('感知源 感知', s.perception.source, '感知');
  eq('occupancy 透传 grid_w', s.perception.occupancy && s.perception.occupancy.grid_w, 108);
  eq('阶段文本', s.planning.stageText, 'LANE_FOLLOW · CRUISE');
}

/* ── 3. 机动意图随 behavior 变化 ── */
{
  const topo = {
    metrics: {
      scene: { ego: { x: 0, y: 0, heading: 0, speed: 5 }, lane: { width: 3.5, count: 4 } },
      behavior: { state: 'LEFT_CHANGE', lane_count: 4, committed_lane: 2 },
      control_debug: { mode: 'MANEUVER', speed: 5, hazard: false },
    },
  };
  const s = buildHmiState(topo);
  eq('左变道 机动码', s.driving.maneuver, 'left');
  eq('左变道 意图', s.planning.intent, '向左变道');
  eq('左变道 行为标签', s.driving.behavior.label, '左变道');
}

/* ── 4. occupancy 尺寸为 0 视为未接入 ── */
{
  const s = buildHmiState({ metrics: { scene: { occupancy: { grid_w: 0, grid_h: 0 } } } });
  eq('occupancy grid_w=0 → null', s.perception.occupancy, null);
}

/* ── 5. 真占用栅格（metrics.occupancy + cells_b64 解码）── */
{
  const b64 = Buffer.from([1, 2, 1, 0]).toString('base64'); // free / occ / free / unknown
  const s = buildHmiState({
    metrics: {
      occupancy: {
        grid_w: 2, grid_h: 2, cell_size_m: 0.5,
        origin_x: 0, origin_y: -0.5, frame: 'body',
        occupied_cells: 1, free_cells: 2, cells_b64: b64,
      },
    },
  });
  const o = s.perception.occupancy;
  ok('occupancy cells 解码长度 = gw*gh', !!o && !!o.cells && o.cells.length === 4);
  eq('occupancy cell[1] = OCCUPIED', o.cells[1], 2);
  eq('occupancy occupied 计数透传', o.occupied, 1);
  eq('occupancy frame', o.frame, 'body');
  eq('occupancy 优先取 metrics.occupancy', o.grid_w, 2);
}

/* ── 6. 规划意图增强：目标车道 / 间隙 / 并道 / 轨迹片段 ── */
{
  const s = buildHmiState({
    metrics: {
      scene: {
        ego: { x: 0, y: 0, heading: 0, speed: 6 },
        trajectory_path: [[0, 0, 5], [10, 0, 5], [20, 2, 3]],
      },
      behavior: {
        state: 'RIGHT_CHANGE', committed_lane: 2, target_lane: 3, lane_count: 4,
        route_lane_goal: 3, dist_to_lane_drop_m: 120,
        left_ok: true, right_ok: false, left_gap: 5.5, right_gap: -1,
        upcoming_own_lanes: -1,
      },
    },
  });
  const pl = s.planning;
  eq('laneCount 4', pl.laneCount, 4);
  eq('currentLane 2', pl.currentLane, 2);
  eq('targetLane 3', pl.targetLane, 3);
  eq('变道方向 right', pl.laneChangeDir, 'right');
  eq('并道距离 120', pl.distToLaneDropM, 120);
  eq('左侧可行', pl.leftOk, true);
  eq('右侧不可行', pl.rightOk, false);
  eq('左间隙 5.5', pl.leftGapM, 5.5);
  ok('轨迹片段 pts>=2', !!pl.traj && pl.traj.pts.length === 3);
  eq('轨迹点[2] 前向=20', pl.traj.pts[2][0], 20);
  eq('轨迹点[2] 右向=2', pl.traj.pts[2][1], 2);
}

/* ── 7. 轨迹片段随 heading 旋转（ego 朝北 heading=π/2，正前方 +y）── */
{
  const s = buildHmiState({
    metrics: {
      scene: {
        ego: { x: 0, y: 0, heading: Math.PI / 2, speed: 3 },
        trajectory_path: [[0, 0, 3], [0, 10, 3]], // 世界 +y = ego 正前方
      },
    },
  });
  ok('朝北时正前方轨迹 fwd≈10/rht≈0',
    Math.abs(s.planning.traj.pts[1][0] - 10) < 1e-6 && Math.abs(s.planning.traj.pts[1][1]) < 1e-6);
}

/* ── 8. 告警聚合：MRM + 降级 → bad ── */
{
  const s = buildHmiState({
    metrics: {
      scene: { ego: { x: 0, y: 0, heading: 0, speed: 2 } },
      tor: { state: 'MRM', countdown_s: 0 },
      degrade: { active: true, reason_name: 'perception_timeout' },
    },
  });
  eq('MRM 告警级别 bad', s.alerts.level, 'bad');
  ok('含 MRM 告警', s.alerts.items.some((it) => it.label.includes('MRM')));
  ok('含降级告警', s.alerts.items.some((it) => it.label.includes('降级')));
}

/* ── 9. L3 激活 = ok；左变道 → blinker left ── */
{
  const s = buildHmiState({
    metrics: {
      scene: { ego: { x: 0, y: 0, heading: 0, speed: 5 } },
      behavior: { state: 'LEFT_CHANGE', lane_count: 4, committed_lane: 2 },
      control_debug: { mode: 'TRACK', speed: 5, hazard: false },
      tor: { state: 'ACTIVE' },
    },
  });
  eq('L3 激活级别 ok', s.alerts.level, 'ok');
  ok('含 L3 激活', s.alerts.items.some((it) => it.label.includes('L3 激活')));
  eq('左变道 blinker left', s.driving.blinker, 'left');
}

/* ── 10. safety_evidence.fault 是结构体：type=none 不报；真实故障报 bad（不出现 [object Object]）── */
{
  const noneS = buildHmiState({
    metrics: {
      scene: { ego: { x: 0, y: 0, heading: 0, speed: 1 } },
      safety_evidence: { fault: { id: 'none', type: 'none', component: 'safety_control' } },
      tor: { state: 'ACTIVE' },
    },
  });
  ok('fault=none 不报故障', !noneS.alerts.items.some((it) => it.label.includes('故障')));

  const realS = buildHmiState({
    metrics: {
      scene: { ego: { x: 0, y: 0, heading: 0, speed: 1 } },
      safety_evidence: { fault: { id: 'f1', type: 'sensor_lost', component: 'perception' } },
    },
  });
  eq('真实故障级别 bad', realS.alerts.level, 'bad');
  ok('故障标签含类型且无 [object Object]',
    realS.alerts.items.some((it) => it.label.includes('sensor_lost'))
    && !realS.alerts.items.some((it) => it.label.includes('[object')));
}

/* ── 11. 告警升级：SAFE 控制模式 → warn 级 ── */
{
  const s = buildHmiState({
    metrics: {
      scene: { ego: { x: 0, y: 0, heading: 0, speed: 5 } },
      control_debug: { speed: 5, mode: 'SAFE', hazard: true },
    },
  });
  eq('安全介入 → warn 级别', s.alerts.level, 'warn');
  ok('含安全介入告警', s.alerts.items.some((it) => it.label.includes('安全介入')));
}

/* ── 12. ODD 越界 / 驶出 / 定位发散 ── */
{
  const s = buildHmiState({
    metrics: {
      scene: { ego: { x: 0, y: 0, heading: 0, speed: 3 } },
      odd: { in_odd: false, violations: [1, 2], loc_diverged: true },
    },
  });
  ok('含 ODD 越界', s.alerts.items.some((it) => it.label.includes('ODD 越界')));
  ok('含驶出 ODD', s.alerts.items.some((it) => it.label.includes('驶出 ODD')));
  ok('含定位发散', s.alerts.items.some((it) => it.label.includes('定位发散')));
  eq('ODD 告警级别 warn', s.alerts.level, 'warn');
}

/* ── 13. TOR 接管请求倒计时 → bad ── */
{
  const s = buildHmiState({
    metrics: {
      scene: { ego: { x: 0, y: 0, heading: 0, speed: 2 } },
      tor: { state: 'TOR_REQUESTED', countdown_s: 5.4 },
    },
  });
  eq('接管请求级别 bad', s.alerts.level, 'bad');
  ok('含接管请求倒计时(5s)', s.alerts.items.some((it) => it.label.includes('接管请求') && it.label.includes('5')));
  eq('接管请求 blinker 空（非 MRM）', s.driving.blinker, '');
}

/* ── 14. 左变道方向 + 车道缩减 + 轨迹 clip(≤~60m) + 倒挡段保留 ── */
{
  const path = [];
  for (let x = 0; x <= 200; x += 10) path.push([x, 0, x < 50 ? 3 : -2]); // 前向 x，后段倒挡 v<0
  const s = buildHmiState({
    metrics: {
      scene: { ego: { x: 0, y: 0, heading: 0, speed: 4 }, trajectory_path: path },
      behavior: { state: 'LEFT_CHANGE', committed_lane: 2, target_lane: 1, lane_count: 4, upcoming_own_lanes: 2 },
    },
  });
  eq('变道方向 left', s.planning.laneChangeDir, 'left');
  eq('upcomingOwnLanes 2', s.planning.upcomingOwnLanes, 2);
  ok('轨迹被 clip（少于全量）', !!s.planning.traj && s.planning.traj.pts.length < path.length);
  ok('轨迹近场（末点 fwd < 80m）', s.planning.traj.pts[s.planning.traj.pts.length - 1][0] < 80);
  ok('轨迹含倒挡速度 v<0', s.planning.traj.pts.some((p) => p[2] < 0));
}

/* ── 15. blinker：掉头 → 空；MRM → hazard ── */
{
  const uturn = buildHmiState({
    metrics: { scene: { ego: { x: 0, y: 0, heading: 0, speed: 1 } }, behavior: { state: 'U_TURN', lane_count: 4, committed_lane: 2 } },
  });
  eq('掉头 blinker 空', uturn.driving.blinker, '');
  const mrm = buildHmiState({
    metrics: { scene: { ego: { x: 0, y: 0, heading: 0, speed: 1 } }, tor: { state: 'MRM' } },
  });
  eq('MRM blinker hazard', mrm.driving.blinker, 'hazard');
}

/* ── 16. 无异常且有数据 → 「全部正常」；完全无数据 → 无告警项 ── */
{
  const okS = buildHmiState({
    metrics: {
      scene: { ego: { x: 0, y: 0, heading: 0, speed: 5 } },
      control_debug: { mode: 'TRACK', speed: 5, hazard: false },
    },
  });
  eq('正常级别 ok', okS.alerts.level, 'ok');
  ok('显示全部正常', okS.alerts.items.some((it) => it.label.includes('全部正常')));
  const emptyS = buildHmiState({});
  eq('无数据 → 无告警项', emptyS.alerts.items.length, 0);
}

done();
