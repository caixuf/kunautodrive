/**
 * TopologyWorker.js — Web Worker for FlowBoard Data Pipeline
 *
 * Offloads CPU-intensive payload processing from the main 3D render thread:
 * 1. JSON parsing & decompression (if string payload is passed).
 * 2. Fast hash computation for road_network topology diffs.
 * 3. Entity duplicate detection & normalization.
 */

const g = typeof globalThis !== 'undefined' ? globalThis : null;

if (g && typeof g.addEventListener === 'function') {
  g.addEventListener('message', function(e) {
    const { id, type, payload } = e.data || {};
    if (type === 'PROCESS_TOPOLOGY') {
      try {
        let data = payload;
        if (typeof payload === 'string') {
          data = JSON.parse(payload);
        }
        
        const processed = processTopologyPayload(data);
        if (typeof g.postMessage === 'function') {
          g.postMessage({ id, success: true, result: processed });
        }
      } catch (err) {
        if (typeof g.postMessage === 'function') {
          g.postMessage({ id, success: false, error: err.message });
        }
      }
    }
  });
}

export function processTopologyPayload(topoData) {
  if (!topoData || typeof topoData !== 'object') {
    return { ok: false, reason: 'Invalid object' };
  }

  let frame = topoData;
  if (topoData.metrics && topoData.metrics.scene) frame = topoData.metrics.scene;
  else if (topoData.scene) frame = topoData.scene;

  if (!frame || typeof frame !== 'object') {
    return { ok: false, reason: 'Invalid frame' };
  }

  const rn = frame.road_network || frame.roads;
  let roadHash = '';
  if (rn && rn.edges && Array.isArray(rn.edges)) {
    let h = 0;
    const str = rn.edges.map(edge => `${edge.id}:${edge.type}:${edge.lanes}:${edge.length || edge.length_m || 0}`).join('|');
    for (let i = 0; i < str.length; i++) {
      h = ((h << 5) - h) + str.charCodeAt(i);
      h |= 0;
    }
    roadHash = 'rn_' + h;
  }

  // Deduplicate entities
  const rawEntities = Array.isArray(frame.entities) ? frame.entities : [];
  const validEntities = [];
  const seen = new Set();
  for (let i = 0; i < rawEntities.length; i++) {
    const ent = rawEntities[i];
    if (!ent || ent.type === 'ego') continue;
    const key = (ent.type || 'car') + ':' + (ent.id != null ? ent.id : i);
    if (!seen.has(key)) {
      seen.add(key);
      validEntities.push(ent);
    }
  }

  return {
    ok: true,
    scenarioName: frame.scenario_name || '',
    roadHash,
    hasRoadNetwork: !!(rn && rn.edges && rn.edges.length > 0),
    ego: frame.ego || null,
    entities: validEntities,
    trajectoryPath: Array.isArray(frame.trajectory_path) ? frame.trajectory_path : null,
    env: {
      lighting: frame.lighting || 'day',
      weather: frame.weather || 'clear',
      visibilityM: Number.isFinite(frame.visibility_m) ? frame.visibility_m : 20000
    },
    raw: topoData
  };
}
