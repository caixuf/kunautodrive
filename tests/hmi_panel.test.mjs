/**
 * hmi_panel.test.mjs — HMI 面板 DOM 层冒烟（无浏览器，用最小 document stub）
 *
 * 只验证 HmiPanel 的 DOM 写入契约：
 *  1. initHmiPanel 幂等（_hmiInit 置位，二次不重建）
 *  2. updateHmiPanel 把模型状态写进正确的元素（速度/行为/意图/计数）
 *  3. 空状态不抛错，降级占位
 *
 * 逻辑正确性在 hmi_model.test.mjs；这里只挡"DOM 层抛错/写错元素"。
 * 跑法：node tests/hmi_panel.test.mjs
 */

import { buildHmiState } from '../tools/flowboard/js/vis/hud/HmiModel.js';
import { initHmiPanel, updateHmiPanel } from '../tools/flowboard/js/vis/hud/HmiPanel.js';
import { ok, eq, done } from './test-utils.mjs';

console.log('=== HMI 面板 DOM 层冒烟 ===\n');

/* ── 最小 document stub：getElementById 按 id 复用一个元素 ── */
const _els = {};
const _fakeCtx = {
  fillStyle: '', strokeStyle: '', lineWidth: 0,
  clearRect() {}, fillRect() {}, beginPath() {}, moveTo() {},
  lineTo() {}, closePath() {}, fill() {}, stroke() {}, arc() {},
};
function _mkEl() {
  return {
    _t: '',
    _inner: '',
    _children: [],
    get textContent() { return this._t; },
    set textContent(v) { this._t = String(v); },
    get innerHTML() { return this._inner; },
    set innerHTML(v) { this._inner = String(v); if (v === '') this._children = []; },
    className: '',
    style: {},
    width: 120,
    height: 150,
    getContext() { return _fakeCtx; },
    appendChild(c) { this._children.push(c); },
    querySelector() { return null; },
  };
}
globalThis.document = {
  getElementById(id) { return _els[id] || (_els[id] = _mkEl()); },
  createElement() { return _mkEl(); },
  querySelector() { return null; },
};

const body = _mkEl();

/* 1. init 幂等 */
initHmiPanel(body);
ok('init 置位 _hmiInit', body._hmiInit === true);
ok('init 注入四分区骨架', body.innerHTML.includes('hmi-grid') && body.innerHTML.includes('驾驶核心'));
const firstHtml = body.innerHTML;
initHmiPanel(body);
ok('init 二次不重建（幂等）', body.innerHTML === firstHtml);

/* 2. 满数据写入正确元素 */
const fullTopo = {
  metrics: {
    scene: {
      ego: { x: 0, y: 0, heading: 0, speed: 10, pitch: 0, roll: 0 },
      lane: { width: 3.5, count: 3 },
      entities: [{}, {}, {}, {}],
      perception_entities: [{}, {}],
    },
    behavior: { state: 'CRUISE', target_speed: 12, lane_count: 3, committed_lane: 1 },
    control_debug: { speed: 10, mode: 'TRACK', throttle: 0.3, brake: 0.0, steer: 0.02, hazard: false },
    planning_debug: { active_scenario: 'LANE_FOLLOW', active_stage: 'CRUISE', command_speed: 12 },
    bev: { count: 3 },
  },
};
updateHmiPanel(body, buildHmiState(fullTopo));
eq('速度写 hmi-speed-kmh', _els['hmi-speed-kmh'].textContent, '36');
eq('行为写 hmi-beh', _els['hmi-beh'].textContent, '巡航');
eq('行为 tone 正常', _els['hmi-beh'].className, 'hmi-chip tone-ok');
eq('意图写 hmi-intent', _els['hmi-intent'].textContent, '车道保持');
eq('感知计数写 hmi-perc-count', _els['hmi-perc-count'].textContent, '2');
eq('无栅格占位提示', _els['hmi-occ'].textContent.includes('等待数据'), true);

/* 3b. 真占用栅格：canvas 绘制不抛错 + 文案更新 */
{
  const b64 = Buffer.from([1, 2, 1, 0]).toString('base64');
  const occTopo = {
    metrics: {
      occupancy: {
        grid_w: 2, grid_h: 2, cell_size_m: 0.5, origin_x: 0, origin_y: -0.5,
        frame: 'body', occupied_cells: 1, free_cells: 2, cells_b64: b64,
      },
    },
  };
  let threw2 = false;
  try { updateHmiPanel(body, buildHmiState(occTopo)); } catch (_) { threw2 = true; }
  ok('占用栅格绘制不抛错', !threw2);
  eq('占用栅格文案含占用数', _els['hmi-occ'].textContent.includes('占用 1'), true);
  eq('占用栅格文案含尺寸', _els['hmi-occ'].textContent.includes('2×2'), true);
}

