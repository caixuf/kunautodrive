/**
 * vis_driving_hud.test.mjs — 驾驶态势 HUD（vis/hud/DrivingStatus.js）契约
 *
 * 纯函数层：
 *   - 前车判定沿车头方向投影（掉头返程朝 −x 也必须找对前车，与 safety_control 同一几何）
 *   - 相邻车道 / 后方 / 超视距目标不算前车
 *   - TTC 只在接近时有限；风险分级阈值 1.5s / 3s
 *   - 控制模式按严重度取最高（MRM > SAFE > MANEUVER）
 *   - 缺字段不抛错，降级为 null / 未决策
 * 接线层（静态检查）：index.html 有 DOM、app.js 已接线 + 快捷键 + ?hud=0、CSS 在手动驾驶时让位。
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { ok, eq, done } from './test-utils.mjs';
import {
  behaviorInfo, controlModeInfo, findLeadVehicle, ttcRisk, oddInfo, computeDrivingStatus,
} from '../tools/flowboard/js/vis/hud/DrivingStatus.js';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const read = (p) => fs.readFileSync(path.join(repo, p), 'utf8');
const near = (a, b, eps = 1e-6) => Math.abs(a - b) < eps;

console.log('=== driving HUD: lead vehicle geometry ===\n');

const egoEast = { x: 0, y: -1.75, heading: 0, speed: 20, vx: 20, vy: 0, length: 4.6 };
const npc = (x, y, vx, extra = {}) => ({ type: 'car', id: x, x, y, vx, vy: 0, speed: Math.abs(vx), length: 4.6, ...extra });

{
  const lead = findLeadVehicle(egoEast, [npc(50, -1.75, 10)], { laneWidth: 3.5 });
  ok('same-lane car ahead is the lead', !!lead);
  ok('gap = center distance − two half lengths', near(lead.gap, 50 - 4.6));
  ok('closing = ego along − lead along', near(lead.closing, 10));
  ok('ttc = gap / closing', near(lead.ttc, (50 - 4.6) / 10));
}
{
  const lead = findLeadVehicle(egoEast, [npc(30, -1.75, 25)], { laneWidth: 3.5 });
  ok('pulling-away lead → ttc = ∞', lead && lead.ttc === Infinity);
}
ok('adjacent-lane car (lat 3.5m) is not the lead',
  findLeadVehicle(egoEast, [npc(20, 1.75, 0)], { laneWidth: 3.5 }) === null);
ok('car behind is not the lead',
  findLeadVehicle(egoEast, [npc(-20, -1.75, 30)], { laneWidth: 3.5 }) === null);
ok('car beyond horizon is not the lead',
  findLeadVehicle(egoEast, [npc(400, -1.75, 0)], { laneWidth: 3.5, horizon: 150 }) === null);
ok('ego entity itself is skipped',
  findLeadVehicle(egoEast, [{ type: 'ego', x: 10, y: -1.75 }], {}) === null);
{
  const lead = findLeadVehicle(egoEast, [npc(80, -1.75, 0), npc(40, -1.75, 0), npc(60, -1.75, 0)], {});
  ok('nearest of several same-lane cars wins', lead && near(lead.gap, 40 - 4.6));
}
{
  /* 掉头返程：ego 朝 −x（heading=π），前车在 x=−40，同向行驶 vx=−5。
   * 用世界 dx 判断会把它当"后方"漏掉——必须沿车头投影。 */
  const egoWest = { x: 0, y: 1.75, heading: Math.PI, speed: 15, vx: -15, vy: 0, length: 4.6 };
  const lead = findLeadVehicle(egoWest, [npc(-40, 1.75, -5)], { laneWidth: 3.5 });
  ok('return leg (heading=π): lead at −x is found', !!lead);
  ok('return leg: closing uses heading projection (15 − 5 = 10)', lead && near(lead.closing, 10, 1e-9));
  ok('return leg: car at +x is behind, not lead',
    findLeadVehicle(egoWest, [npc(40, 1.75, -5)], { laneWidth: 3.5 }) === null);
}
{
  const lead = findLeadVehicle(egoEast, [{ type: 'car', x: 30, y: -1.75, speed: 5, heading: 0, length: 4.6 }], {});
  ok('missing vx/vy falls back to speed projected on ego heading', lead && near(lead.leadSpeed, 5));
}
ok('null ego → null', findLeadVehicle(null, [npc(10, 0, 0)]) === null);
ok('non-array entities → null', findLeadVehicle(egoEast, null) === null);

console.log('\n=== driving HUD: risk / labels ===\n');

eq('no lead → none', ttcRisk(null).level, 'none');
eq('ttc 1.0s → danger', ttcRisk({ ttc: 1.0, gap: 10, closing: 10 }).level, 'danger');
eq('ttc 2.5s → caution', ttcRisk({ ttc: 2.5, gap: 25, closing: 10 }).level, 'caution');
eq('ttc 5s → safe', ttcRisk({ ttc: 5, gap: 50, closing: 10 }).level, 'safe');
eq('gap 1m while closing → danger', ttcRisk({ ttc: 10, gap: 1, closing: 0.1 }).level, 'danger');
eq('gap 1m but separating → safe', ttcRisk({ ttc: Infinity, gap: 1, closing: -1 }).level, 'safe');

