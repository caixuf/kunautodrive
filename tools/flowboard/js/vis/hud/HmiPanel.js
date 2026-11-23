/**
 * HmiPanel.js — HMI 专区的薄 DOM 渲染层（无业务逻辑）
 *
 * 职责只有一个：把 `buildHmiState(topo)` 归约好的状态写进 `#hmi-body` 的四个分区。
 * 逻辑/映射全部在 HmiModel.js（可单测），这里禁止再做任何数据推导。
 *
 * 模块顶层不触碰 document —— 只在 initHmiPanel/updateHmiPanel 被调用时才操作 DOM，
 * 保证 vis_module_load 冒烟能安全 import。
 *
 * 四分区：驾驶核心 / 感知·环境 / 规划意图 / 态势·诊断。
 */

function _set(id, text) {
  const el = document.getElementById(id);
  if (el && el.textContent !== text) el.textContent = text;
}

function _chip(id, text, tone) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = text;
  const cls = 'hmi-chip tone-' + (tone || 'idle');
  if (el.className !== cls) el.className = cls;
}

function _bar(id, frac) {
  const el = document.getElementById(id);
  if (!el) return;
  const w = (Math.max(0, Math.min(1, Number(frac) || 0)) * 100).toFixed(1) + '%';
  if (el.style.width !== w) el.style.width = w;
}

/**
 * 在 canvas 上画 mini BEV 占用栅格（车身系）。
 * 索引 = gx*grid_h + gy；gx 前向(0→x_range) 画成"向上"，gy 横向画成"左右"。
 * FREE 绿 / OCCUPIED 红 / UNKNOWN 透明。车在底部中央画一个小三角。
 * 纯示意（canvas 拉伸填满），不做精确比例；无 cells 时返回 false 让调用方保留占位文字。
 */
function _drawOccupancy(canvas, occ) {
  if (!canvas || !occ || !occ.cells) return false;
  const gw = occ.grid_w | 0;
  const gh = occ.grid_h | 0;
  if (gw <= 0 || gh <= 0) return false;
  const ctx = canvas.getContext && canvas.getContext('2d');
  if (!ctx) return false;
  const W = canvas.width, H = canvas.height;
  const cw = W / gh, ch = H / gw;

  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = 'rgba(13,17,23,0.85)'; /* UNKNOWN 底 */
  ctx.fillRect(0, 0, W, H);

  const cells = occ.cells;
  for (let gx = 0; gx < gw; gx++) {
    for (let gy = 0; gy < gh; gy++) {
      const v = cells[gx * gh + gy];
      if (v === 0) continue; /* UNKNOWN 留底 */
      ctx.fillStyle = (v === 2) ? 'rgba(248,81,73,0.85)'     /* OCCUPIED 红 */
                                : 'rgba(63,185,80,0.42)';    /* FREE 绿 */
      ctx.fillRect(gy * cw, (gw - 1 - gx) * ch, cw + 0.5, ch + 0.5);
    }
  }

  /* 自车标记：底部中央小三角（朝上 = 车头方向） */
  const cx = W / 2;
  ctx.fillStyle = 'rgba(88,166,255,0.95)';
  ctx.beginPath();
  ctx.moveTo(cx, H - 10);
  ctx.lineTo(cx - 5, H - 1);
  ctx.lineTo(cx + 5, H - 1);
  ctx.closePath();
  ctx.fill();
  return true;
}

/** 画目标车道梯：count 个方框，高亮当前/目标车道。容器签名不变则跳过重建。 */
function _renderLanes(container, count, current, target) {
  if (!container || typeof document.createElement !== 'function'
      || typeof container.appendChild !== 'function') return;
  const n = Math.max(0, Math.min(10, count | 0));
  const sig = n + '|' + current + '|' + target;
  if (container._sig === sig) return;
  container._sig = sig;
  container.innerHTML = '';
  for (let i = 0; i < n; i++) {
    const b = document.createElement('span');
    b.className = 'hmi-lane-box'
      + (i === current ? ' is-current' : '')
      + (i === target ? ' is-target' : '');
    container.appendChild(b);
  }
}

