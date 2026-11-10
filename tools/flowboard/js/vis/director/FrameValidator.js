import { LANE_WIDTH } from '../core/Constants.js';

/**
 * FrameValidator.js — SceneDirector frame schema 校验纯函数
 *
 * Step 5 重构：从 SceneDirector.js 抽出，零 THREE 依赖，便于单元测试。
 * 校验逻辑详见各分支注释，与 Step 1 行为一致：
 *   1. topoData 必须是 object（非 object 时整帧丢弃）
 *   2. frame（解包后）必须是 object
 *   3. road_network 必须是 object，edges 必须是 array
 *   4. （空 edges 允许：实际由 update() 的 isViaduct 兜底处理，不算校验失败）
 *   5. ego 必须是 object
 *   6. ego.x 必须是 finite number；heading / lights 若发则必须是 number
 *   7. entities 必须是 array
 *   8. 每个 entity 必须有 type 字段（仅检查非 ego 实体，与 build 过滤一致）
 *
 * C-5 schema 校验：在原有 range 检查基础上加 edge 必填字段 + nodes arity：
 *   9. 每个 edge 必须有 id / type / lanes / lane_width / length(或 length_m) / nodes
 *  10. 每个 node 必须是 [x, y, z] 三元组（arity=3，缺 z 会让路网高度插值退化）
 */

// C-5: edge 必填字段（length 单独处理，接受 length_m 别名）
const EDGE_REQUIRED_FIELDS = ['id', 'name', 'type', 'lanes', 'lane_width', 'nodes', 'oneway'];
// C-5: 每个 node 应为 [x, y, z] 三元组
const NODE_ARITY = 3;

function _typeOf(v) {
  if (v === null) return 'null';
  if (Array.isArray(v)) return 'array';
  return typeof v;
}

/**
 * @param {*} topoData  SceneDirector.update() 收到的原始数据
 * @returns {{
 *   ok: boolean,
 *   frame?: object,
 *   rn?: object,
 *   skipRoad?: boolean,
 *   skipEgo?: boolean,
 *   skipEntities?: boolean,
 *   warnings: Array<{key: string, msg: string}>,
 * }}
 */
