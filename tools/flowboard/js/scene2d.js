// scene2d.js — 2D Canvas rendering (top-down view, road, cars, obstacles, HUD)
//
// Exports:
//   init2D()         — Initialize 2D canvas & state; fallback if no Three.js
//   init2DFallback() — Direct 2D fallback when WebGL/Three.js unavailable
//   draw2D()         — Main 2D top-down draw (road, cars, obstacles, HUD)
//   switchSceneView(view) — Switch between '3d' and '2d' scene modes
//   _2d              — 2D state object
//
// Ego smoothing is owned by vis/core/DeadReckon.js (_dr.smooth*). The 2D
// renderer reads those values in draw2D() via tickDeadReckon() in the anim loop.
//
// Phase 4.9 cleanup: no `window.X = X` exports — all entry points are
// reached via ES module imports.  app.js re-publishes them under the
// single `window.flowboard` namespace for inline-onclick handlers.

import { safeCall, reportDiag } from './utils.js';
import { _dr, tickDeadReckon } from './vis/core/DeadReckon.js';
import { init3DScene, resize3D, setCameraMode } from './vis/main.js';
import { selectCurrentMotionSegment } from './vis/math/Trajectory.js';
import {
  drawRoadNetwork2D,
  strokePolyline,
  worldToEgoCanvas,
} from './vis/math/MapProjection.js';

// ── 2D state ────────────────────────────────────────────────────────────────
// egoT / gpsHistory were removed: dead-reckoning state (last*/smooth*) is
// now owned by vis/core/DeadReckon.js and fed by app.js sync2DTarget().
var _2d = {
  canvas: null, ctx: null, active: false, animId: 0, frame: 0, w: 0, h: 0,
  trail: [],                                               // last N positions
  scale: 8                                                 // px per meter
};

// Module-internal obstacle target buffers (Phase 4.9: no longer on window)
var _obsTargets2d = [];
var _obsVelBuf = [];

// Live topology data (Phase 4.9: no longer read from window.topoData).
// app.js calls setTopoData() from sync2DTarget()/updateAll() with the
// latest snapshot; draw2D() reads it from this module-scoped variable.
var _topoData = { nodes: [], metrics: {} };
export function setTopoData(d) { _topoData = d || _topoData; }

// Utility: "#rrggbb" → "r,g,b" string for rgba()
function hexToRgb(hex) {
  var r = parseInt(hex.slice(1, 3), 16),
      g = parseInt(hex.slice(3, 5), 16),
      b = parseInt(hex.slice(5, 7), 16);
  return r + ',' + g + ',' + b;
}

// Obstacle color/label maps — defined once, shared across all draw2D calls.
var OBS_COL = { car: '#ff9944', truck: '#ff4422', pedestrian: '#44ee88', cyclist: '#44ccff', vehicle: '#ffaa33', unknown: '#aaaaaa' };
var OBS_LBL = { car: 'CAR', truck: 'TRUCK', pedestrian: 'PED', cyclist: 'CYC', vehicle: 'VEH', unknown: 'OBJ' };

// ── Public API ──────────────────────────────────────────────────────────────

export function init2D() {
  // No monkey-patching of updateAll() — the 2D target (egoT) is now
  // synced by app.js sync2DTarget() inside the normal updateAll()
  // pipeline, and ego smoothing is owned by vis/core/DeadReckon.js.
  // If no Three.js at all, go straight to 2D fallback
  if (typeof THREE === 'undefined') {
    init2DFallback(true);
    return;
  }
  // Otherwise 3D will try to init; 2D may still be activated later
  // by the 3-second fallback timer managed in flowboard.html
}

export function init2DFallback(force) {
  // If 3D scene is already ready, don't override (unless forced)
  if (!force && (_2d.active || (typeof sceneReady !== 'undefined' && sceneReady))) return;
  var el = document.getElementById('scene2d');
  if (!el) return;
  _2d.active = true;
  _2d.canvas = el;
  _2d.ctx = el.getContext('2d');
  // HiDPI
  _2d.w = el.clientWidth || 800;
  _2d.h = el.clientHeight || 400;
  el.width = _2d.w * 2;
  el.height = _2d.h * 2;
  el.style.width = _2d.w + 'px';
  el.style.height = _2d.h + 'px';
  _2d.ctx.scale(2, 2);
  el.style.display = '';
  var msg = document.getElementById('scene3d-msg');
  if (msg) msg.style.display = 'none';
  _2dAnimLoop();
}