/** 画规划轨迹片段（ego 相对；前向上、右向右）。倒挡段(v<0)红色。 */
function _drawTrajectory(canvas, traj) {
  if (!canvas || !traj || !traj.pts || traj.pts.length < 2) return false;
  const ctx = canvas.getContext && canvas.getContext('2d');
  if (!ctx) return false;
  const W = canvas.width, H = canvas.height;
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = 'rgba(10,13,20,0.9)';
  ctx.fillRect(0, 0, W, H);

  let maxF = 1, maxR = 1;
  for (let i = 0; i < traj.pts.length; i++) {
    const p = traj.pts[i];
    if (p[0] > maxF) maxF = p[0];
    if (Math.abs(p[1]) > maxR) maxR = Math.abs(p[1]);
  }
  maxF = Math.max(maxF, 15);
  maxR = Math.max(maxR, 5);
  const s = Math.min((W / 2 - 6) / maxR, (H - 12) / maxF);
  const px = (p) => W / 2 + p[1] * s;
  const py = (p) => H - 6 - Math.max(0, p[0]) * s;

  ctx.lineWidth = 2;
  for (let i = 1; i < traj.pts.length; i++) {
    const a = traj.pts[i - 1], b = traj.pts[i];
    ctx.strokeStyle = (b[2] < -0.1) ? 'rgba(248,81,73,0.95)' : 'rgba(88,166,255,0.95)';
    ctx.beginPath();
    ctx.moveTo(px(a), py(a));
    ctx.lineTo(px(b), py(b));
    ctx.stroke();
  }
  ctx.fillStyle = 'rgba(63,185,80,0.95)';
  ctx.beginPath();
  ctx.arc(W / 2, H - 6, 3, 0, Math.PI * 2);
  ctx.fill();
  return true;
}

/** 顶部告警横幅：按最高严重度着色，逐条 chip。 */
function _renderAlerts(container, alerts) {
  if (!container) return;
  const lvl = (alerts && alerts.level) ? alerts.level : 'idle';
  container.className = 'hmi-alerts tone-' + lvl;
  const items = (alerts && alerts.items) || [];
  const sig = lvl + '|' + items.map((it) => (it.tone || '') + ':' + it.label).join('|');
  if (container._sig === sig) return;
  container._sig = sig;
  if (typeof document.createElement !== 'function' || typeof container.appendChild !== 'function') {
    container.textContent = sig;
    return;
  }
  container.innerHTML = '';
  if (!items.length) { container.textContent = '—'; return; }
  items.forEach((it) => {
    const s = document.createElement('span');
    s.className = 'hmi-alert-chip tone-' + (it.tone || 'idle');
    s.textContent = it.label;
    container.appendChild(s);
  });
}