eq('CRUISE → 巡航', behaviorInfo('CRUISE').label, '巡航');
eq('U_TURN → 掉头', behaviorInfo('U_TURN').label, '掉头');
eq('EMERGENCY tone bad', behaviorInfo('EMERGENCY').tone, 'bad');
eq('NA → 未决策', behaviorInfo('NA').label, '未决策');
eq('unknown state passes through', behaviorInfo('FOO_BAR').label, 'FOO_BAR');

eq('MRM+MANEUVER → MRM wins', controlModeInfo('MRM+MANEUVER').tone, 'bad');
eq('DATA_TIMEOUT+SAFE → bad', controlModeInfo('DATA_TIMEOUT+SAFE').tone, 'bad');
eq('HOLD+SAFE → warn', controlModeInfo('HOLD+SAFE').tone, 'warn');
eq('HOLD+MANEUVER → info', controlModeInfo('HOLD+MANEUVER').tone, 'info');
eq('HOLD → ok', controlModeInfo('HOLD').tone, 'ok');
eq('HOLD + hazard → warn', controlModeInfo('HOLD', true).tone, 'warn');
eq('empty mode → idle', controlModeInfo('').tone, 'idle');

eq('clear/day/1km → ODD 内', oddInfo({ weather: 'clear', lighting: 'day', visibility_m: 1000 }).label, 'ODD 内');
eq('rain → ODD 边缘', oddInfo({ weather: 'rain', lighting: 'day', visibility_m: 800 }).label, 'ODD 边缘');
eq('fog → 超出 ODD', oddInfo({ weather: 'fog', lighting: 'day', visibility_m: 800 }).label, '超出 ODD');
eq('visibility 100m → 超出 ODD', oddInfo({ weather: 'clear', visibility_m: 100 }).label, '超出 ODD');
eq('no env → ODD 未知', oddInfo({}).label, 'ODD 未知');

console.log('\n=== driving HUD: computeDrivingStatus ===\n');

{
  const st = computeDrivingStatus(undefined);
  ok('undefined metrics → hasData false, no throw', st.hasData === false && st.speed === null);
  eq('undefined metrics → behavior 未决策', st.behavior.label, '未决策');
}
{
  const metrics = {
    scene: {
      ego: egoEast, lane: { width: 3.5, count: 4 },
      weather: 'clear', lighting: 'day', visibility_m: 1000,
      entities: [{ type: 'ego', ...egoEast }, npc(30, -1.75, 15)],
      perception_entities: [npc(25, -1.75, 15)],
    },
    behavior: { state: 'FOLLOW', target_speed: 20, committed_lane: 1, lane_count: 4 },
    control_debug: { mode: 'HOLD+SAFE', throttle: 2, brake: -1, steer: 0.1, target_speed: 18 },
    planning_debug: { driver_mode: 'NOA:ACTIVE' },
  };
  const st = computeDrivingStatus(metrics);
  ok('hasData', st.hasData);
  ok('speedKmh = speed × 3.6', near(st.speedKmh, 72));
  ok('perceived entities preferred over truth', st.perceptionSource === '感知' && near(st.lead.gap, 25 - 4.6));
  ok('pedals clamped to [0,1]', st.throttle === 1 && st.brake === 0);
  ok('steer rad → deg', near(st.steerDeg, 0.1 * 180 / Math.PI));
  eq('behavior FOLLOW → 跟车', st.behavior.label, '跟车');
  eq('control HOLD+SAFE → 安全介入', st.control.label, '安全介入');
  eq('driver mode passthrough', st.driverMode, 'NOA:ACTIVE');
  ok('lane index/count', st.lane && st.lane.index === 1 && st.lane.count === 4);
  eq('cruise from behavior.target_speed', st.cruiseSpeed, 20);
  eq('command from control_debug.target_speed', st.commandSpeed, 18);
}
{
  const st = computeDrivingStatus({
    scene: { ego: egoEast, entities: [npc(30, -1.75, 15)], perception_entities: [] },
  });
  ok('empty perception list → fall back to truth entities', st.perceptionSource === '真值' && !!st.lead);
}

console.log('\n=== driving HUD: wiring ===\n');

const html = read('tools/flowboard/index.html');
const app = read('tools/flowboard/js/app.js');
const css = read('tools/flowboard/css/style.css');
const mod = read('tools/flowboard/js/vis/hud/DrivingStatus.js');

for (const id of ['drive-hud', 'dh-kmh', 'dh-beh', 'dh-ctl', 'dh-risk', 'dh-gap', 'dh-ttc', 'dh-thr', 'dh-brk', 'dh-odd']) {
  ok('index.html has #' + id, html.includes('id="' + id + '"'));
}
ok('app.js imports computeDrivingStatus', /import \{ computeDrivingStatus \} from '\.\/vis\/hud\/DrivingStatus\.js'/.test(app));
ok('app.js calls updateDriveHud from updateAll', /updateDriveHud\(false\)/.test(app));
ok('app.js binds shortcut I', app.includes("key === 'i'") && app.includes('toggleDriveHud()'));
ok('app.js honors ?hud=0', app.includes("params.get('hud') === '0'"));
ok('CSS hides HUD in manual game mode', css.includes('#game-hud.active~#drive-hud'));
ok('module uses Coord.forwardENU (no raw Math.sin/cos)', mod.includes("from '../math/Coord.js'") && !/Math\.(sin|cos)\(/.test(mod));

done();
