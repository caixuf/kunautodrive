/** three-real-preload.mjs — 预加载：真实 three.js + loader 注册
 *
 * 与 three-preload.mjs（Proxy shim 版）对称，本版给「需要真实几何数值」的
 * 测试用：InstancedMesh.getMatrixAt / BufferGeometry.boundingSphere 等真实 API。
 *
 * 用法：
 *   node --import ./tests/support/three-real-preload.mjs tests/junction_markings.test.mjs
 */
import { register } from 'node:module';
register('./three-real-loader.mjs', import.meta.url);

import * as THREE from '../../tools/flowboard/vendor/three/three.module.js';
globalThis.THREE = THREE;

// 最小 DOM shim（与 three-preload.mjs 同契约：纹理生成路径走 canvas 桩）
const _ctx = new Proxy({}, { get: (t, k) => (k === 'measureText' ? () => ({ width: 0 }) : () => ({ addColorStop() {} }) ) });
globalThis.document = {
  createElement: (tag) => (tag === 'canvas' ? { width: 0, height: 0, getContext: () => _ctx } : {}),
  getElementById: () => null,
  querySelector: () => null,
  querySelectorAll: () => [],
  body: { appendChild() {}, style: {} },
  addEventListener() {}, hidden: false,
};
globalThis.window = globalThis.window || { THREE, document: globalThis.document, location: { search: '' } };
globalThis.ImageData = class {
  constructor(data, width = 0, height = 0) {
    this.data = data || new Uint8ClampedArray(0);
    this.width = width; this.height = height;
  }
};
