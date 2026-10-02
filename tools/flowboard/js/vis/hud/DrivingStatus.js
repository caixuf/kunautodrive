/**
 * DrivingStatus.js — 驾驶态势 HUD 的纯函数层（无 DOM、无 THREE，可单测）
 *
 * 把 monitor 透传的 metrics（scene / behavior / control_debug / planning_debug）
 * 归约成一个 HUD 视图模型：
 *   - 速度 / 目标速度 / 踏板 / 方向盘
 *   - 行为状态（behavior FSM）中文标签 + 色调
 *   - 控制模式（control→safety 的 raw.mode：MRM / SAFE / MANEUVER …）
 *   - 本车道前车 gap / 接近速度 / TTC 与风险等级
 *   - ODD 环境提示（天气 / 能见度 / 时段）
 *
 * 只做"可观测量的陈述"，不做任何控制决策（前端是纯观察者）。
 * 前车判定沿车头方向投影（与 safety_control 的 min_vehicle_ttc 同一套几何，
 * 掉头返程朝 −x 也成立），方向向量只走 Coord.forwardENU。
 */
import { forwardENU } from '../math/Coord.js';

/** 色调：HUD 用 CSS class `tone-<tone>` 上色 */
export const TONE = Object.freeze({
  OK: 'ok', INFO: 'info', WARN: 'warn', BAD: 'bad', IDLE: 'idle',
});

/** behavior_planner FSM 状态 → 中文标签 + 色调 */
const BEHAVIOR_LABELS = {
  CRUISE:         ['巡航', TONE.OK],
  FOLLOW:         ['跟车', TONE.INFO],
  LEFT_CHANGE:    ['左变道', TONE.INFO],
  RIGHT_CHANGE:   ['右变道', TONE.INFO],
  OVERTAKE_LEFT:  ['左侧超车', TONE.INFO],
  OVERTAKE_RIGHT: ['右侧超车', TONE.INFO],
  U_TURN:         ['掉头', TONE.INFO],
  UTURN_TRIGGER:  ['准备掉头', TONE.INFO],
  YIELD:          ['让行', TONE.WARN],
  STOP:           ['停车', TONE.WARN],
  BLOCKED:        ['受阻', TONE.WARN],
  LOST_LEAD:      ['前车丢失', TONE.WARN],
  EMERGENCY:      ['紧急', TONE.BAD],
};

export function behaviorInfo(state) {
  const key = typeof state === 'string' ? state.trim().toUpperCase() : '';
  if (!key || key === 'NA') return { key: '', label: '未决策', tone: TONE.IDLE };
  const hit = BEHAVIOR_LABELS[key];
  return hit ? { key, label: hit[0], tone: hit[1] } : { key, label: key, tone: TONE.INFO };
}

/**
 * 控制模式（control/raw_cmd 的 mode 字符串，safety 会追加 "+SAFE" 等后缀）。
 * 按严重度取最高：MRM / DATA_TIMEOUT > SAFE / ROAD_GUARD / BRAKE > MANEUVER > 常规。
 */
export function controlModeInfo(mode, hazard) {
  const m = typeof mode === 'string' ? mode.toUpperCase() : '';
  if (m.includes('MRM'))          return { label: 'MRM 最小风险', tone: TONE.BAD };
  if (m.includes('DATA_TIMEOUT')) return { label: '数据超时', tone: TONE.BAD };
  if (m.includes('SAFE'))         return { label: '安全介入', tone: TONE.WARN };
  if (m.includes('ROAD_GUARD'))   return { label: '路沿保护', tone: TONE.WARN };
  if (m.includes('BRAKE'))        return { label: '制动', tone: TONE.WARN };
  if (m.includes('MANEUVER'))     return { label: '机动执行', tone: TONE.INFO };
  if (hazard)                     return { label: '危险告警', tone: TONE.WARN };
  if (!m || m === 'NONE')         return { label: '—', tone: TONE.IDLE };
  return { label: '轨迹跟随', tone: TONE.OK };
}

function num(v, fallback) {
  if (v === null || v === undefined || v === '') return fallback;
  const n = Number(v);
  return Number.isFinite(n) ? n : fallback;
}

/**
 * 本车道前车：沿车头方向投影，取 ahead>0 且 |lat| < 半车道宽 的最近者。
 * gap = 车头到车尾的净距（中心距 − 两半车长）。TTC 只在接近（closing>0）时有限。
 *
 * @returns {null | {id, type, gap, closing, ttc, leadSpeed}}
 */
export function findLeadVehicle(ego, entities, opts = {}) {
  if (!ego || !Array.isArray(entities)) return null;
  const ex = num(ego.x, NaN), ey = num(ego.y, NaN);
  if (!Number.isFinite(ex) || !Number.isFinite(ey)) return null;
  const [fx, fy] = forwardENU(num(ego.heading, 0));
  const halfLane = num(opts.laneWidth, 3.5) / 2;
  const horizon = num(opts.horizon, 150);
  const egoHalfLen = num(ego.length, 4.6) / 2;
  const egoAlong = num(ego.vx, NaN) * fx + num(ego.vy, NaN) * fy;
  const egoSpeed = Number.isFinite(egoAlong) ? egoAlong : num(ego.speed, 0);

  let best = null;
  for (const e of entities) {
    if (!e || e === ego || e.type === 'ego') continue;
    const dx = num(e.x, NaN) - ex, dy = num(e.y, NaN) - ey;
    if (!Number.isFinite(dx) || !Number.isFinite(dy)) continue;
    const ahead = dx * fx + dy * fy;
    const lat = -dx * fy + dy * fx;
    if (ahead <= 0 || ahead > horizon || Math.abs(lat) >= halfLane) continue;
    const gap = Math.max(0, ahead - egoHalfLen - num(e.length, 4.6) / 2);
    if (best && gap >= best.gap) continue;
    let along = num(e.vx, NaN) * fx + num(e.vy, NaN) * fy;
    if (!Number.isFinite(along)) {
      const [ox, oy] = forwardENU(num(e.heading, 0));
      along = num(e.speed, 0) * (ox * fx + oy * fy);
    }
    const closing = egoSpeed - along;
    best = {
      id: e.id, type: e.type || 'obstacle', gap, closing, leadSpeed: along,
      ttc: closing > 0.1 ? gap / closing : Infinity,
    };
  }
  return best;
}