/** 首次调用：把四分区骨架注入 body（幂等）。 */
export function initHmiPanel(body) {
  if (!body || body._hmiInit) return;
  body._hmiInit = true;
  body.innerHTML = `
    <div class="hmi-alerts tone-idle" id="hmi-alerts"></div>
    <div class="hmi-grid">
      <section class="hmi-sec">
        <h3 class="hmi-sec-title">驾驶核心</h3>
        <div class="hmi-speed"><b id="hmi-speed-kmh">--</b><span class="hmi-unit">km/h</span><em id="hmi-speed-ms">-- m/s</em></div>
        <div class="hmi-row"><span>目标巡航</span><b id="hmi-cruise">--</b></div>
        <div class="hmi-chips">
          <span class="hmi-chip tone-idle" id="hmi-beh">未决策</span>
          <span class="hmi-chip tone-idle" id="hmi-ctl">—</span>
          <span class="hmi-chip tone-idle" id="hmi-drv">—</span>
          <span class="hmi-chip tone-idle" id="hmi-tl" style="display:none">信号 --</span>
          <span class="hmi-chip tone-idle" id="hmi-blinker" style="display:none">◀</span>
        </div>
        <div class="hmi-bars">
          <span>油门</span><div class="hmi-bar"><i id="hmi-thr"></i></div>
          <span>制动</span><div class="hmi-bar hmi-brk"><i id="hmi-brk"></i></div>
          <span>方向</span><b id="hmi-steer">0.0°</b>
        </div>
        <div class="hmi-row"><span>姿态</span><b id="hmi-att">P +0.0° · R +0.0°</b></div>
        <div class="hmi-row"><span>车道</span><b id="hmi-lane">--</b></div>
        <div class="hmi-lead">
          <span class="hmi-chip tone-ok" id="hmi-risk">前方畅通</span>
          <span>前车距 <b id="hmi-gap">—</b></span>
          <span>TTC <b id="hmi-ttc">∞</b></span>
        </div>
      </section>

      <section class="hmi-sec">
        <h3 class="hmi-sec-title">感知 / 环境 <em class="hmi-tag">占用栅格待接入</em></h3>
        <div class="hmi-row"><span>感知源</span><b id="hmi-src">—</b></div>
        <div class="hmi-row"><span>感知目标</span><b id="hmi-perc-count">0</b></div>
        <div class="hmi-row"><span>真值目标</span><b id="hmi-gt-count">0</b></div>
        <div class="hmi-row"><span>BEV</span><b id="hmi-bev">—</b></div>
        <div class="hmi-occwrap">
          <canvas id="hmi-occ-canvas" width="120" height="150" class="hmi-occ-canvas"></canvas>
          <div class="hmi-occ-legend"><i class="lg-free"></i>空闲 <i class="lg-occ"></i>占用 <i class="lg-ego"></i>自车·前向上</div>
          <div class="hmi-occ" id="hmi-occ">占用栅格：等待数据…</div>
        </div>
      </section>

      <section class="hmi-sec">
        <h3 class="hmi-sec-title">规划意图</h3>
        <div class="hmi-intent" id="hmi-intent">—</div>
        <div class="hmi-lanes" id="hmi-lanes"></div>
        <div class="hmi-lane-cap" id="hmi-lane-cap">--</div>
        <canvas id="hmi-traj-canvas" width="200" height="88" class="hmi-traj-canvas"></canvas>
        <div class="hmi-row"><span>并道 / 缩线</span><b id="hmi-drop">—</b></div>
        <div class="hmi-row"><span>变道间隙</span><b id="hmi-gaps">—</b></div>
        <div class="hmi-row"><span>场景 · 阶段</span><b id="hmi-stage">--</b></div>
      </section>

      <section class="hmi-sec">
        <h3 class="hmi-sec-title">态势 / 诊断</h3>
        <div class="hmi-row"><span>定位</span><b id="hmi-loc">—</b></div>
        <div class="hmi-row"><span>TOR</span><b id="hmi-tor">L3 关闭</b></div>
        <div class="hmi-row"><span>L3 事件</span><b id="hmi-l3">0</b></div>
        <div class="hmi-row"><span>ODD</span><b id="hmi-odd">—</b></div>
      </section>
    </div>`;
}

