/**
 * WorldLayoutClient.js — 权威程序化世界布局客户端（C++ WorldEngine 单一事实源通道）
 *
 * 工作流程：
 *   1. SceneDirector 从 metrics.scene.world_layout_hash 获知后端本帧权威世界布局指纹；
 *   2. 若本地已缓存该指纹对应的 `/api/world/layout` 结果，立即返回权威 JSON；
 *   3. 若尚未拉取，先异步发起 GET `/api/world/layout?hash=<hash>`（同指纹严格去重），
 *      期间同步返回确定性 WorldCompiler.js 预览布局兜底，待 HTTP 返回后触发单次热重建；
 *   4. 无后端（如 /map_preview 或离线单元测试）时自动使用 WorldCompiler.js 输出。
 */

import { compileWorld } from '../world/WorldCompiler.js';
import { roadNetworkHash } from '../store/SceneStore.js';

const _authoritativeCache = new Map(); // hash -> worldLayout JSON
const _inFlight = new Map();           // hash -> Promise<worldLayout|null>
const _fallbackCache = new Map();      // fallbackKey -> compiled worldLayout

const _FALLBACK_MAX = 8;

function _putFallback(key, value) {
  if (_fallbackCache.has(key)) _fallbackCache.delete(key);
  if (_fallbackCache.size >= _FALLBACK_MAX) {
    const oldest = _fallbackCache.keys().next().value;
    _fallbackCache.delete(oldest);
  }
  _fallbackCache.set(key, value);
  return value;
}

/** 获取已缓存的 C++ 权威世界布局（未命中返回 null）。 */
export function getCachedWorldLayout(hash) {
  if (!hash || typeof hash !== 'string') return null;
  return _authoritativeCache.get(hash) || null;
}

/** 异步从 monitor_server 拉取 C++ 编译好的世界布局 JSON（按 hash 去重，静默容错）。 */
export function fetchWorldLayout(hash, onReady) {
  if (!hash || typeof hash !== 'string') return Promise.resolve(null);
  const existing = _authoritativeCache.get(hash);
  if (existing) {
    if (typeof onReady === 'function') onReady(existing);
    return Promise.resolve(existing);
  }
  if (_inFlight.has(hash)) {
    const p = _inFlight.get(hash);
    if (typeof onReady === 'function') {
      p.then((layout) => { if (layout) onReady(layout); });
    }
    return p;
  }
  if (typeof globalThis.fetch !== 'function') {
    return Promise.resolve(null);
  }

  const url = `/api/world/layout?hash=${encodeURIComponent(hash)}`;
  const promise = globalThis.fetch(url)
    .then((resp) => (resp && resp.ok ? resp.json() : null))
    .then((layout) => {
      _inFlight.delete(hash);
      if (!layout || !Array.isArray(layout.poles)) return null;
      if (layout.world_hash && layout.world_hash !== hash) {
        // 即使后端切了更新一轮布局，也先按各自 hash 写入缓存供匹配使用
        _authoritativeCache.set(layout.world_hash, layout);
        return null;
      }
      _authoritativeCache.set(hash, layout);
      if (typeof onReady === 'function') onReady(layout);
      return layout;
    })
    .catch(() => {
      _inFlight.delete(hash);
      return null;
    });

  _inFlight.set(hash, promise);
  return promise;
}

/**
 * 同步解析当前应渲染的 worldLayout：
 *   - 若 C++ 权威 layout 已到达且匹配 worldLayoutHash，直接返回；
 *   - 否则返回缓存的确定性 WorldCompiler.js 兜底编译结果。
 */
export function resolveWorldLayout({ worldLayoutHash = '', roadNetwork = null, mapData = null, worldGen = null } = {}) {
  const auth = getCachedWorldLayout(worldLayoutHash);
  if (auth) return auth;

  const edges = roadNetwork && Array.isArray(roadNetwork.edges) ? roadNetwork.edges : [];
  if (!edges.length) return null;

  const effectiveWg = worldGen || (roadNetwork && roadNetwork.world_gen) || (mapData && mapData.world_gen) || null;
  const seed = (mapData && mapData.random_seed != null) ? mapData.random_seed : null;
  const wgSig = effectiveWg ? (typeof effectiveWg === 'string' ? effectiveWg : JSON.stringify(effectiveWg)) : '';
  const fbKey = `${roadNetworkHash(roadNetwork)}|${wgSig}|${seed != null ? seed : ''}`;

  if (_fallbackCache.has(fbKey)) {
    return _fallbackCache.get(fbKey);
  }
  const compiled = compileWorld(roadNetwork, effectiveWg, seed);
  return _putFallback(fbKey, compiled);
}

/** 清空全部缓存（供单元测试与切换场景隔离调用）。 */
export function clearWorldLayoutCache() {
  _authoritativeCache.clear();
  _inFlight.clear();
  _fallbackCache.clear();
}