export function validateFrame(topoData) {
  const warnings = [];
  if (!topoData) return { ok: false, warnings };
  if (typeof topoData !== 'object') {
    warnings.push({
      key: 'topoData.type',
      msg: 'update() 收到非对象 topoData: ' + _typeOf(topoData) + '，跳过本帧',
    });
    return { ok: false, warnings };
  }

  let frame = topoData;
  if (topoData.metrics && topoData.metrics.scene) frame = topoData.metrics.scene;
  else if (topoData.scene) frame = topoData.scene;

  if (typeof frame !== 'object' || frame === null) {
    warnings.push({
      key: 'frame.type',
      msg: 'frame 解包后非对象: ' + _typeOf(frame) + '，跳过本帧',
    });
    return { ok: false, warnings };
  }

  const rn = frame.road_network || frame.roads;
  let skipRoad = false;
  if (rn) {
    if (typeof rn !== 'object' || rn === null) {
      warnings.push({
        key: 'road_network.type',
        msg: 'road_network 非 object: ' + _typeOf(rn) + '，跳过道路重建',
      });
      skipRoad = true;
    } else if (rn.edges !== undefined && !Array.isArray(rn.edges)) {
      warnings.push({
        key: 'road_network.edges',
        msg: 'road_network.edges 非数组: ' + _typeOf(rn.edges) + '，跳过道路重建',
      });
      skipRoad = true;
    } else if (rn.edges && rn.edges.length > 0) {
      for (let i = 0; i < rn.edges.length; i++) {
        const edge = rn.edges[i];
        if (!edge) continue;

        // C-5: 必填字段检查（id / type / lanes / lane_width / nodes）
        for (const field of EDGE_REQUIRED_FIELDS) {
          if (edge[field] === undefined || edge[field] === null) {
            warnings.push({
              key: 'road_network.edges[' + i + '].' + field,
              msg: 'edge[' + i + '].' + field + ' 缺失（必填字段），可能影响道路渲染',
            });
          }
        }
        // C-5: length 接受 length_m 别名，两者皆缺才告警
        if ((edge.length === undefined || edge.length === null) &&
            (edge.length_m === undefined || edge.length_m === null)) {
          warnings.push({
            key: 'road_network.edges[' + i + '].length',
            msg: 'edge[' + i + '].length 缺失（必填字段，接受 length 或 length_m），回退默认长度',
          });
        }

        if (edge.lane_width != null && (edge.lane_width <= 0 || edge.lane_width > 10)) {
          warnings.push({
            key: 'road_network.edges[' + i + '].lane_width',
            msg: 'edge[' + i + '].lane_width=' + edge.lane_width + ' 超出合理范围(0,10]，回退默认 ' + LANE_WIDTH + 'm',
          });
        }
        const len = edge.length || edge.length_m;
        if (len != null && len <= 0) {
          warnings.push({
            key: 'road_network.edges[' + i + '].length',
            msg: 'edge[' + i + '].length=' + len + ' <= 0，道路长度无效',
          });
        }
        if (!edge.nodes || !Array.isArray(edge.nodes) || edge.nodes.length < 2) {
          warnings.push({
            key: 'road_network.edges[' + i + '].nodes',
            msg: 'edge[' + i + '].nodes 无效（需至少 2 个节点），道路可能无法渲染',
          });
        } else {
          // C-5: nodes arity 检查 —— 每个 node 必须是 [x, y, z] 三元组
          // 缺 z（2 元组）会让 _roadHeightAt 的 a[2] || 0 静默退化为 0，
          // 高架场景下路面高度插值失真，故显式告警。
          for (let j = 0; j < edge.nodes.length; j++) {
            const node = edge.nodes[j];
            if (!Array.isArray(node) || node.length !== NODE_ARITY) {
              warnings.push({
                key: 'road_network.edges[' + i + '].nodes[' + j + '].arity',
                msg: 'edge[' + i + '].nodes[' + j + '] 元素数=' +
                     (Array.isArray(node) ? node.length : _typeOf(node)) +
                     '（应为 ' + NODE_ARITY + ': [x, y, z]）',
              });
            }
          }
        }
      }
    }
  }

  let skipEgo = false;
  if (frame.ego !== undefined) {
    const e = frame.ego;
    if (typeof e !== 'object' || e === null) {
      warnings.push({
        key: 'ego.type',
        msg: 'frame.ego 非 object: ' + _typeOf(e) + '，跳过 ego 更新',
      });
      skipEgo = true;
    } else {
      if (typeof e.x !== 'number' || !isFinite(e.x)) {
        warnings.push({
          key: 'ego.x',
          msg: 'ego.x 非 finite number: ' + _typeOf(e.x) + '，回退 0（ego 会卡在原点，检查 scene_pub ego JSON）',
        });
      }
      if (e.heading !== undefined && typeof e.heading !== 'number') {
        warnings.push({
          key: 'ego.heading',
          msg: 'ego.heading 非 number: ' + _typeOf(e.heading),
        });
      }
      if (e.lights !== undefined && typeof e.lights !== 'number') {
        warnings.push({
          key: 'ego.lights',
          msg: 'ego.lights 非 number: ' + _typeOf(e.lights) + '，回退 0（车灯不亮）',
        });
      }
      if (e.pitch !== undefined && (typeof e.pitch !== 'number' || !isFinite(e.pitch))) {
        warnings.push({
          key: 'ego.pitch',
          msg: 'ego.pitch 非 finite number: ' + _typeOf(e.pitch),
        });
      }
      if (e.roll !== undefined && (typeof e.roll !== 'number' || !isFinite(e.roll))) {
        warnings.push({
          key: 'ego.roll',
          msg: 'ego.roll 非 finite number: ' + _typeOf(e.roll),
        });
      }
      if (e.mu_scale !== undefined && (typeof e.mu_scale !== 'number' || !isFinite(e.mu_scale))) {
        warnings.push({
          key: 'ego.mu_scale',
          msg: 'ego.mu_scale 非 finite number: ' + _typeOf(e.mu_scale),
        });
      }
      if (e.road_grade !== undefined && (typeof e.road_grade !== 'number' || !isFinite(e.road_grade))) {
        warnings.push({
          key: 'ego.road_grade',
          msg: 'ego.road_grade 非 finite number: ' + _typeOf(e.road_grade),
        });
      }
    }
  }

  // M5: predictions / friction_zones / parking_goal schema 校验
  if (frame.predictions !== undefined && !Array.isArray(frame.predictions)) {
    warnings.push({
      key: 'predictions.type',
      msg: 'frame.predictions 非数组: ' + _typeOf(frame.predictions),
    });
  }
  if (frame.friction_zones !== undefined && !Array.isArray(frame.friction_zones)) {
    warnings.push({
      key: 'friction_zones.type',
      msg: 'frame.friction_zones 非数组: ' + _typeOf(frame.friction_zones),
    });
  }
  if (frame.parking_goal !== undefined && frame.parking_goal !== null &&
      (typeof frame.parking_goal !== 'object' || Array.isArray(frame.parking_goal))) {
    warnings.push({
      key: 'parking_goal.type',
      msg: 'frame.parking_goal 非对象: ' + _typeOf(frame.parking_goal),
    });
  }

  let skipEntities = false;
  if (frame.entities !== undefined) {
    if (!Array.isArray(frame.entities)) {
      warnings.push({
        key: 'entities.type',
        msg: 'frame.entities 非数组: ' + _typeOf(frame.entities) + '，跳过实体更新',
      });
      skipEntities = true;
    } else {
      // 与 build 过滤一致：仅检查非 ego 实体
      let nonEgoIdx = 0;
      for (const e of frame.entities) {
        if (!e || e.type === 'ego') continue;
        if (!e.type) {
          warnings.push({
            key: 'entities[' + nonEgoIdx + '].type',
            msg: 'entity 缺 type 字段，会被各 View 静默丢弃',
          });
        } else if (!['car', 'suv', 'truck', 'pedestrian', 'traffic_light', 'tl', 'etc_gate', 'stop_line', 'sign', 'ego'].includes(e.type)) {
          warnings.push({
            key: 'entities[' + nonEgoIdx + '].type',
            msg: 'entity 类型 "' + e.type + '" 不在已知集合 {car, suv, truck, pedestrian, traffic_light, etc_gate, stop_line, sign, ego}，可能被静默兜底为 Car',
          });
        }
        nonEgoIdx++;
      }
    }
  }

  /* ── 不变量：实体/ego 高度 vs 路网高度偏差 >2m → 悬空/入地告警 ──
   * 把"红绿灯在路面下 7m"这类问题从静默变成可见 warn。
   * 只对有路网、有坐标的数据做检查，不因此阻断渲染。 */
  if (rn && rn.edges && rn.edges.length > 0) {
    const toCheck = [];
    if (frame.ego && typeof frame.ego === 'object') {
      toCheck.push({ label: 'ego', x: frame.ego.x, y: frame.ego.y, z: frame.ego.z });
    }
    if (frame.entities && Array.isArray(frame.entities)) {
      for (let i = 0; i < frame.entities.length; i++) {
        const e = frame.entities[i];
        if (!e || e.type === 'ego') continue;
        toCheck.push({ label: 'entity[' + i + '](type=' + (e.type || '?') + ')', x: e.x, y: e.y, z: e.z });
      }
    }
    for (const item of toCheck) {
      const rz = _roadHeightAt(rn.edges, item.x, item.y);
      const dz = Math.abs((item.z || 0) - rz);
      if (dz > 2.0) {
        warnings.push({
          key: 'height.' + item.label,
          msg: item.label + ' z=' + (item.z || 0) + 'm 与路面高度 ' + rz + 'm 偏差 ' + dz.toFixed(1) + 'm，可能悬空或入地',
        });
      }
    }
  }

  return { ok: true, frame, rn, skipRoad, skipEgo, skipEntities, warnings };
}

