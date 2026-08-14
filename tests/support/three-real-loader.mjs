/** three-real-loader.mjs — Node.js 自定义 loader hook（真实 three.js 版）
 *
 * 拦截 import 'three' → tools/flowboard/vendor/three/three.module.js（真实 ESM），
 * 供需要读取 InstancedMesh 矩阵等真实数值的测试使用（shim 版 three 是 Proxy 桩，
 * 只能做"不抛错"冒烟，无法做几何断言）。
 *
 * 用法（通过 three-real-preload.mjs 间接注册）：
 *   node --import ./tests/support/three-real-preload.mjs <test.mjs>
 */
const REAL_THREE_URL = new URL('../../tools/flowboard/vendor/three/three.module.js', import.meta.url).href;

export async function resolve(specifier, context, nextResolve) {
  if (specifier === 'three' || specifier.startsWith('three/')) {
    return { url: REAL_THREE_URL, format: 'module', shortCircuit: true };
  }
  return nextResolve(specifier, context);
}

export async function load(url, context, nextLoad) {
  const result = await nextLoad(url, context);
  // 与 three-loader.mjs 同一约定：vis/ 模块未显式 import THREE 的，注入全局引用
  if (result.source && url.includes('/tools/flowboard/js/vis/')) {
    const src = result.source.toString();
    if (!src.includes('import * as THREE from') && /\bTHREE\./.test(src)) {
      result.source = 'const THREE = globalThis.THREE;\n' + src;
    }
  }
  return result;
}