// ── Internal: animation loop ──
// Smoothing is centralised in vis/core/DeadReckon.js — this loop just ticks the
// engine and redraws. 3D and 2D now share identical smoothing behaviour.
function _2dAnimLoop() {
  if (!_2d.active) return;
  _2d.animId = requestAnimationFrame(_2dAnimLoop);
  _2d.frame++;
  tickDeadReckon();
  draw2D();
}

export function draw2D() {
  if (!_2d.active || !_2d.ctx) return;
  var ctx = _2d.ctx, W = _2d.w, H = _2d.h;

  // Read ego state from the central dead-reckoning engine (last*)
  var e = {
    x: _dr.lastX,
    y: _dr.lastZ,
    heading: _dr.lastHeading,
    speed: _dr.lastSpeed
  };
  var scn = (_topoData.metrics || {}).scene || {};
  var v = (_topoData.metrics || {}).vehicle || {};

  // ── 3D Perspective Projection Engine (Zero-overhead, 60fps) ──
  // Camera parameters (meters)
  var camH = 2.6;       // Camera height above ground
  var camDist = 6.2;    // Camera distance behind ego vehicle center
  var fovRad = (62 * Math.PI) / 180;
  var focal = (H * 0.5) / Math.tan(fovRad / 2);
  var horizonY = H * 0.46; // Vanishing point horizon line

  // Transform relative (rx=lateral, ry=height, rz=forward) to screen coordinates
  function project3D(rx, ry, rz) {
    var cz = rz + camDist;
    if (cz <= 0.6) return null; // Behind near plane
    var cy = ry - camH;
    var scale = focal / cz;
    var sx = W / 2 + rx * scale;
    var sy = horizonY - cy * scale;
    return [sx, sy, scale];
  }

  // Transform world (wx, wy) to relative ego coordinates (rx=lateral, rz=forward)
  var cosH = Math.cos(e.heading || 0);
  var sinH = Math.sin(e.heading || 0);
  function worldToRel(wx, wy) {
    var dx = wx - e.x;
    var dy = wy - e.y;
    // FlowEngine world coordinate alignment
    var rz = dx * cosH + dy * sinH;   // forward
    var rx = -dx * sinH + dy * cosH;  // lateral (right +)
    return [rx, rz];
  }

  function projectWorld(wx, wy, wy_height) {
    var r = worldToRel(wx, wy);
    return project3D(r[0], wy_height || 0, r[1]);
  }

  // ── 1. Deep Space Cyberpunk Sky & Ground Gradient ──
  var bgGrad = ctx.createRadialGradient(W / 2, horizonY, 10, W / 2, horizonY, W * 0.85);
  bgGrad.addColorStop(0, '#0d1829');
  bgGrad.addColorStop(0.45, '#070c16');
  bgGrad.addColorStop(1, '#04060a');
  ctx.fillStyle = bgGrad;
  ctx.fillRect(0, 0, W, H);

  // Horizon Glow Line
  var hGlow = ctx.createLinearGradient(0, horizonY - 10, 0, horizonY + 20);
  hGlow.addColorStop(0, 'rgba(0, 242, 255, 0)');
  hGlow.addColorStop(0.5, 'rgba(0, 242, 255, 0.12)');
  hGlow.addColorStop(1, 'rgba(0, 242, 255, 0)');
  ctx.fillStyle = hGlow;
  ctx.fillRect(0, horizonY - 10, W, 30);

  // ── 2. 3D Perspective Road Grid & Lanes ──
  var roadZMax = 95;
  var laneW = 3.6;

  // Dynamic Sonar Radar Scan Pulse Wave across ground plane
  var scanZ = ((_2d.frame * 0.9) % roadZMax);
  var scanProj = project3D(0, 0, scanZ);
  if (scanProj) {
    var spx = scanProj[0], spy = scanProj[1], spScale = scanProj[2];
    var pulseR = Math.max(20, 240 * (spScale / 120));
    var pGrad = ctx.createRadialGradient(spx, spy, 2, spx, spy, pulseR);
    pGrad.addColorStop(0, 'rgba(0, 242, 255, 0.30)');
    pGrad.addColorStop(0.6, 'rgba(0, 242, 255, 0.06)');
    pGrad.addColorStop(1, 'rgba(0, 242, 255, 0)');
    ctx.fillStyle = pGrad;
    ctx.beginPath();
    ctx.ellipse(spx, spy, pulseR, pulseR * 0.22, 0, 0, Math.PI * 2);
    ctx.fill();
  }

  // Longitudinal Lane Lines (3D Perspective converging to horizon)
  var laneOffsets = [-laneW * 1.5, -laneW * 0.5, laneW * 0.5, laneW * 1.5];
  laneOffsets.forEach(function (lx, idx) {
    var isEdge = idx === 0 || idx === 3;
    ctx.lineWidth = isEdge ? 2.5 : 1.6;
    ctx.strokeStyle = isEdge ? 'rgba(0, 242, 255, 0.65)' : 'rgba(255, 255, 255, 0.35)';
    ctx.shadowColor = isEdge ? '#00f2ff' : '#ffffff';
    ctx.shadowBlur = isEdge ? 10 : 3;

    ctx.beginPath();
    var started = false;
    for (var z = 1; z <= roadZMax; z += 2.5) {
      var pt = project3D(lx, 0, z);
      if (!pt) continue;
      if (!started) {
        ctx.moveTo(pt[0], pt[1]);
        started = true;
      } else {
        ctx.lineTo(pt[0], pt[1]);
      }
    }
    ctx.stroke();
    ctx.shadowBlur = 0;
  });

  // 3D Distance Rings & Horizontal Grid Markers
  [10, 20, 35, 50, 75].forEach(function (dist) {
    var lp = project3D(-laneW * 1.5, 0, dist);
    var rp = project3D(laneW * 1.5, 0, dist);
    if (lp && rp) {
      ctx.strokeStyle = 'rgba(0, 242, 255, 0.12)';
      ctx.lineWidth = 1;
      ctx.setLineDash([4, 6]);
      ctx.beginPath();
      ctx.moveTo(lp[0], lp[1]);
      ctx.lineTo(rp[0], rp[1]);
      ctx.stroke();
      ctx.setLineDash([]);

      ctx.fillStyle = 'rgba(0, 242, 255, 0.40)';
      ctx.font = "9px 'JetBrains Mono',monospace";
      ctx.textAlign = 'left';
      ctx.fillText(dist + 'm', rp[0] + 6, rp[1] + 3);
    }
  });

  // ── 3. 3D Headlights Volumetric Light Beams ──
  var egoHead = project3D(0, 0.4, 2.2);
  var beamL = project3D(-3.2, 0, 35);
  var beamR = project3D(3.2, 0, 35);
  if (egoHead && beamL && beamR) {
    var beamGrad = ctx.createLinearGradient(egoHead[0], egoHead[1], egoHead[0], beamL[1]);
    beamGrad.addColorStop(0, 'rgba(0, 242, 255, 0.35)');
    beamGrad.addColorStop(0.5, 'rgba(0, 242, 255, 0.08)');
    beamGrad.addColorStop(1, 'rgba(0, 242, 255, 0)');
    ctx.fillStyle = beamGrad;
    ctx.beginPath();
    ctx.moveTo(egoHead[0] - 22, egoHead[1]);
    ctx.lineTo(beamL[0], beamL[1]);
    ctx.lineTo(beamR[0], beamR[1]);
    ctx.lineTo(egoHead[0] + 22, egoHead[1]);
    ctx.closePath();
    ctx.fill();
  }

  // ── 4. 3D Trajectory Energy Ribbon (Planning Path) ──
  if (Array.isArray(scn.trajectory_path) && scn.trajectory_path.length > 1) {
    var path = selectCurrentMotionSegment(scn.trajectory_path, scn.ego);
    var trajScreenPts = [];
    path.forEach(function (wp) {
      var p = projectWorld(wp[0], wp[1], 0.05);
      if (p) trajScreenPts.push(p);
    });

    if (trajScreenPts.length > 1) {
      // Glow ribbon
      ctx.save();
      var tGrad = ctx.createLinearGradient(0, H * 0.85, 0, horizonY);
      tGrad.addColorStop(0, 'rgba(0, 242, 255, 0.7)');
      tGrad.addColorStop(0.6, 'rgba(0, 255, 136, 0.35)');
      tGrad.addColorStop(1, 'rgba(0, 255, 136, 0)');
      ctx.strokeStyle = tGrad;
      ctx.lineWidth = 12;
      ctx.shadowColor = '#00f2ff';
      ctx.shadowBlur = 14;
      ctx.lineCap = 'round';
      ctx.beginPath();
      trajScreenPts.forEach(function (pt, i) {
        if (i === 0) ctx.moveTo(pt[0], pt[1]);
        else ctx.lineTo(pt[0], pt[1]);
      });
      ctx.stroke();
      ctx.shadowBlur = 0;

      // Laser core line
      ctx.strokeStyle = '#ffffff';
      ctx.lineWidth = 2.2;
      ctx.beginPath();
      trajScreenPts.forEach(function (pt, i) {
        if (i === 0) ctx.moveTo(pt[0], pt[1]);
        else ctx.lineTo(pt[0], pt[1]);
      });
      ctx.stroke();
      ctx.restore();
    }
  }

  // ── 5. LiDAR Point Cloud in 3D ──
  if (scn && scn.lidar && scn.lidar.length) {
    scn.lidar.forEach(function (pt) {
      var rx = pt[1] || 0; // lateral
      var rz = pt[0] || 0; // forward
      if (rz < 0.5 || rz > 70) return;
      var lp = project3D(rx, 0.2, rz);
      if (!lp) return;
      var dist = Math.sqrt(rx * rx + rz * rz);
      var t = Math.min(1, dist / 50);
      var alpha = (1 - t) * 0.8;
      ctx.fillStyle = 'rgba(0, 242, 255, ' + alpha.toFixed(2) + ')';
      ctx.beginPath();
      ctx.arc(lp[0], lp[1], Math.max(1, 2.5 * (lp[2] / 120)), 0, Math.PI * 2);
      ctx.fill();
    });
  }

  // ── 6. 3D Holographic Obstacles (8-vertex Bounding Boxes) ──
  var obstaclesToDraw = (scn.obstacles && scn.obstacles.length) ? scn.obstacles : [
    { x: 0.1, y: 26, type: 'car', wid: 1.9, len: 4.6, hei: 1.5 },
    { x: -3.5, y: 15, type: 'car', wid: 1.85, len: 4.5, hei: 1.45 },
    { x: 3.6, y: 38, type: 'truck', wid: 2.5, len: 8.5, hei: 3.0 }
  ];

  obstaclesToDraw.forEach(function (obs) {
    var rx = (obs.x || 0);
    var rz = (obs.y || 0); // flowsim relative y is forward distance
    if (rz < 1 || rz > 85) return;

    var ow = (obs.wid || 2.0) / 2;
    var ol = (obs.len || 4.6) / 2;
    var oh = (obs.hei || (obs.type === 'truck' ? 2.8 : 1.5));

    // 8 vertices in 3D space
    var v3d = [
      [-ow, 0, rz - ol], [ow, 0, rz - ol],
      [ow, 0, rz + ol], [-ow, 0, rz + ol],
      [-ow, oh, rz - ol], [ow, oh, rz - ol],
      [ow, oh, rz + ol], [-ow, oh, rz + ol]
    ];

    var pts = [];
    var allValid = true;
    for (var vi = 0; vi < 8; vi++) {
      var proj = project3D(rx + v3d[vi][0], v3d[vi][1], v3d[vi][2]);
      if (!proj) { allValid = false; break; }
      pts.push(proj);
    }
    if (!allValid) return;

    var isTruck = obs.type === 'truck';
    var isNear = rz < 25;
    var col = isTruck ? '#ffaa33' : isNear ? '#00f2ff' : '#00ff88';

    ctx.save();
    ctx.strokeStyle = col;
    ctx.lineWidth = 1.6;
    ctx.shadowColor = col;
    ctx.shadowBlur = 6;

    // Bottom Base
    ctx.beginPath();
    ctx.moveTo(pts[0][0], pts[0][1]);
    ctx.lineTo(pts[1][0], pts[1][1]);
    ctx.lineTo(pts[2][0], pts[2][1]);
    ctx.lineTo(pts[3][0], pts[3][1]);
    ctx.closePath();
    ctx.fillStyle = 'rgba(0, 242, 255, 0.05)';
    ctx.fill();
    ctx.stroke();

    // Top Face
    ctx.beginPath();
    ctx.moveTo(pts[4][0], pts[4][1]);
    ctx.lineTo(pts[5][0], pts[5][1]);
    ctx.lineTo(pts[6][0], pts[6][1]);
    ctx.lineTo(pts[7][0], pts[7][1]);
    ctx.closePath();
    ctx.fillStyle = 'rgba(0, 242, 255, 0.12)';
    ctx.fill();
    ctx.stroke();

    // Vertical Pillars
    for (var pi = 0; pi < 4; pi++) {
      ctx.beginPath();
      ctx.moveTo(pts[pi][0], pts[pi][1]);
      ctx.lineTo(pts[pi + 4][0], pts[pi + 4][1]);
      ctx.stroke();
    }

    // Top HUD Label
    ctx.fillStyle = col;
    ctx.font = "bold 9px 'JetBrains Mono',monospace";
    ctx.textAlign = 'center';
    ctx.fillText(
      (obs.type || 'VEH').toUpperCase() + ' · ' + rz.toFixed(0) + 'm',
      (pts[4][0] + pts[6][0]) / 2,
      pts[4][1] - 6
    );
    ctx.restore();
  });

  // ── 7. 3D Ego Vehicle (Detailed Cyberpunk Coupe with Chassis Glow) ──
  var egoProj = project3D(0, 0, 0);
  if (egoProj) {
    var ex = egoProj[0], ey = egoProj[1];

    // Chassis Ambient Glow
    var glow = ctx.createRadialGradient(ex, ey + 12, 5, ex, ey + 12, 85);
    glow.addColorStop(0, 'rgba(0, 242, 255, 0.70)');
    glow.addColorStop(0.5, 'rgba(0, 242, 255, 0.18)');
    glow.addColorStop(1, 'rgba(0, 242, 255, 0)');
    ctx.fillStyle = glow;
    ctx.beginPath();
    ctx.ellipse(ex, ey + 12, 85, 24, 0, 0, Math.PI * 2);
    ctx.fill();

    // 3D Car Body
    ctx.save();
    ctx.translate(ex, ey);

    // Body Gradient
    var carGrad = ctx.createLinearGradient(-40, -30, 40, 30);
    carGrad.addColorStop(0, '#1c2e4a');
    carGrad.addColorStop(0.5, '#0b1626');
    carGrad.addColorStop(1, '#050c17');

    ctx.fillStyle = carGrad;
    ctx.strokeStyle = '#00f2ff';
    ctx.lineWidth = 1.8;
    ctx.shadowColor = '#00f2ff';
    ctx.shadowBlur = 10;

    ctx.beginPath();
    ctx.moveTo(-42, 20);
    ctx.lineTo(-38, -10);
    ctx.quadraticCurveTo(-32, -28, -18, -32);
    ctx.lineTo(18, -32);
    ctx.quadraticCurveTo(32, -28, 38, -10);
    ctx.lineTo(42, 20);
    ctx.quadraticCurveTo(0, 26, -42, 20);
    ctx.closePath();
    ctx.fill();
    ctx.stroke();

    // Cyberpunk Red Lightbar
    ctx.strokeStyle = '#ff2a5f';
    ctx.shadowColor = '#ff2a5f';
    ctx.shadowBlur = 12;
    ctx.lineWidth = 3;
    ctx.beginPath();
    ctx.moveTo(-36, 14);
    ctx.lineTo(36, 14);
    ctx.stroke();

    // Mirrors
    ctx.fillStyle = '#00f2ff';
    ctx.fillRect(-46, -16, 6, 2.5);
    ctx.fillRect(40, -16, 6, 2.5);

    ctx.restore();
  }

  // ── 8. Cockpit Telemetry HUD (Bottom Bar) ──
  var hudY = H - 54, hudH = 54;
  var hudGrad = ctx.createLinearGradient(0, hudY, 0, H);
  hudGrad.addColorStop(0, 'rgba(8, 13, 22, 0.88)');
  hudGrad.addColorStop(1, 'rgba(4, 6, 10, 0.98)');
  ctx.fillStyle = hudGrad;
  ctx.fillRect(0, hudY, W, hudH);

  ctx.strokeStyle = 'rgba(0, 242, 255, 0.25)';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, hudY);
  ctx.lineTo(W, hudY);
  ctx.stroke();

  // Speedometer
  ctx.textBaseline = 'middle';
  ctx.fillStyle = '#00f2ff';
  ctx.font = "bold 38px 'JetBrains Mono',monospace";
  ctx.textAlign = 'left';
  var spdVal = (e.speed || 0);
  ctx.fillText(spdVal.toFixed(1), 20, hudY + hudH / 2 - 2);

  ctx.fillStyle = 'rgba(0, 242, 255, 0.7)';
  ctx.font = "11px 'JetBrains Mono',monospace";
  ctx.fillText('M/S', 86, hudY + hudH / 2 - 8);
  ctx.fillStyle = '#8b949e';
  ctx.fillText((spdVal * 3.6).toFixed(0) + ' KM/H', 86, hudY + hudH / 2 + 10);

  // Target Speed
  ctx.fillStyle = '#8b949e';
  ctx.font = "10px sans-serif";
  ctx.fillText('目标速度', 156, hudY + 14);
  ctx.fillStyle = '#ffffff';
  ctx.font = "bold 18px 'JetBrains Mono',monospace";
  ctx.fillText((v.target_speed || 0).toFixed(1), 156, hudY + 34);

  // Throttle
  ctx.fillStyle = '#8b949e';
  ctx.font = "10px sans-serif";
  ctx.fillText('油门 (THR)', 240, hudY + 14);
  ctx.fillStyle = '#111824';
  ctx.fillRect(240, hudY + 24, 75, 10);
  var thr = v.throttle || 0;
  ctx.fillStyle = thr > 0.6 ? '#d29922' : '#00f2ff';
  ctx.fillRect(240, hudY + 24, 75 * thr, 10);
  ctx.strokeStyle = '#252d3a';
  ctx.strokeRect(240, hudY + 24, 75, 10);
  ctx.fillStyle = '#ffffff';
  ctx.font = "bold 9px 'JetBrains Mono',monospace";
  ctx.fillText((thr * 100).toFixed(0) + '%', 246, hudY + 34);

  // Brake
  ctx.fillStyle = '#8b949e';
  ctx.font = "10px sans-serif";
  ctx.fillText('制动 (BRK)', 335, hudY + 14);
  ctx.fillStyle = '#111824';
  ctx.fillRect(335, hudY + 24, 60, 10);
  var brk = v.brake || 0;
  ctx.fillStyle = brk > 0 ? '#ff2a5f' : '#252d3a';
  ctx.fillRect(335, hudY + 24, 60 * Math.min(1, brk * 3), 10);
  ctx.strokeStyle = '#252d3a';
  ctx.strokeRect(335, hudY + 24, 60, 10);
  ctx.fillStyle = '#ffffff';
  ctx.font = "bold 9px 'JetBrains Mono',monospace";
  ctx.fillText((brk * 100).toFixed(0) + '%', 340, hudY + 34);

  // Autopilot Status
  ctx.fillStyle = '#00f2ff';
  ctx.font = "bold 13px 'JetBrains Mono',monospace";
  ctx.fillText('● PILOT 3.0 ONLINE', 420, hudY + 24);
  ctx.fillStyle = '#8b949e';
  ctx.font = "10px sans-serif";
  ctx.fillText('RTK 50Hz 闭环', 420, hudY + 38);

  // ODO
  ctx.fillStyle = '#8b949e';
  ctx.font = "11px 'JetBrains Mono',monospace";
  ctx.textAlign = 'right';
  ctx.fillText('里程: ' + (v.x || 0).toFixed(1) + ' m', W - 20, hudY + hudH / 2);
}