/** 轻量路面高度查询（FrameValidator 零依赖版）
 *  复用 RoadHeight.js 算法：找最近 edge 投影，线性插值 z。 */
function _roadHeightAt(edges, px, py) {
  let minDist = Infinity, resultZ = 0;
  for (const edge of edges) {
    const nodes = edge.nodes;
    if (!nodes || nodes.length < 2) continue;
    const laneWidth = edge.lane_width || LANE_WIDTH;
    const lanes = edge.lanes || 2;
    const halfWidth = (lanes * laneWidth) / 2 + 3;
    for (let i = 0; i < nodes.length - 1; i++) {
      const a = nodes[i], b = nodes[i + 1];
      const ax = a[0], ay = a[1], bx = b[0], by = b[1];
      const dx = bx - ax, dy = by - ay;
      const len2 = dx * dx + dy * dy;
      if (len2 < 1e-6) continue;
      let t = ((px - ax) * dx + (py - ay) * dy) / len2;
      t = Math.max(0, Math.min(1, t));
      const projX = ax + dx * t;
      const projY = ay + dy * t;
      const dist = Math.hypot(px - projX, py - projY);
      if (dist < minDist) {
        minDist = dist;
        const edgeT = (i + t) / (nodes.length - 1);
        const az = a[2] || 0, bz = b[2] || 0;
        resultZ = az + (bz - az) * edgeT;
      }
    }
    if (minDist <= halfWidth) break; // 已在道路范围内，不必继续
  }
  return resultZ;
}

