/**
 * HmiModel.js — HMI 专区纯模型层（无 DOM、无 THREE，可单测）
 *
 * 把 monitor 透传的 `topo`（{ metrics }）归约成 HMI 面板的规范化状态：
 *   driving  —— 速度 / 目标 / 踏板 / 行为 / 控制模式 / 车道 / 前车 / 信号灯
 *   perception —— 感知源、地面真值计数、BEV 目标数、占用栅格（HMI-2 接入）
 *   planning —— 行为阶段、"车想干什么"意图文本、P2P 阶段
 *   diag     —— 定位模态、TOR、L3 事件、ODD
 *
 * 唯一事实源：真实数据从 `computeDrivingStatus(metrics)` 复用，**不重算**几何/前车，
 * 避免与驾驶 HUD 出现第二份实现（本仓"同一功能第二份实现=违规"铁律）。
 *
 * 纯观察者：只陈述可观测量，不做任何控制决策；缺字段一律降级为占位符，绝不抛错。
 */
import { computeDrivingStatus } from './DrivingStatus.js';
import { forwardENU } from '../math/Coord.js';

/** behavior FSM key → HMI 机动码（用于意图文本与转向指示） */
const MANEUVER_BY_KEY = {
  LEFT_CHANGE: 'left', OVERTAKE_LEFT: 'left',
  RIGHT_CHANGE: 'right', OVERTAKE_RIGHT: 'right',
  U_TURN: 'uturn', UTURN_TRIGGER: 'uturn',
  STOP: 'stop', YIELD: 'yield',
  FOLLOW: 'follow', LOST_LEAD: 'follow', CRUISE: 'cruise',
  BLOCKED: 'blocked', EMERGENCY: 'emergency',
};

/** 机动码 → "车想干什么" 文本 */
const INTENT_BY_MANEUVER = {
  left: '向左变道', right: '向右变道', uturn: '掉头',
  stop: '停止让行', yield: '减速让行', follow: '跟车',
  cruise: '车道保持', blocked: '受阻停车', emergency: '紧急处置', '': '—',
};

function _count(arr) {
  return Array.isArray(arr) ? arr.length : 0;
}

/** base64 → Uint8Array。浏览器用 atob，Node 用 Buffer；都没有则返回 null。 */
function _decodeB64(s) {
  if (typeof s !== 'string' || !s) return null;
  try {
    if (typeof globalThis.atob === 'function') {
      const bin = globalThis.atob(s);
      const a = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) a[i] = bin.charCodeAt(i);
      return a;
    }
    if (globalThis.Buffer && typeof globalThis.Buffer.from === 'function') {
      return new Uint8Array(globalThis.Buffer.from(s, 'base64'));
    }
  } catch (_) { /* 畸形 base64 → 当作无栅格 */ }
  return null;
}

/**
 * @param {object} topo  monitor 透传的拓扑对象（含 metrics）
 * @returns {object} 规范化 HMI 状态
 */
