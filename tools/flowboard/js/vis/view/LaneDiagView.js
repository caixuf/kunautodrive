/**
 * LaneDiagView.js — 车道可观测性叠加层（纯诊断，不参与任何控制）
 *
 * 目的：把"车道几何到底以谁为准"摊在同一屏，让"是不是感知出问题"肉眼可判。
 * 四组数据严格区分来源、颜色/线型互不相同：
 *
 *   ① 规划参考线     store.planningDebug.ref_x/ref_y      青·实线
 *                    —— 规划正在跟踪的 Frenet 参考线，即目标车道中心
 *   ② 车道格网       roadNetwork.lane_data 的 lanes[].centerline（有则用）
 *                    否则按 planning_debug.lane_width × n_lanes 从道路中心线推算
 *                    灰·虚线 —— "系统认为的车道在哪"
 *   ③ 权威车道定位   store.laneMatch（esmini Frenet 解算的 x/y）  绿·法向刻度
 *   ④ 感知车道线     store.perceivedLanes.boundaries[].pts（沙箱合成，synthetic）
 *                    橙·虚线 —— "感知实际输出了什么"
 *
 * 判读：② 与 ① 重合而自车不在其上 ⇒ 规划横向没收敛；④ 为空 ⇒ 感知支路没数据；
 * ①② 本身错位 ⇒ 标线来源与规划参考不同源。结论由 HUD 输出（app.js），
 * 本层只负责把四组几何画出来。
 *
 * 契约：颜色只从 theme/tokens.js 出；坐标只走 math/Coord.js；不读 DOM/window。
 * 数据缺失时本层留空——"缺失"本身就是证据，由 HUD 显式呈现。
 */

import { worldToThree, tangentToNormal, offsetAlongNormal } from '../math/Coord.js';
import { LANE_DIAG } from '../theme/tokens.js';

const LINE_LIFT = 0.06;   // 抬离地面，避免与路面/标线 z-fight
const MAX_LINES  = 16;    // 单类最多画多少条（防止异常数据把场景刷爆）
const DASH_SIZE  = 1.4;
const DASH_GAP   = 1.0;