/** TTC 风险分级：<1.5s 危险，<3s 注意；净距 <2m 且仍在接近也算危险 */
export function ttcRisk(lead) {
  if (!lead) return { level: 'none', label: '前方畅通', tone: TONE.OK };
  if (lead.ttc < 1.5 || (lead.gap < 2 && lead.closing > 0)) {
    return { level: 'danger', label: '碰撞风险', tone: TONE.BAD };
  }
  if (lead.ttc < 3.0) return { level: 'caution', label: '注意前车', tone: TONE.WARN };
  return { level: 'safe', label: '安全跟随', tone: TONE.OK };
}

const WEATHER_LABELS = {
  clear: ['晴', TONE.OK], cloudy: ['多云', TONE.OK], overcast: ['阴', TONE.OK],
  rain: ['雨', TONE.WARN], snow: ['雪', TONE.BAD], storm: ['雷暴', TONE.BAD],
  fog: ['雾', TONE.BAD], sandstorm: ['沙尘', TONE.BAD],
};
const LIGHTING_LABELS = {
  dawn: ['拂晓', TONE.OK], day: ['白天', TONE.OK], afternoon: ['午后', TONE.OK],
  dusk: ['黄昏', TONE.WARN], night: ['夜间', TONE.WARN],
};
const TONE_RANK = { ok: 0, idle: 0, info: 0, warn: 1, bad: 2 };

/**
 * ODD 环境提示（仅前端观测提示，不是系统的 ODD 判定；
 * 真正的 ODD 监控节点见 docs/ROADMAP_L3.md 方向六）。
 */
export function oddInfo(scene) {
  const s = scene || {};
  const items = [];
  const w = WEATHER_LABELS[String(s.weather || '').toLowerCase()];
  if (w) items.push({ label: w[0], tone: w[1] });
  const l = LIGHTING_LABELS[String(s.lighting || '').toLowerCase()];
  if (l) items.push({ label: l[0], tone: l[1] });
  const vis = num(s.visibility_m, NaN);
  if (Number.isFinite(vis)) {
    const tone = vis < 150 ? TONE.BAD : (vis < 400 ? TONE.WARN : TONE.OK);
    const txt = vis >= 1000 ? (vis / 1000).toFixed(1) + 'km' : Math.round(vis) + 'm';
    items.push({ label: '能见度 ' + txt, tone });
  }
  if (items.length === 0) return { items, tone: TONE.IDLE, label: 'ODD 未知' };
  const worst = items.reduce((r, it) => Math.max(r, TONE_RANK[it.tone] || 0), 0);
  if (worst >= 2) return { items, tone: TONE.BAD, label: '超出 ODD' };
  if (worst === 1) return { items, tone: TONE.WARN, label: 'ODD 边缘' };
  return { items, tone: TONE.OK, label: 'ODD 内' };
}

/**
 * 顶层归约：metrics → HUD 视图模型。缺字段一律降级为 null / '—'，不抛错。
 */
export function computeDrivingStatus(metrics) {
  const m = metrics || {};
  const scene = m.scene || {};
  const ego = scene.ego || null;
  const beh = m.behavior || {};
  const cd = m.control_debug || {};
  const pd = m.planning_debug || {};
  const lane = scene.lane || {};

  const perceived = Array.isArray(scene.perception_entities) && scene.perception_entities.length
    ? scene.perception_entities : null;
  const entities = perceived || (Array.isArray(scene.entities) ? scene.entities : []);
  const laneWidth = num(lane.width, num(beh.lane_width, 3.5));
  const lead = findLeadVehicle(ego, entities, { laneWidth });

  const speed = num(ego && ego.speed, num(cd.speed, null));
  const laneCount = num(beh.lane_count, num(lane.count, 0));
  const committed = num(beh.committed_lane, -1);
  let driverMode = '';
  if (typeof pd.driver_mode === 'string' && pd.driver_mode) driverMode = pd.driver_mode;
  else if (typeof m.driver_mode === 'string') driverMode = m.driver_mode;

  return {
    hasData: !!(ego || m.control_debug || m.behavior),
    speed,
    speedKmh: speed === null ? null : speed * 3.6,
    cruiseSpeed: num(beh.target_speed, num(pd.command_speed, null)),
    commandSpeed: num(cd.target_speed, null),
    throttle: Math.max(0, Math.min(1, num(cd.throttle, num(ego && ego.throttle, 0)))),
    brake: Math.max(0, Math.min(1, num(cd.brake, num(ego && ego.brake, 0)))),
    steerDeg: num(cd.steer, num(ego && ego.steer, 0)) * 180 / Math.PI,
    behavior: behaviorInfo(beh.state),
    control: controlModeInfo(cd.mode, !!cd.hazard),
    driverMode,
    lane: laneCount > 0 && committed >= 0 ? { index: committed, count: laneCount } : null,
    lead,
    risk: ttcRisk(lead),
    perceptionSource: perceived ? '感知' : '真值',
    odd: oddInfo(scene),
  };
}
