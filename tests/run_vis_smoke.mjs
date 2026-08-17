/* run_vis_smoke.mjs — 跨平台 vis 冒烟跑批（替代 bash `for t in tests/vis_*.test.mjs`，
 * 该写法在 Windows npm（cmd.exe）下解析失败，导致作者本地 `npm run vis:check:all`
 * 无法执行）。规则：
 *   - 默认用 three-preload（shim）跑 tests/vis_*.test.mjs（不抛错冒烟）；
 *   - 需要真实几何/真实 three 的测试（REAL_THREE 白名单）改用 three-real-preload；
 *     vis_roadaxis 做精确 CatmullRom 几何断言，shim 的 CatmullRomCurve3 是 Proxy 桩
 *     会采样出全零 spine（假红/假绿），必须走真实 three。
 *   - 任一失败即整体退出码 1，与旧 bash 循环一致。
 * junction_markings.test.mjs 不以 vis_ 开头，由 vis:check:junction 单独跑（真实 three）。 */
import { readdirSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const TESTS = join(ROOT, 'tests');

const REAL_THREE = new Set([
  'vis_roadaxis.test.mjs',  // 精确 CatmullRom 几何断言
  'vis_connector_clip.test.mjs',  // 路口 mesh/instanced 断言需要真实 scene 树
]);

const tests = readdirSync(TESTS)
  .filter((f) => f.startsWith('vis_') && f.endsWith('.test.mjs'))
  .sort();

let failed = false;
for (const t of tests) {
  const pre = REAL_THREE.has(t) ? 'three-real-preload' : 'three-preload';
  console.log(`--- ${t} ---`);
  const r = spawnSync(process.execPath,
    ['--import', `./tests/support/${pre}.mjs`, join('tests', t)],
    { stdio: 'inherit', cwd: ROOT });
  if (r.status !== 0) failed = true;
}
process.exit(failed ? 1 : 0);