export function createLaneDiagView(scene) {
  const group = new THREE.Group();
  group.name = 'laneDiag';
  scene.add(group);

  let visible = false;      // 默认关闭：诊断叠加层不参与常规观感，由 app.js 打开
  let signature = '';
  const pool = [];
  group.visible = false;

  /** 释放位置 i 之后的线（几何 + 材质一起回收） */
  function _releaseFrom(i) {
    while (pool.length > i) {
      const line = pool.pop();
      group.remove(line);
      if (line.geometry) line.geometry.dispose();
      if (line.material) line.material.dispose();
    }
  }

  function _addLine(points3, color, dashed, opacity) {
    if (!Array.isArray(points3) || points3.length < 2) return false;
    const arr = new Float32Array(points3.length * 3);
    for (let i = 0; i < points3.length; i++) {
      arr[i * 3]     = points3[i][0];
      arr[i * 3 + 1] = points3[i][1];
      arr[i * 3 + 2] = points3[i][2];
    }
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute('position', new THREE.BufferAttribute(arr, 3));
    const material = dashed
      ? new THREE.LineDashedMaterial({
          color, dashSize: DASH_SIZE, gapSize: DASH_GAP,
          transparent: true, opacity, depthWrite: false,
        })
      : new THREE.LineBasicMaterial({ color, transparent: true, opacity, depthWrite: false });
    const line = new THREE.Line(geometry, material);
    if (dashed) line.computeLineDistances();
    line.frustumCulled = false;
    group.add(line);
    pool.push(line);
    return true;
  }

  /** ENU 折线 [[x,y],...] → THREE 世界点 [[x, lift, z],...]（唯一坐标入口） */
  function _enuToThree(nodes) {
    const out = [];
    if (!Array.isArray(nodes)) return out;
    for (let i = 0; i < nodes.length; i++) {
      const p = nodes[i];
      if (!Array.isArray(p) || p.length < 2) continue;
      const x = Number(p[0]);
      const y = Number(p[1]);
      if (!Number.isFinite(x) || !Number.isFinite(y)) continue;
      const t = worldToThree(x, y, 0);
      out.push([t[0], LINE_LIFT, t[2]]);
    }
    return out;
  }

  /** 把 THREE 折线沿法向平移 offset（m） */
  function _offsetPolyline(points3, offset) {
    if (points3.length < 2) return [];
    const out = [];
    for (let i = 0; i < points3.length; i++) {
      const a = points3[Math.max(0, i - 1)];
      const b = points3[Math.min(points3.length - 1, i + 1)];
      const normal = tangentToNormal(b[0] - a[0], b[2] - a[2]);
      const q = offsetAlongNormal(points3[i][0], points3[i][2], normal[0], normal[1], offset);
      out.push([q[0], LINE_LIFT, q[2]]);
    }
    return out;
  }

  /** ② 车道格网。返回画出的条数（0 = 无可用数据） */
  function _buildLattice(roadNetwork, planningDebug) {
    let made = 0;
    const laneData = roadNetwork && roadNetwork.lane_data;
    if (laneData) {
      const keys = Object.keys(laneData);
      for (let ki = 0; ki < keys.length && made < MAX_LINES; ki++) {
        const lanes = laneData[keys[ki]];
        if (!Array.isArray(lanes)) continue;
        for (let li = 0; li < lanes.length && made < MAX_LINES; li++) {
          const lane = lanes[li];
          if (_addLine(_enuToThree(lane && lane.centerline),
                       LANE_DIAG.lattice, true, 0.55)) made++;
        }
      }
    }
    if (made > 0) return made;

    /* 启发式兜底（demo/直道场景无 lane_data）：道路中心线 ± lane_width 推算。
     * 只画一次中心线平移，不复制标线样式——它标的是"车道中心"，不是标线。 */
    const edges = (roadNetwork && Array.isArray(roadNetwork.edges)) ? roadNetwork.edges : [];
    const edge0 = edges[0];
    const center3 = _enuToThree(edge0 && edge0.nodes);
    if (center3.length < 2) return 0;
    const nLanes = Math.max(1, Math.min(MAX_LINES,
      Number(planningDebug && planningDebug.n_lanes) || Number(edge0.lanes) || 2));
    const laneW = Number((planningDebug && planningDebug.lane_width)
      || Number(edge0.lane_width)) || 3.5;
    for (let i = 0; i < nLanes && made < MAX_LINES; i++) {
      const off = (i - (nLanes - 1) / 2) * laneW;
      if (_addLine(_offsetPolyline(center3, off), LANE_DIAG.lattice, true, 0.45)) made++;
    }
    return made;
  }

  function _rebuild(store) {
    _releaseFrom(0);
    const planningDebug = store.planningDebug || null;
    const laneMatch     = store.laneMatch || null;
    const perceived     = store.perceivedLanes || null;

    /* ① 规划参考线：ref_x/ref_y 是规划侧 Frenet 参考线的世界坐标序列 */
    if (planningDebug
        && Array.isArray(planningDebug.ref_x)
        && Array.isArray(planningDebug.ref_y)) {
      const n = Math.min(planningDebug.ref_x.length, planningDebug.ref_y.length);
      const nodes = [];
      for (let i = 0; i < n; i++) nodes.push([planningDebug.ref_x[i], planningDebug.ref_y[i]]);
      _addLine(_enuToThree(nodes), LANE_DIAG.plannerRef, false, 0.95);
    }

    /* ② 车道格网 */
    _buildLattice(store.roadNetwork || null, planningDebug);

    /* ③ 权威车道定位：在 esmini 解算出的自车位置画一段横跨本车道的法向刻度，
     * 刻度长度取一个车道宽 —— 一眼看出车压在车道内还是骑在线上。 */
    const lx = Number(laneMatch && laneMatch.x);
    const ly = Number(laneMatch && laneMatch.y);
    if (Number.isFinite(lx) && Number.isFinite(ly)) {
      const laneW = Number((planningDebug && planningDebug.lane_width)) || 3.5;
      const half = laneW / 2;
      _addLine(_enuToThree([[lx, ly - half], [lx, ly + half]]),
               LANE_DIAG.laneMatch, false, 0.95);
    }

    /* ④ 感知车道线：沙箱合成的边界折线（pts 已是世界 ENU） */
    const boundaries = perceived && Array.isArray(perceived.boundaries)
      ? perceived.boundaries : [];
    for (let i = 0; i < boundaries.length && i < MAX_LINES; i++) {
      const b = boundaries[i];
      if (!b || !Array.isArray(b.pts)) continue;
      _addLine(_enuToThree(b.pts), LANE_DIAG.detected, true, 0.85);
    }
  }

  /** 数据签名：变了才重建（诊断数据 ~2Hz，签名天然限流，不必每帧重建几何） */
  function _signature(store) {
    const pd = store.planningDebug || {};
    const lm = store.laneMatch || {};
    const pl = store.perceivedLanes || {};
    const ref0 = (Array.isArray(pd.ref_x) && pd.ref_x.length) ? Number(pd.ref_x[0]) : NaN;
    return [
      Number.isFinite(ref0) ? ref0.toFixed(1) : 'na',
      pd.target_lane_offset, pd.ego_d,
      lm.lane_id, lm.offset,
      pl.frame_id, (pl.boundaries || []).length,
      store.roadHash,
    ].join('|');
  }

  function update(store) {
    if (!visible) return;
    const sig = _signature(store);
    if (sig === signature) return;
    signature = sig;
    _rebuild(store);
  }

  function setVisible(on) {
    visible = !!on;
    group.visible = visible;
    if (visible) signature = '';   // 重新打开时强制重建一次，避免显示过期几何
  }

  function isVisible() { return visible; }

  function clear() {
    _releaseFrom(0);
    signature = '';
  }

  function dispose() {
    clear();
    if (group.parent) group.parent.remove(group);
  }

  return { update, clear, dispose, setVisible, isVisible };
}