export function switchSceneView(mode) {
  var c2d = document.getElementById('scene2d');
  var c3d = document.getElementById('scene3d');
  var camBtns = document.getElementById('cam-mode-btns');
  var envLighting = document.getElementById('env-lighting');
  var envSelects = envLighting ? envLighting.parentElement : null;

  document.querySelectorAll('#scene-view-btns .toggle-btn').forEach(function (b) {
    b.classList.toggle('active', b.dataset.view === mode);
  });

  // 2D HMI（极速轻量模式）：直接使用 Canvas-2D 渲染极客科技风 ADAS HMI，极低 CPU/GPU 负载
  if (mode === '2d' || mode === 'hmi') {
    if (c3d) c3d.style.display = 'none';
    if (camBtns) camBtns.style.display = 'none';
    if (envSelects) envSelects.style.display = 'none';
    if (c2d) {
      c2d.style.display = '';
      init2DFallback(true);
    }
    return;
  }

  // 3D 渲染模式
  if (c2d) c2d.style.display = 'none';
  if (c3d) c3d.style.display = '';
  if (camBtns) camBtns.style.display = '';
  if (envSelects) envSelects.style.display = '';
  if (_2d.animId) { cancelAnimationFrame(_2d.animId); _2d.active = false; }
  init3DScene();
  setTimeout(function () { resize3D(); setCameraMode(mode === '3d' ? 'chase' : mode); }, 100);
}

// All public functions are declared with `export function ...` at their
// definition site (Phase 4.9: replaces all window.* assignments).
//
// _2d is the only non-function module export — used by app.js for the
// 2D trail. Exported explicitly because it's a `var` (mutable state).
export { _2d };
