import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { ok, done } from './test-utils.mjs';

/**
 * 车道可观测性契约：四组数据（地图/启发式标线 · 规划参考线 · 权威车道定位 ·
 * 感知车道线）必须能同时到达前端并可区分，否则"是不是感知出问题"无法判读。
 *
 * 覆盖两端：
 *   后端 —— lane_detection 产出世界坐标 pts；monitor 透传到 metrics.perceived_lanes；
 *          pipeline 真的启用了 lane_detection（否则话题永远没数据）。
 *   前端 —— LaneDiagView 读 store 的 planningDebug/laneMatch/perceivedLanes、
 *          坐标只走 Coord、颜色只从 tokens 出；开关与 HUD 已接线。
 */
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const read = (p) => fs.readFileSync(path.join(repo, p), 'utf8');

const laneNode   = read('modules/adas_nodes/lane_detection_node.c');
const monitorNode = read('modules/adas_nodes/monitor_node.c');
const pipeline   = read('config/pipeline.json');
const director   = read('tools/flowboard/js/vis/director/SceneDirector.js');
const view       = read('tools/flowboard/js/vis/view/LaneDiagView.js');
const tokens     = read('tools/flowboard/js/vis/theme/tokens.js');
const mainJs     = read('tools/flowboard/js/vis/main.js');
const appJs      = read('tools/flowboard/js/app.js');
const indexHtml  = read('tools/flowboard/index.html');

console.log('=== lane observability contracts ===\n');

/* ── 后端：数据真的产生并透传 ── */
ok('lane_detection emits world-frame polyline pts',
  laneNode.includes('cJSON_AddItemToObject(b, "pts", pts)'));
ok('lane_detection emits ego pose + model frame tag',
  laneNode.includes('cJSON_AddItemToObject(root, "ego", ego_obj)') &&
  laneNode.includes('"model_frame"'));
ok('lane_detection tracks ego y/heading for pts sampling',
  laneNode.includes('g.ego_heading') && laneNode.includes('g.ego_y'));
ok('monitor caches perception/lanes payload',
  monitorNode.includes('on_perceived_lanes') &&
  monitorNode.includes('g.perceived_lanes_json'));
ok('monitor forwards it as metrics.perceived_lanes',
  monitorNode.includes('"perceived_lanes"') &&
  monitorNode.includes('TOPIC_PERCEPTION_LANES, on_perceived_lanes'));
ok('pipeline actually enables lane_detection (else the topic never has data)',
  pipeline.includes('"name": "lane_detection"') &&
  pipeline.includes('liblane_detection_node.so') &&
  pipeline.includes('"topic": "perception/lanes"'));

/* ── 前端：四组数据都进 store，且渲染契约不被破坏 ── */
ok('director writes the three diag metrics into the store',
  director.includes('store.planningDebug') &&
  director.includes('store.laneMatch') &&
  director.includes('store.perceivedLanes'));
ok('director registers + mounts the laneDiag view',
  director.includes("ViewRegistry.register('laneDiag'") &&
  /\[.infra.,\s*\[[^\]]*'laneDiag'/.test(director));
ok('director exposes getLaneDiagView for the toggle path',
  director.includes('getLaneDiagView'));

ok('view reads all four sources',
  view.includes('store.planningDebug') && view.includes('store.laneMatch') &&
  view.includes('store.perceivedLanes') && view.includes('store.roadNetwork'));
ok('view consumes planning ref line and detected boundaries',
  view.includes('planningDebug.ref_x') && view.includes('boundaries'));
ok('view extends the shared diag color token (no bare hex)',
  tokens.includes('export const LANE_DIAG') && view.includes("from '../theme/tokens.js'") &&
  !/0x[0-9a-fA-F]{6}/.test(view));
ok('view routes coordinates through Coord (no bare -y flip / atan2)',
  view.includes("from '../math/Coord.js'") &&
  !/z:\s*-\(/.test(view) && !view.includes('Math.atan2'));
ok('view defaults to hidden (diagnostic overlay must not alter default look)',
  /let visible = false/.test(view) && view.includes('group.visible = false'));

/* ── 开关与 HUD ── */
ok('main.js exports the overlay toggle',
  mainJs.includes('export function setLaneDiagVisible') &&
  mainJs.includes('getLaneDiagView()'));
ok('app.js wires K key + namespace handler + ?lanediag=1',
  appJs.includes("if (key === 'k')") && appJs.includes('toggleLaneDiag') &&
  appJs.includes("params.get('lanediag') === '1'"));
ok('app.js updates the HUD from the live metrics',
  appJs.includes('function updateLaneDiagHud') &&
  appJs.includes('m.planning_debug') && appJs.includes('m.perceived_lanes'));
ok('HUD exposes all four readouts',
  indexHtml.includes('id="ld-ref"') && indexHtml.includes('id="ld-lattice"') &&
  indexHtml.includes('id="ld-match"') && indexHtml.includes('id="ld-det"'));
ok('HUD has a verdict line driven by observable mismatches',
  indexHtml.includes('id="ld-verdict"') &&
  appJs.includes('自车压车道线') && appJs.includes('规划横向未收敛'));
ok('overlay toggle button exists with the K hint',
  indexHtml.includes('id="lane-diag-toggle"') && indexHtml.includes('快捷键 K'));

done();