/**
 * M5: cleanEntity — 清洗并规范化单个实体字段（保留 6-DOF 姿态、路面坡度与摩擦系数）
 * @param {object} e 原始实体对象
 * @returns {object|null}
 */
export function cleanEntity(e) {
  if (!e || typeof e !== 'object') return null;
  const etype = e.type || 'car';
  let defLen, defWid, defH;
  switch (etype) {
    case 'pedestrian': defLen = 0.5; defWid = 0.5; defH = 1.75; break;
    case 'bicycle': case 'cyclist': case 'motorcycle': defLen = 1.8; defWid = 0.6; defH = 1.6; break;
    case 'truck': case 'bus': defLen = 8.0; defWid = 2.5; defH = 4.0; break;
    case 'suv': defLen = 4.7; defWid = 2.0; defH = 1.85; break;
    case 'car': default: defLen = 4.6; defWid = 2.0; defH = 1.5; break;
  }
  const rawLen = e.length != null ? e.length : (e.len != null ? e.len : defLen);
  const rawWid = e.width != null ? e.width : (e.wid != null ? e.wid : defWid);
  const rawH   = e.height != null ? e.height : defH;
  const evx = (typeof e.vx === 'number' && isFinite(e.vx)) ? e.vx : 0;
  const evy = (typeof e.vy === 'number' && isFinite(e.vy)) ? e.vy : 0;
  const speed = (typeof e.speed === 'number' && isFinite(e.speed))
    ? e.speed
    : ((typeof e.spd === 'number' && isFinite(e.spd)) ? e.spd : Math.hypot(evx, evy));

  return {
    id: e.id,
    type: etype,
    x: (typeof e.x === 'number' && isFinite(e.x)) ? e.x : 0,
    y: (typeof e.y === 'number' && isFinite(e.y)) ? e.y : 0,
    z: (typeof e.z === 'number' && isFinite(e.z)) ? e.z : 0,
    pitch: (typeof e.pitch === 'number' && isFinite(e.pitch)) ? e.pitch : 0,
    roll: (typeof e.roll === 'number' && isFinite(e.roll)) ? e.roll : 0,
    mu_scale: (typeof e.mu_scale === 'number' && isFinite(e.mu_scale)) ? e.mu_scale : 1.0,
    road_grade: (typeof e.road_grade === 'number' && isFinite(e.road_grade)) ? e.road_grade : 0,
    heading: (typeof e.heading === 'number' && isFinite(e.heading))
      ? e.heading
      : ((typeof e.h === 'number' && isFinite(e.h)) ? e.h : 0),
    speed,
    steer: (typeof e.steer === 'number' && isFinite(e.steer)) ? e.steer : 0,
    length: rawLen,
    width: rawWid,
    height: rawH,
    ai_state: e.ai_state || e.ai || '',
    lights: typeof e.lights === 'number' ? e.lights : 0,
    vx: evx,
    vy: evy,
    throttle: (typeof e.throttle === 'number' && isFinite(e.throttle)) ? e.throttle : 0,
    brake: (typeof e.brake === 'number' && isFinite(e.brake)) ? e.brake : 0,
    state: e.state || '',
    progress: (typeof e.progress === 'number' && isFinite(e.progress)) ? e.progress : 0,
    remain_s: (typeof e.remain_s === 'number' && isFinite(e.remain_s)) ? e.remain_s : 0,
    parked: !!e.parked,
    stop_x: e.stop_x,
    stop_y: e.stop_y,
    lane_offset: e.lane_offset,
  };
}

