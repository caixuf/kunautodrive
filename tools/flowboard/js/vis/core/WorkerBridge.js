/**
 * WorkerBridge.js — Manager for background Web Worker data parsing
 *
 * Bridges the main 3D visualization thread with TopologyWorker.
 * If Web Worker is not available (Node.js test environments or file:// restrictions),
 * transparently executes synchronous in-thread processing with zero interface difference.
 */

let _worker = null;
let _msgId = 0;
const _pendingCallbacks = new Map();
let _workerSupported = null;

export function initWorkerBridge() {
  const g = typeof globalThis !== 'undefined' ? globalThis : null;
  if (!g || typeof g.Worker === 'undefined' || typeof g.URL === 'undefined') {
    _workerSupported = false;
    return;
  }

  try {
    const workerUrl = new g.URL('../worker/TopologyWorker.js', import.meta.url);
    _worker = new g.Worker(workerUrl, { type: 'module' });
    _worker.onmessage = function(e) {
      const { id, success, result, error } = e.data || {};
      const cb = _pendingCallbacks.get(id);
      if (cb) {
        _pendingCallbacks.delete(id);
        if (success) cb.resolve(result);
        else cb.reject(new Error(error || 'Worker error'));
      }
    };
    _worker.onerror = function(err) {
      console.warn('[WorkerBridge] Web Worker error, falling back to sync processing:', err);
      _workerSupported = false;
    };
    _workerSupported = true;
  } catch (e) {
    console.warn('[WorkerBridge] Web Worker unavailable (using sync fallback):', e.message);
    _workerSupported = false;
  }
}

export function isWorkerSupported() {
  if (_workerSupported === null) {
    initWorkerBridge();
  }
  return !!_workerSupported;
}

export function processTopologyAsync(payload) {
  if (!isWorkerSupported() || !_worker) {
    return Promise.resolve(syncProcessTopology(payload));
  }

  return new Promise((resolve, reject) => {
    const id = ++_msgId;
    _pendingCallbacks.set(id, { resolve, reject });
    _worker.postMessage({ id, type: 'PROCESS_TOPOLOGY', payload });
  });
}

export function syncProcessTopology(topoData) {
  if (!topoData || typeof topoData !== 'object') {
    return { ok: false, raw: topoData };
  }
  let frame = topoData;
  if (topoData.metrics && topoData.metrics.scene) frame = topoData.metrics.scene;
  else if (topoData.scene) frame = topoData.scene;
  if (!frame || typeof frame !== 'object') {
    return { ok: false, raw: topoData };
  }

  const rawEntities = Array.isArray(frame.entities) ? frame.entities : [];
  const validEntities = rawEntities.filter(e => e && e.type !== 'ego');

  return {
    ok: true,
    scenarioName: frame.scenario_name || '',
    ego: frame.ego || null,
    entities: validEntities,
    trajectoryPath: Array.isArray(frame.trajectory_path) ? frame.trajectory_path : null,
    raw: topoData
  };
}