export function buildHmiState(topo) {
  const metrics = (topo && topo.metrics) || {};
  const st = computeDrivingStatus(metrics);
  const scene = metrics.scene || {};

  /* ── 驾驶核心 ── */
  const maneuver = MANEUVER_BY_KEY[st.behavior && st.behavior.key] || '';
  const laneText = st.lane ? `${st.lane.index}/${st.lane.count}` : '--';
  const leadGapText = st.lead && Number.isFinite(st.lead.gap)
    ? `${st.lead.gap.toFixed(1)} m` : '—';
  const leadTtcText = st.lead && Number.isFinite(st.lead.ttc)
    ? `${st.lead.ttc.toFixed(1)} s` : '∞';
  const cruiseKmh = Number.isFinite(st.cruiseSpeed) ? st.cruiseSpeed * 3.6 : null;
  const tl = st.trafficLight && st.trafficLight.visible ? st.trafficLight : null;

  /* ── 感知 / 环境 ── */
  /* 占用栅格来源：monitor 把 traversability 透传到 metrics.occupancy（顶层，
   * 与 lane_match 同级）；兼容早期放在 metrics.scene.occupancy 的形态。 */
  const occSrc = (metrics.occupancy && typeof metrics.occupancy === 'object')
    ? metrics.occupancy
    : (scene.occupancy && typeof scene.occupancy === 'object' ? scene.occupancy : null);
  let occ = null;
  if (occSrc && Number(occSrc.grid_w) > 0) {
    const gw = Number(occSrc.grid_w) || 0;
    const gh = Number(occSrc.grid_h) || 0;
    const cells = _decodeB64(occSrc.cells_b64);
    occ = {
      grid_w: gw,
      grid_h: gh,
      cell_size_m: Number(occSrc.cell_size_m) || 0,
      origin_x: Number(occSrc.origin_x) || 0,
      origin_y: Number(occSrc.origin_y) || 0,
      frame: occSrc.frame || 'body',
      occupied: Number(occSrc.occupied_cells) || 0,
      free: Number(occSrc.free_cells) || 0,
      /* cells 索引 = gx*grid_h + gy（gx 前向，gy 横向）；解码失败为 null */
      cells: cells && cells.length === gw * gh ? cells : null,
    };
  }

  /* ── 规划意图 ── */
  const p2p = st.p2p || {};
  const p2pText = p2p.isP2P
    ? `${p2p.stageLabel || ''}${p2p.slotText ? ' · ' + p2p.slotText : ''}`.trim()
    : '—';

  /* ── 规划意图增强：目标车道 / 变道可行 / 并道距离 / 轨迹片段 ──
   * 全部来自 metrics.behavior（behavior_planner 真实输出）+ scene.trajectory_path，
   * 不在前端重算规划，只做相对坐标归约（见下）。 */
  const mb = metrics.behavior || {};
  const laneCountPlan = Number.isFinite(Number(mb.lane_count)) ? Number(mb.lane_count)
    : (st.lane ? st.lane.count : 0);
  const currentLane = (Number(mb.committed_lane) >= 0) ? Number(mb.committed_lane)
    : (st.lane ? st.lane.index : -1);
  const targetLane = (Number(mb.target_lane) >= 0) ? Number(mb.target_lane) : -1;
  let laneChangeDir = '';
  if (targetLane >= 0 && currentLane >= 0 && targetLane !== currentLane) {
    laneChangeDir = targetLane > currentLane ? 'right' : 'left';
  } else if (maneuver === 'left' || maneuver === 'right') {
    laneChangeDir = maneuver;
  }
  const routeLaneGoal = Number.isFinite(Number(mb.route_lane_goal)) ? Number(mb.route_lane_goal) : -1;
  let distToLaneDropM = Number(mb.dist_to_lane_drop_m);
  if (!Number.isFinite(distToLaneDropM) || distToLaneDropM < 0 || distToLaneDropM > 5000) distToLaneDropM = null;
  const leftGapM = Number.isFinite(Number(mb.left_gap)) ? Number(mb.left_gap) : null;
  const rightGapM = Number.isFinite(Number(mb.right_gap)) ? Number(mb.right_gap) : null;

  /* 轨迹片段：世界 ENU → ego 相对（前向 fwd / 右向 right / 速度 v），供面板画 mini path。 */
  let traj = null;
  const ego = scene.ego || null;
  const trajPath = Array.isArray(scene.trajectory_path) ? scene.trajectory_path : [];
  if (ego && trajPath.length >= 2) {
    const ex = Number(ego.x), ey = Number(ego.y);
    if (Number.isFinite(ex) && Number.isFinite(ey)) {
      const [c, s] = forwardENU(Number(ego.heading) || 0); // c=cos, s=sin（唯一合法入口）
      const pts = [];
      for (let i = 0; i < trajPath.length; i++) {
        const p = trajPath[i];
        if (!Array.isArray(p) || p.length < 2) continue;
        const dx = Number(p[0]) - ex, dy = Number(p[1]) - ey;
        if (!Number.isFinite(dx) || !Number.isFinite(dy)) continue;
        pts.push([dx * c + dy * s, -dx * s + dy * c, Number(p[2]) || 0]);
      }
      /* 只保留近场片段（≤60m），否则整条 290m 路径把 mini canvas 压扁。 */
      const clip = [];
      for (let i = 0; i < pts.length; i++) {
        clip.push(pts[i]);
        if (pts[i][0] > 60) break;
      }
      if (clip.length >= 2) traj = { pts: clip };
    }
  }

  /* ── 态势 / 诊断 ── */
  const tor = st.tor || { state: '', label: 'L3 关闭', tone: 'idle' };
  const odd = st.odd || { label: '—', tone: 'idle', items: [] };
  const l3Count = _count(st.l3Events);

  /* ── 告警聚合（安全 HMI 顶栏）：TOR/MRM > 降级 > 故障 > 控制告警 > 碰撞风险
   * > 受阻 > ODD > L3 事件。只陈述可观测量，按严重度取最高。 ── */
  const degrade = metrics.degrade || {};
  const safetyEv = metrics.safety_evidence || {};
  const oddRaw = metrics.odd || {};
  const alertItems = [];
  const pushAlert = (label, tone) => {
    if (label && !alertItems.some((it) => it.label === label)) alertItems.push({ label, tone });
  };
  const torState = (st.tor && st.tor.state) || '';
  if (torState === 'TOR_REQUESTED') {
    pushAlert('接管请求 ' + Math.max(0, Math.round(Number(st.tor.countdown_s) || 0)) + 's', 'bad');
  } else if (torState === 'MRM') {
    pushAlert('MRM 最小风险', 'bad');
  } else if (torState === 'ACTIVE') {
    pushAlert('L3 激活', 'ok');
  }
  if (degrade.active) pushAlert('降级: ' + (degrade.reason_name || degrade.reason || 'unknown'), 'bad');
  /* safety_evidence.fault 是结构体 {id,type,component,injected}，type/id="none" 表示无故障 */
  const seFault = safetyEv.fault || {};
  const seFaultType = (typeof seFault === 'string') ? seFault : (seFault.type || seFault.id || '');
  if (seFaultType && seFaultType !== 'none') {
    pushAlert('故障: ' + seFaultType + (seFault.component && seFault.component !== 'none' ? '@' + seFault.component : ''), 'bad');
  }
  const seAction = safetyEv.action || {};
  if (seAction.immediate_stop) pushAlert('紧急制动', 'bad');
  else if (seAction.mrm_stop) pushAlert('MRM 停车', 'bad');
  if (seAction.disable_lane_change) pushAlert('禁用变道', 'warn');
  if (st.control && st.control.tone === 'bad') pushAlert(st.control.label, 'bad');
  else if (st.control && st.control.tone === 'warn') pushAlert(st.control.label, 'warn');
  if (st.risk && st.risk.tone === 'bad') pushAlert('碰撞风险', 'bad');
  else if (st.risk && st.risk.tone === 'warn') pushAlert(st.risk.label, 'warn');
  if (mb.blocked) pushAlert('前方受阻', 'warn');
  if (mb.trav_blocked) pushAlert('可通行受阻', 'warn');
  if (Array.isArray(oddRaw.violations) && oddRaw.violations.length) pushAlert('ODD 越界 ' + oddRaw.violations.length, 'warn');
  if (oddRaw.in_odd === false) pushAlert('驶出 ODD', 'warn');
  if (oddRaw.loc_diverged) pushAlert('定位发散', 'warn');
  if (Array.isArray(st.l3Events)) {
    st.l3Events.slice(0, 3).forEach((ev) => pushAlert(ev.label || ev.type || '事件', ev.tone || 'warn'));
  }
  let alertLevel = 'ok';
  if (alertItems.some((it) => it.tone === 'bad')) alertLevel = 'bad';
  else if (alertItems.some((it) => it.tone === 'warn')) alertLevel = 'warn';
  if (!alertItems.length && st.hasData) alertItems.push({ label: '全部正常', tone: 'ok' });

  /* 转向/双闪指示：由机动 + MRM 派生（前端无原始灯控字段，只表达意图）。 */
  const blinker = (maneuver === 'left' || maneuver === 'right') ? maneuver
    : (torState === 'MRM' ? 'hazard' : '');

  return {
    hasData: !!st.hasData,
    driving: {
      speedKmh: Number.isFinite(st.speedKmh) ? Math.max(0, st.speedKmh) : null,
      speed: Number.isFinite(st.speed) ? st.speed : null,
      cruiseKmh,
      commandSpeed: Number.isFinite(st.commandSpeed) ? st.commandSpeed : null,
      behavior: st.behavior || { label: '未决策', tone: 'idle', key: '' },
      control: st.control || { label: '—', tone: 'idle' },
      driverMode: st.driverMode || '—',
      scenarioStageText: st.scenarioStageText || '--',
      maneuver,
      laneText,
      throttle: Number.isFinite(st.throttle) ? st.throttle : 0,
      brake: Number.isFinite(st.brake) ? st.brake : 0,
      steerDeg: Number.isFinite(st.steerDeg) ? st.steerDeg : 0,
      attitudeText: st.attitudeText || 'P +0.0° · R +0.0°',
      muText: st.muText || 'μ=1.00',
      isSlippery: !!st.isSlippery,
      trafficLight: tl ? { label: tl.label, tone: tl.tone } : null,
      risk: st.risk || { label: '—', tone: 'idle' },
      leadGapText,
      leadTtcText,
      blinker,
    },
    perception: {
      source: st.perceptionSource || '—',
      gtCount: _count(scene.entities),
      percCount: _count(scene.perception_entities),
      bevText: st.bevText || '—',
      bevCount: Number.isFinite(st.bevCount) ? st.bevCount : null,
      occupancy: occ,
    },
    planning: {
      intent: INTENT_BY_MANEUVER[maneuver] || '—',
      behaviorLabel: (st.behavior && st.behavior.label) || '未决策',
      stageText: st.scenarioStageText || '--',
      p2pText,
      laneCount: laneCountPlan,
      currentLane,
      targetLane,
      laneChangeDir,
      routeLaneGoal,
      distToLaneDropM,
      leftGapM,
      rightGapM,
      leftOk: !!mb.left_ok,
      rightOk: !!mb.right_ok,
      blocked: !!mb.blocked,
      travBlocked: !!mb.trav_blocked,
      upcomingOwnLanes: Number.isFinite(Number(mb.upcoming_own_lanes)) ? Number(mb.upcoming_own_lanes) : -1,
      traj,
    },
    diag: {
      locLabel: p2p.locLabel || '—',
      locTone: p2p.locTone || 'idle',
      torLabel: tor.label || 'L3 关闭',
      torTone: tor.tone || 'idle',
      torState: tor.state || '',
      l3Count,
      oddLabel: odd.label || '—',
      oddTone: odd.tone || 'idle',
      oddItems: Array.isArray(odd.items) ? odd.items.slice() : [],
    },
    alerts: { level: alertLevel, items: alertItems },
  };
}
