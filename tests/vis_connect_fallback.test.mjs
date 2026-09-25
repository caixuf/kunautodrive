import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { ok, done } from './test-utils.mjs';

/**
 * 契约：后端地址连不上时，必须回退到本页 origin，而不是直接掉进 doSimulate()
 * 的演示模式（演示模式没有自车遥测，3D 里的车会一直冻在原点）。
 *
 * 背景：serverUrl 会从 localStorage['flowboard'].url 恢复；旧版 Python dashboard
 * 时代存下的端口（例如 8801）在 flowmond 接手后会永久失效，页面因此静默冻死。
 */
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const appJs = fs.readFileSync(
  path.join(repo, 'tools/flowboard/js/app.js'), 'utf8');

console.log('=== backend-origin fallback contract ===\n');

const iDecl = appJs.indexOf('var _originFallbackTried = false;');
const iFallback = appJs.indexOf('var pageOrigin = defaultServerUrl();');
const iGuard = appJs.indexOf('if (!_originFallbackTried && serverUrl !== pageOrigin)');
const iReassign = appJs.indexOf('serverUrl = pageOrigin;');
const iRetries = appJs.indexOf('connectRetries++;', iFallback);
const iSimulate = appJs.indexOf('doSimulate();', iFallback);

ok('a one-shot origin fallback flag is declared', iDecl !== -1);
ok('connect failure computes the page origin', iFallback !== -1);
ok('fallback only fires when the configured url differs from origin', iGuard !== -1);
ok('fallback rewrites serverUrl to the origin', iReassign > iFallback);
ok('fallback happens before retry counting and the demo fallback',
  iFallback > 0 && iRetries > iFallback && iSimulate > iRetries);
ok('fallback persists the corrected url (self-heal on next load)',
  appJs.indexOf('saveState();', iReassign) > iReassign &&
  appJs.indexOf('saveState();', iReassign) - iReassign < 400);
ok('fallback is one-shot: it returns instead of looping',
  appJs.indexOf('return;', iReassign) > iReassign &&
  appJs.indexOf('return;', iReassign) - iReassign < 400);
ok('successful connect resets the fallback flag',
  appJs.indexOf('_originFallbackTried = false;', appJs.indexOf('startSSE();')) > 0);
ok('demo fallback is still reachable after retries', iSimulate > 0);

done();