/* 3. 空状态不抛错、降级占位 */
let threw = false;
try { updateHmiPanel(body, buildHmiState({})); } catch (_) { threw = true; }
ok('空状态不抛错', !threw);
eq('空状态速度占位', _els['hmi-speed-kmh'].textContent, '--');
eq('空状态意图占位', _els['hmi-intent'].textContent, '—');

/* 4. 规划意图增强：车道梯 / 并道 / 间隙 / 轨迹 canvas 不抛错 */
{
  const planTopo = {
    metrics: {
      scene: {
        ego: { x: 0, y: 0, heading: 0, speed: 6 },
        trajectory_path: [[0, 0, 5], [10, 0, 5], [20, 2, 3]],
      },
      behavior: {
        state: 'RIGHT_CHANGE', committed_lane: 2, target_lane: 3, lane_count: 4,
        left_ok: true, right_ok: false, left_gap: 5.5, right_gap: -1,
        dist_to_lane_drop_m: 120, upcoming_own_lanes: -1,
      },
    },
  };
  let threw = false;
  try { updateHmiPanel(body, buildHmiState(planTopo)); } catch (_) { threw = true; }
  ok('规划意图绘制不抛错', !threw);
  eq('车道梯 当前 L3/4', _els['hmi-lane-cap'].textContent.includes('当前 L3/4'), true);
  eq('车道梯 目标 L4', _els['hmi-lane-cap'].textContent.includes('目标 L4'), true);
  eq('并道提示', _els['hmi-drop'].textContent.includes('并道'), true);
  eq('间隙提示 左 ✓', _els['hmi-gaps'].textContent.includes('左 ✓'), true);
}

/* 5. 告警横幅 + 转向指示 */
{
  const alertTopo = {
    metrics: {
      scene: { ego: { x: 0, y: 0, heading: 0, speed: 2 } },
      behavior: { state: 'LEFT_CHANGE', lane_count: 4, committed_lane: 2 },
      control_debug: { mode: 'TRACK', speed: 2, hazard: false },
      tor: { state: 'MRM' },
      degrade: { active: true, reason_name: 'x' },
    },
  };
  let threw = false;
  try { updateHmiPanel(body, buildHmiState(alertTopo)); } catch (_) { threw = true; }
  ok('告警渲染不抛错', !threw);
  eq('告警横幅 tone-bad', _els['hmi-alerts'].className, 'hmi-alerts tone-bad');
  ok('告警至少 2 条 chip（MRM+降级）', (_els['hmi-alerts']._children || []).length >= 2);
  eq('左变道显示 blinker', _els['hmi-blinker'].textContent, '◀ 左');
}

/* 6. 转向三态 / 车道梯计数 / 图例 / 空告警占位 */
{
  // RIGHT_CHANGE → 右 ▶；4 车道方框
  updateHmiPanel(body, buildHmiState({
    metrics: {
      scene: { ego: { x: 0, y: 0, heading: 0, speed: 2 } },
      behavior: { state: 'RIGHT_CHANGE', committed_lane: 1, target_lane: 2, lane_count: 4 },
      control_debug: { mode: 'TRACK', speed: 2, hazard: false },
    },
  }));
  eq('右变道 blinker', _els['hmi-blinker'].textContent, '右 ▶');
  eq('车道梯 4 个方框', (_els['hmi-lanes']._children || []).length, 4);
  ok('占用图例存在', (body.innerHTML || '').includes('hmi-occ-legend'));

  // MRM → ⚠ 双闪
  updateHmiPanel(body, buildHmiState({
    metrics: { scene: { ego: { x: 0, y: 0, heading: 0, speed: 2 } }, tor: { state: 'MRM' } },
  }));
  eq('MRM blinker 双闪', _els['hmi-blinker'].textContent, '⚠ 双闪');

  // CRUISE → blinker 隐藏
  updateHmiPanel(body, buildHmiState({
    metrics: {
      scene: { ego: { x: 0, y: 0, heading: 0, speed: 2 } },
      behavior: { state: 'CRUISE', lane_count: 4, committed_lane: 2 },
      control_debug: { mode: 'TRACK', speed: 2, hazard: false },
    },
  }));
  eq('巡航 blinker 隐藏', _els['hmi-blinker'].style.display, 'none');

  // 空数据 → 告警占位 '—'，无 chip
  updateHmiPanel(body, buildHmiState({}));
  eq('空告警占位', _els['hmi-alerts'].textContent, '—');
  eq('空告警无 chip', (_els['hmi-alerts']._children || []).length, 0);
}

done();
