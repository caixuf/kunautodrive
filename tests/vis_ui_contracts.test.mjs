/**
 * FlowBoard HUD 与事故时间轴 UI 契约回归门禁：
 *   1. HUD 速度区宽度与 nowrap/overflow 约束（防“目标 XX km/h · 指令 YY”折行溢出）
 *   2. 事故时间轴必须包含可点击的收起按钮（✕）与可感知反馈的诊断追溯按钮
 *   3. 事件时间格式校验逻辑必须防护 NaN，禁止渲染出 "(NaNs)"
 */

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { ok, done } from './test-utils.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(__dirname, '..');

const css = readFileSync(resolve(ROOT, 'tools/flowboard/css/style.css'), 'utf-8');
const html = readFileSync(resolve(ROOT, 'tools/flowboard/index.html'), 'utf-8');
const appJs = readFileSync(resolve(ROOT, 'tools/flowboard/js/app.js'), 'utf-8');

console.log('=== FlowBoard HUD 与时间轴 UI 契约测试 ===\n');

ok('CSS: #drive-hud 启用 overflow:hidden 防止内容越界',
  /#drive-hud\{[^}]*overflow:hidden/.test(css));

const speedWidthMatch = css.match(/#drive-hud \.dh-speed\{[^}]*min-width:(\d+)px/);
ok('CSS: #drive-hud .dh-speed min-width >= 150px',
  Boolean(speedWidthMatch && Number(speedWidthMatch[1]) >= 150));

ok('CSS: #drive-hud em 禁止折行 (white-space:nowrap)',
  /#drive-hud em\{[^}]*white-space:nowrap/.test(css));

ok('HTML: #incident-timeline 包含诊断追溯按钮 (#incident-trace-btn)',
  html.includes('id="incident-trace-btn"') && html.includes('flowboard.refreshIncidentTrace()'));

ok('HTML: #incident-timeline 包含关闭/收起按钮 (dismissIncidentTimeline)',
  html.includes('flowboard.dismissIncidentTimeline()'));

ok('JS: app.js 导出 dismissIncidentTimeline 且按签名防轮询重弹',
  appJs.includes('dismissIncidentTimeline: dismissIncidentTimeline') &&
  appJs.includes('_incidentDismissedSig'));

ok('JS: app.js refreshIncidentTrace 提供按钮加载反馈与错误提示',
  appJs.includes('追溯中…') && appJs.includes('诊断追溯请求失败'));

ok('JS: app.js 事件时间渲染使用 Number.isFinite 防护 NaNs',
  appJs.includes('Number.isFinite(rawT)') && appJs.includes("evSec.toFixed(1) + 's)'"));

done();
