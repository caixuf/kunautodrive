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
  behaviorInfo, controlModeInfo, findLeadVehicle, ttcRisk, oddInfo, torInfo, computeDrivingStatus,
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

console.log('\n=== driving HUD: torInfo ===\n');

eq('tor undefined → L3 关闭', torInfo(undefined).label, 'L3 关闭');
eq('tor undefined tone → idle', torInfo(undefined).tone, 'idle');
eq('tor null state → empty', torInfo(null).state, '');
eq('tor null countdown_s → 0', torInfo(null).countdown_s, 0);
eq('tor null reason → empty', torInfo(null).reason, '');
ok('tor null dms → null', torInfo(null).dms === null);
eq('tor empty object → L3 关闭', torInfo({}).label, 'L3 关闭');
eq('tor OFF → L3 关闭', torInfo({ state: 'OFF' }).label, 'L3 关闭');
eq('tor OFF tone → idle', torInfo({ state: 'OFF' }).tone, 'idle');
eq('tor READY → L3 就绪', torInfo({ state: 'READY' }).label, 'L3 就绪');
eq('tor READY tone → idle', torInfo({ state: 'READY' }).tone, 'idle');
eq('tor ACTIVE → L3 激活', torInfo({ state: 'ACTIVE' }).label, 'L3 激活');
eq('tor ACTIVE tone → ok', torInfo({ state: 'ACTIVE' }).tone, 'ok');

{
  const tr = torInfo({ state: 'TOR_REQUESTED', countdown_s: 7.23, reason: 'visibility' });
  eq('tor TOR_REQUESTED label with countdown', tr.label, '接管请求 (7.2s)');
  eq('tor TOR_REQUESTED tone → bad', tr.tone, 'bad');
  eq('tor TOR_REQUESTED reason passthrough', tr.reason, 'visibility');
  ok('tor TOR_REQUESTED countdown numeric', near(tr.countdown_s, 7.23));
}
{
  const dt = torInfo({
    state: 'DRIVER_TAKEOVER',
    dms: { attention: true, will_takeover: true, takeover_latency_s: 1.5 },
  });
  eq('tor DRIVER_TAKEOVER → 驾驶员接管', dt.label, '驾驶员接管');
  eq('tor DRIVER_TAKEOVER tone → info', dt.tone, 'info');
  ok('tor dms passthrough', dt.dms && dt.dms.attention === true && dt.dms.will_takeover === true);
}
{
  const mrm = torInfo({ state: 'MRM', reason: 'timeout' });
  eq('tor MRM → MRM 风险减速', mrm.label, 'MRM 风险减速');
  eq('tor MRM tone → bad', mrm.tone, 'bad');
  eq('tor MRM reason passthrough', mrm.reason, 'timeout');
}

console.log('\n=== driving HUD: oddInfo authoritative ===\n');

eq('empty oddMetric falls back to scene', oddInfo({ weather: 'clear', visibility_m: 1000 }, {}).label, 'ODD 内');
{
  const o1 = oddInfo({ weather: 'clear' }, { in_odd: true, violations: [], visibility_m: 1200 });
  eq('authoritative odd in_odd true → ODD 域内', o1.label, 'ODD 域内');
  eq('authoritative odd in_odd true tone → ok', o1.tone, 'ok');
  ok('odd items preserves visibility', o1.items.some((it) => it.label === '能见度 1.2km' && it.tone === 'ok'));
}
{
  const o2 = oddInfo(null, { in_odd: false, violations: ['visibility'], visibility_m: 80 });
  eq('authoritative odd in_odd false → 超出 ODD', o2.label, '超出 ODD');
  eq('authoritative odd in_odd false tone → bad', o2.tone, 'bad');
  ok('violation visibility mapped to 能见度超限', o2.items.some((it) => it.label === '能见度超限' && it.tone === 'bad'));
}
{
  const viols = ['loc_diverged', 'loc_untrustworthy', 'loc_stale', 'route_stale', 'degraded', 'weather'];
  const o3 = oddInfo(null, { in_odd: false, violations: viols });
  eq('all violations mapped', o3.label, '超出 ODD');
  const mapped = o3.items.map((it) => it.label);
  ok('loc_diverged mapped', mapped.includes('定位发散'));
  ok('loc_untrustworthy mapped', mapped.includes('定位不可信'));
  ok('loc_stale mapped', mapped.includes('定位超时'));
  ok('route_stale mapped', mapped.includes('路线超时'));
  ok('degraded mapped', mapped.includes('系统降级'));
  ok('weather mapped', mapped.includes('天气超限'));
}
{
  const o4 = oddInfo(null, { in_odd: false, violations: [] });
  eq('in_odd false with empty violations → 超出 ODD', o4.label, '超出 ODD');
  eq('in_odd false tone → bad', o4.tone, 'bad');
}

console.log('\n=== driving HUD: computeDrivingStatus ===\n');

{
  const st = computeDrivingStatus(undefined);
  ok('undefined metrics → hasData false, no throw', st.hasData === false && st.speed === null);
  eq('undefined metrics → behavior 未决策', st.behavior.label, '未决策');
  eq('undefined metrics → tor L3 关闭', st.tor.label, 'L3 关闭');
  eq('undefined metrics → odd ODD 未知', st.odd.label, 'ODD 未知');
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
  const l3Metrics = {
    scene: { ego: egoEast, weather: 'clear', visibility_m: 1000 },
    odd: {
      in_odd: false,
      violations: ['visibility'],
      visibility_m: 80,
    },
    tor: {
      state: 'TOR_REQUESTED',
      countdown_s: 5.5,
      reason: 'visibility',
      dms: { attention: true, will_takeover: true, takeover_latency_s: 1.2 },
    },
  };
  const st = computeDrivingStatus(l3Metrics);
  ok('computeDrivingStatus exports tor', !!st.tor);
  eq('st.tor.label reflects countdown', st.tor.label, '接管请求 (5.5s)');
  eq('st.tor.tone is bad', st.tor.tone, 'bad');
  eq('st.tor.reason is visibility', st.tor.reason, 'visibility');
  ok('st.tor.dms attention is true', st.tor.dms && st.tor.dms.attention === true);
  eq('st.odd.label is 超出 ODD', st.odd.label, '超出 ODD');
  eq('st.odd.tone is bad', st.odd.tone, 'bad');
  ok('st.odd.items has violation', st.odd.items.some((it) => it.label === '能见度超限'));
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

for (const id of ['drive-hud', 'dh-kmh', 'dh-beh', 'dh-ctl', 'dh-risk', 'dh-gap', 'dh-ttc', 'dh-thr', 'dh-brk', 'dh-odd', 'dh-tor']) {
  ok('index.html has #' + id, html.includes('id="' + id + '"'));
}
ok('app.js imports computeDrivingStatus', /import \{ computeDrivingStatus \} from '\.\/vis\/hud\/DrivingStatus\.js'/.test(app));
ok('app.js calls updateDriveHud from updateAll', /updateDriveHud\(false\)/.test(app));
ok('app.js handles st.tor', app.includes('st.tor'));
ok('app.js binds shortcut I', app.includes("key === 'i'") && app.includes('toggleDriveHud()'));
ok('app.js honors ?hud=0', app.includes("params.get('hud') === '0'"));
ok('CSS hides HUD in manual game mode', css.includes('#game-hud.active~#drive-hud'));
ok('module uses Coord.forwardENU (no raw Math.sin/cos)', mod.includes("from '../math/Coord.js'") && !/Math\.(sin|cos)\(/.test(mod));

done();