/** 每帧（限流后）把状态写进 DOM。缺省/无数据时显式占位，不残留旧值。 */
export function updateHmiPanel(body, state) {
  if (!body) return;
  if (!body._hmiInit) initHmiPanel(body);
  if (!state) return;

  const d = state.driving || {};
  const p = state.perception || {};
  const pl = state.planning || {};
  const dg = state.diag || {};

  /* 顶部告警横幅（安全 HMI） */
  _renderAlerts(document.getElementById('hmi-alerts'), state.alerts);

  /* 驾驶核心 */
  _set('hmi-speed-kmh', Number.isFinite(d.speedKmh) ? d.speedKmh.toFixed(0) : '--');
  _set('hmi-speed-ms', Number.isFinite(d.speed) ? d.speed.toFixed(1) + ' m/s' : '-- m/s');
  _set('hmi-cruise', Number.isFinite(d.cruiseKmh) ? d.cruiseKmh.toFixed(0) + ' km/h' : '--');
  _chip('hmi-beh', d.behavior ? d.behavior.label : '未决策', d.behavior ? d.behavior.tone : 'idle');
  _chip('hmi-ctl', d.control ? d.control.label : '—', d.control ? d.control.tone : 'idle');
  _chip('hmi-drv', d.driverMode || '—', 'info');
  const blEl = document.getElementById('hmi-blinker');
  if (blEl) {
    if (d.blinker === 'left') { blEl.style.display = ''; _chip('hmi-blinker', '◀ 左', 'info'); }
    else if (d.blinker === 'right') { blEl.style.display = ''; _chip('hmi-blinker', '右 ▶', 'info'); }
    else if (d.blinker === 'hazard') { blEl.style.display = ''; _chip('hmi-blinker', '⚠ 双闪', 'warn'); }
    else { blEl.style.display = 'none'; }
  }
  const tlEl = document.getElementById('hmi-tl');
  if (tlEl) {
    if (d.trafficLight) {
      tlEl.style.display = '';
      _chip('hmi-tl', d.trafficLight.label, d.trafficLight.tone);
    } else {
      tlEl.style.display = 'none';
    }
  }
  _bar('hmi-thr', d.throttle);
  _bar('hmi-brk', d.brake);
  _set('hmi-steer', (d.steerDeg >= 0 ? '+' : '') + (Number(d.steerDeg) || 0).toFixed(1) + '°');
  _set('hmi-att', d.attitudeText || 'P +0.0° · R +0.0°');
  _set('hmi-lane', d.laneText || '--');
  _chip('hmi-risk', d.risk ? d.risk.label : '—', d.risk ? d.risk.tone : 'idle');
  _set('hmi-gap', d.leadGapText || '—');
  _set('hmi-ttc', d.leadTtcText || '∞');

  /* 感知 / 环境 */
  _set('hmi-src', p.source || '—');
  _set('hmi-perc-count', String(p.percCount || 0));
  _set('hmi-gt-count', String(p.gtCount || 0));
  _set('hmi-bev', p.bevText || '—');
  {
    const occ = p.occupancy;
    const canvas = document.getElementById('hmi-occ-canvas');
    const drew = occ ? _drawOccupancy(canvas, occ) : false;
    _set('hmi-occ', occ
      ? `占用 ${occ.occupied || 0} / 空闲 ${occ.free || 0} cell · ${occ.grid_w}×${occ.grid_h}`
      : '占用栅格：等待数据…');
    if (!drew && canvas && canvas.getContext) {
      const ctx = canvas.getContext('2d');
      if (ctx) ctx.clearRect(0, 0, canvas.width, canvas.height);
    }
  }

  /* 规划意图 */
  _set('hmi-intent', pl.intent || '—');
  _renderLanes(document.getElementById('hmi-lanes'), pl.laneCount, pl.currentLane, pl.targetLane);
  {
    let cap = '—';
    if (pl.laneCount > 0 && pl.currentLane >= 0) {
      cap = '当前 L' + (pl.currentLane + 1) + '/' + pl.laneCount;
      if (pl.targetLane >= 0 && pl.targetLane !== pl.currentLane) {
        cap += ' → 目标 L' + (pl.targetLane + 1) + (pl.laneChangeDir === 'left' ? ' ←' : ' →');
      }
      if (pl.routeLaneGoal >= 0 && pl.routeLaneGoal !== pl.targetLane) {
        cap += ' · 路线 L' + (pl.routeLaneGoal + 1);
      }
    }
    _set('hmi-lane-cap', cap);
  }
  _drawTrajectory(document.getElementById('hmi-traj-canvas'), pl.traj);
  _set('hmi-drop', pl.distToLaneDropM != null
    ? '前方 ' + Math.max(0, pl.distToLaneDropM).toFixed(0) + ' m 并道'
    : (pl.upcomingOwnLanes >= 0 ? '前方车道数 ' + pl.upcomingOwnLanes
        : ((pl.blocked || pl.travBlocked) ? '受阻' : '—')));
  {
    const lg = pl.leftGapM != null ? pl.leftGapM.toFixed(1) + 'm' : '—';
    const rg = pl.rightGapM != null ? pl.rightGapM.toFixed(1) + 'm' : '—';
    _set('hmi-gaps', '左 ' + (pl.leftOk ? '✓' : '✗') + lg + ' · 右 ' + (pl.rightOk ? '✓' : '✗') + rg);
  }
  _set('hmi-stage', pl.stageText || '--');

  /* 态势 / 诊断 */
  _set('hmi-loc', dg.locLabel || '—');
  _set('hmi-tor', dg.torLabel || 'L3 关闭');
  _set('hmi-l3', String(dg.l3Count || 0));
  _set('hmi-odd', dg.oddLabel || '—');

  /* 无数据遮罩 */
  const empty = body.querySelector('.hmi-empty');
  if (empty) empty.style.display = state.hasData ? 'none' : '';
}