/**
 * M5: validateSceneFrame — 校验并透传 predictions、friction_zones、parking_goal 等数据
 * @param {*} raw 原始场景帧
 * @returns {{ ok: boolean, clean?: object, frame?: object, rn?: object, warnings: Array }}
 */
export function validateSceneFrame(raw) {
  const result = validateFrame(raw);
  if (!result.ok || !result.frame) {
    return { ok: false, clean: null, frame: null, warnings: result.warnings };
  }
  const frame = result.frame;
  const warnings = [...result.warnings];

  let predictions = [];
  if (frame.predictions !== undefined) {
    if (Array.isArray(frame.predictions)) {
      predictions = frame.predictions;
    } else {
      warnings.push({
        key: 'predictions.type',
        msg: 'frame.predictions 非数组: ' + _typeOf(frame.predictions),
      });
    }
  }

  let friction_zones = [];
  if (frame.friction_zones !== undefined) {
    if (Array.isArray(frame.friction_zones)) {
      friction_zones = frame.friction_zones;
    } else {
      warnings.push({
        key: 'friction_zones.type',
        msg: 'frame.friction_zones 非数组: ' + _typeOf(frame.friction_zones),
      });
    }
  }

  let parking_goal = null;
  if (frame.parking_goal !== undefined) {
    if (frame.parking_goal === null || (typeof frame.parking_goal === 'object' && !Array.isArray(frame.parking_goal))) {
      parking_goal = frame.parking_goal;
    } else {
      warnings.push({
        key: 'parking_goal.type',
        msg: 'frame.parking_goal 非对象: ' + _typeOf(frame.parking_goal),
      });
    }
  }

  let cleanEgo = null;
  if (frame.ego && typeof frame.ego === 'object') {
    cleanEgo = {
      ...frame.ego,
      pitch: (typeof frame.ego.pitch === 'number' && isFinite(frame.ego.pitch)) ? frame.ego.pitch : 0,
      roll: (typeof frame.ego.roll === 'number' && isFinite(frame.ego.roll)) ? frame.ego.roll : 0,
      mu_scale: (typeof frame.ego.mu_scale === 'number' && isFinite(frame.ego.mu_scale)) ? frame.ego.mu_scale : 1.0,
      road_grade: (typeof frame.ego.road_grade === 'number' && isFinite(frame.ego.road_grade)) ? frame.ego.road_grade : 0,
    };
  }

  const clean = {
    ...frame,
    ego: cleanEgo,
    predictions,
    friction_zones,
    parking_goal,
  };

  return {
    ok: true,
    clean,
    frame,
    rn: result.rn,
    skipRoad: result.skipRoad,
    skipEgo: result.skipEgo,
    skipEntities: result.skipEntities,
    warnings,
  };
}
