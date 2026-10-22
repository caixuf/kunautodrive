/**
 * StatsView.js — 性能监控面板（FPS / drawcall / triangle / 纹理缓存）
 *
 * 挂在 DOM 层（非 3D 场景），每帧读取 renderer.info 统计渲染指标。
 * 默认显示在右上角，可通过 CSS 调整位置。
 *
 * 使用方式：
 *   const stats = createStatsView();
 *   document.body.appendChild(stats.dom);
 *   // 每帧渲染循环中
 *   stats.update(renderer);
 *
 * 显示内容：
 *   FPS      — 帧率（绿色 >=50，黄色 >=30，红色 <30）
 *   ms       — 帧耗时
 *   Draw     — drawcall 数量
 *   Tri      — 三角形数量
 *   Geo      — geometry 内存占用
 *   Tex      — texture 内存占用（含 CanvasTextureFactory 缓存）
 *   Cache    — CanvasTextureFactory 缓存条目数
 */

import { getTextureCacheSize } from '../utils/CanvasTextureFactory.js';

const UPDATE_INTERVAL = 500;  // 每 500ms 更新一次显示（避免每帧 DOM 操作）

export function createStatsView() {
  const container = document.createElement('div');
  container.id = 'stats-view';
  container.style.cssText = `
    position: fixed;
    top: 8px;
    right: 8px;
    z-index: 9999;
    background: rgba(0, 0, 0, 0.75);
    color: #0f0;
    font-family: 'JetBrains Mono', 'Consolas', monospace;
    font-size: 11px;
    line-height: 1.4;
    padding: 8px 10px;
    border-radius: 4px;
    border: 1px solid rgba(0, 255, 0, 0.3);
    pointer-events: none;
    user-select: none;
    min-width: 120px;
  `;

  const rows = {
    fps:   _createRow(container, 'FPS', '#0f0'),
    ms:    _createRow(container, 'ms', '#0f0'),
    draw:  _createRow(container, 'Draw', '#8af'),
    tri:   _createRow(container, 'Tri', '#8af'),
    geo:   _createRow(container, 'Geo', '#fa0'),
    tex:   _createRow(container, 'Tex', '#fa0'),
    cache: _createRow(container, 'Cache', '#f0f'),
  };

  let lastTime = performance.now();
  let frameCount = 0;
  let fps = 0;
  let lastUpdate = 0;

  function _createRow(parent, label, color) {
    const row = document.createElement('div');
    row.style.cssText = 'display:flex;justify-content:space-between;';
    const labelEl = document.createElement('span');
    labelEl.textContent = label;
    labelEl.style.color = '#888';
    const valueEl = document.createElement('span');
    valueEl.style.color = color;
    valueEl.textContent = '-';
    row.appendChild(labelEl);
    row.appendChild(valueEl);
    parent.appendChild(row);
    return valueEl;
  }

  function _formatNumber(n) {
    if (n >= 1000000) return (n / 1000000).toFixed(1) + 'M';
    if (n >= 1000) return (n / 1000).toFixed(1) + 'K';
    return String(n);
  }

  /**
   * 每帧调用（在 renderer.render 之后）
   * @param {THREE.WebGLRenderer} renderer
   */
  function update(renderer) {
    frameCount++;
    const now = performance.now();

    // 每 500ms 更新一次 DOM
    if (now - lastUpdate < UPDATE_INTERVAL) return;

    fps = Math.round(frameCount * 1000 / (now - lastTime));
    const ms = ((now - lastTime) / frameCount).toFixed(1);
    frameCount = 0;
    lastTime = now;
    lastUpdate = now;

    // FPS 颜色：绿 >=50，黄 >=30，红 <30
    const fpsColor = fps >= 50 ? '#0f0' : fps >= 30 ? '#ff0' : '#f00';
    rows.fps.style.color = fpsColor;
    rows.fps.textContent = fps;
    rows.ms.textContent = ms;

    if (renderer && renderer.info) {
      const info = renderer.info;
      rows.draw.textContent = _formatNumber(info.render.calls);
      rows.tri.textContent = _formatNumber(info.render.triangles);
      /* 🔴 Geo / Tex 显示的是**个数**，不是字节。
       * three 的 `info.memory.geometries` / `.textures` 是计数器（几何体数、纹理数），
       * 旧实现写 `_formatBytes(n * 1000)` 把它们乘 1000 再按 KB/MB 格式化 ——
       * 于是「215 个几何体」被显示成「Geo 215.0KB」，一个凭空捏造的内存读数。
       * 这里改为如实显示计数：泄漏排查（纹理缓存/几何体只增不减）看的正是这两个
       * 数字的单调增长，伪装成字节反而把判据盖掉了。 */
      rows.geo.textContent = _formatNumber(info.memory.geometries);
      rows.tex.textContent = _formatNumber(info.memory.textures);
    }

    rows.cache.textContent = getTextureCacheSize();
  }

  function dispose() {
    if (container.parentNode) {
      container.parentNode.removeChild(container);
    }
  }

  return { dom: container, update, dispose };
}
