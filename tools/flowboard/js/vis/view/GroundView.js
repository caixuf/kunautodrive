/**
 * GroundView.js — 地面（城市街区卫星图占位）
 *
 * 程序化生成"俯视城市"质感的地面纹理（占位卫星图素材）：
 *   - 不再是纯草地，而是街区网格 + 建筑屋顶色块 + 绿地点缀
 *   - 未来接入真实卫星图（OSM 拉取瓦片）时只需替换 _groundTexture 的
 *     生成来源，其余构建逻辑不变
 * 位置 y=-0.05（路面上方 0.10m，防 z-fight）。
 */

import { SCENE } from '../theme/tokens.js';

const GROUND_BASE = SCENE.groundBase;  // 城市地块基底（偏土绿，比纯草更中性）
const SR_GROUND_TINT = SCENE.srGroundTint;  // SR/BEV 深色冷底 tint
let _cityBlockTexture = null;
let _naturalGroundTexture = null;
let _authoritativeMapMode = false;

/** 判定当前路网是否为权威/多路段真实地图（OSM lane_data / map_junctions / >12 条路） */
export function isAuthoritativeMap(rn) {
  if (!rn || typeof rn !== 'object') return false;
  if (rn.lane_data && Object.keys(rn.lane_data).length > 0) return true;
  if (Array.isArray(rn.map_junctions) && rn.map_junctions.length > 0) return true;
  if (Array.isArray(rn.edges) && rn.edges.length > 12) return true;
  return false;
}

/** 供同周期先执行的 RoadView.build(rn) 同步地图模式，确保首帧 ground.build(20000) 即切换自然地表纹理 */
export function setGroundAuthoritativeMode(flag) {
  _authoritativeMapMode = Boolean(flag);
}

/* 程序化自然城市草坪/壤土地表纹理：周期无缝多尺度有机噪声，
 * 无假网格街道、无随机色块矩形、无噪点斑点，专供真实 OSM 路网与建筑垫底。 */
function buildNaturalGroundTexture() {
  if (_naturalGroundTexture || typeof document === 'undefined') return _naturalGroundTexture;

  const SIZE = 512;
  const MASK = SIZE - 1;
  const canvas = document.createElement('canvas');
  canvas.width = SIZE; canvas.height = SIZE;
  const ctx = canvas.getContext('2d');
  if (!ctx || typeof ctx.getImageData !== 'function') return null;

  ctx.fillStyle = '#677055';
  ctx.fillRect(0, 0, SIZE, SIZE);

  const imageData = ctx.getImageData(0, 0, SIZE, SIZE);
  if (!imageData || !imageData.data || !imageData.data.length) return null;
  const data = imageData.data;

  const hashGrid = (gx, gy, period, salt) => {
    const xw = ((gx % period) + period) % period;
    const yw = ((gy % period) + period) % period;
    let h = (xw * 374761393 + yw * 668265263 + salt * 1442695041) | 0;
    h = Math.imul(h ^ (h >>> 13), 1274126177);
    return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
  };

  const smoothNoise = (px, py, scale, salt) => {
    const period = Math.max(2, Math.round(SIZE / scale));
    const fx = px / scale, fy = py / scale;
    const x0 = Math.floor(fx), y0 = Math.floor(fy);
    const tx = fx - x0, ty = fy - y0;
    const ux = tx * tx * (3 - 2 * tx);
    const uy = ty * ty * (3 - 2 * ty);
    const v00 = hashGrid(x0, y0, period, salt);
    const v10 = hashGrid(x0 + 1, y0, period, salt);
    const v01 = hashGrid(x0, y0 + 1, period, salt);
    const v11 = hashGrid(x0 + 1, y0 + 1, period, salt);
    const a = v00 + (v10 - v00) * ux;
    const b = v01 + (v11 - v01) * ux;
    return a + (b - a) * uy;
  };

  const fineHash = (ix, iy) => {
    let h = ((ix & MASK) * 2246822519 + (iy & MASK) * 3266489917 + 97) | 0;
    h = Math.imul(h ^ (h >>> 15), 668265263);
    return ((h ^ (h >>> 13)) >>> 0) / 4294967296;
  };

  for (let y = 0; y < SIZE; y++) {
    for (let x = 0; x < SIZE; x++) {
      const macro = smoothNoise(x, y, 64, 11) - 0.5;
      const meso = smoothNoise(x, y, 24, 29) - 0.5;
      const detail = smoothNoise(x, y, 8, 53) - 0.5;
      const loam = smoothNoise(x, y, 48, 71) - 0.5;
      const grain = (fineHash(x, y) - 0.5) * 7.5;

      const shade = macro * 15 + meso * 9 + detail * 5 + grain;
      const warmth = loam * 7;
      const pi = (y * SIZE + x) * 4;
      data[pi]     = Math.max(68, Math.min(138, Math.round(103 + shade + warmth)));
      data[pi + 1] = Math.max(76, Math.min(146, Math.round(112 + shade - warmth * 0.25)));
      data[pi + 2] = Math.max(58, Math.min(120, Math.round(85 + shade * 0.85 - warmth * 0.5)));
      data[pi + 3] = 255;
    }
  }
  ctx.putImageData(imageData, 0, 0);

  _naturalGroundTexture = new THREE.CanvasTexture(canvas);
  _naturalGroundTexture.wrapS = THREE.RepeatWrapping;
  _naturalGroundTexture.wrapT = THREE.RepeatWrapping;
  _naturalGroundTexture.repeat.set(220, 220);
  _naturalGroundTexture.anisotropy = 8;
  _naturalGroundTexture.colorSpace = THREE.SRGBColorSpace;
  return _naturalGroundTexture;
}

/* 合成小场景占位地面：仅用于 1~12 条边的微型合成演示场景。 */
function buildCityBlockTexture() {
  if (_cityBlockTexture || typeof document === 'undefined') return _cityBlockTexture;

  const CELL = 256;            // 每像素代表 200m/256 ≈ 0.78m
  const cells = 2;             // 纹理含 2×2 个 200m 街区
  const size = CELL * cells;
  const canvas = document.createElement('canvas');
  canvas.width = size; canvas.height = size;
  const ctx = canvas.getContext('2d');
  if (!ctx || typeof ctx.getImageData !== 'function') return null;

  // ── 基底：中性城市地块色（比纯草灰、淡）──
  ctx.fillStyle = '#6b7257';
  ctx.fillRect(0, 0, size, size);

  // ── 街区网格：每个 200m 街区填一块（轻微色差）──
  for (let cy = 0; cy < cells; cy++) {
    for (let cx = 0; cx < cells; cx++) {
      const seed = (cx * 73856093 ^ cy * 19349663) >>> 0;
      const hue = 60 + (seed % 12);      // 黄绿-棕绿
      const light = 38 + (seed % 10);    // 亮度微差
      ctx.fillStyle = `hsl(${hue}, ${26 + (seed % 8)}%, ${light}%)`;
      ctx.fillRect(cx * CELL, cy * CELL, CELL, CELL);
    }
  }

  // ── 街道带：每个街区边缘一条深色带（模拟道路俯视）──
  ctx.fillStyle = '#4a4d42';
  for (let i = 0; i <= cells; i++) {
    ctx.fillRect(0, i * CELL - 6, size, 12);   // 横街
    ctx.fillRect(i * CELL - 6, 0, 12, size);   // 竖街
  }

  // ── 建筑屋顶块：每个街区随机 4~9 个灰色矩形 ──
  for (let cy = 0; cy < cells; cy++) {
    for (let cx = 0; cx < cells; cx++) {
      const seed = (cx * 19349663 + cy * 73856093 + 17) >>> 0;
      const count = 5 + (seed % 5);
      let s = seed >>> 3;
      for (let b = 0; b < count; b++) {
        s = (s * 1664525 + 1013904223) >>> 0;
        const bw = 24 + (s % 90);
        s = (s * 1664525 + 1013904223) >>> 0;
        const bh = 20 + (s % 80);
        s = (s * 1664525 + 1013904223) >>> 0;
        const bx = cx * CELL + 12 + (s % (CELL - bw - 24));
        s = (s * 1664525 + 1013904223) >>> 0;
        const by = cy * CELL + 12 + (s % (CELL - bh - 24));
        s = (s * 1664525 + 1013904223) >>> 0;
        const roofLight = 45 + (s % 25);
        ctx.fillStyle = `hsl(210, ${10 + (s % 12)}%, ${roofLight}%)`;
        ctx.fillRect(bx, by, bw, bh);
        // 屋顶阴影边（东南）
        ctx.fillStyle = 'rgba(0,0,0,0.16)';
        ctx.fillRect(bx, by + bh, bw, 3);
        ctx.fillRect(bx + bw, by, 3, bh);
      }
    }
  }

  // ── 绿地点缀：每个街区随机 2~4 块绿地 ──
  for (let cy = 0; cy < cells; cy++) {
    for (let cx = 0; cx < cells; cx++) {
      const seed = (cx * 2246822519 + cy * 3266489917 + 31) >>> 0;
      let s = seed >>> 3;
      const greens = 2 + (s % 3);
      for (let g = 0; g < greens; g++) {
        s = (s * 1664525 + 1013904223) >>> 0;
        const gw = 30 + (s % 70);
        s = (s * 1664525 + 1013904223) >>> 0;
        const gh = 24 + (s % 60);
        s = (s * 1664525 + 1013904223) >>> 0;
        const gx = cx * CELL + 10 + (s % (CELL - gw - 20));
        s = (s * 1664525 + 1013904223) >>> 0;
        const gy = cy * CELL + 10 + (s % (CELL - gh - 20));
        s = (s * 1664525 + 1013904223) >>> 0;
        ctx.fillStyle = `hsl(${95 + (s % 25)}, ${30 + (s % 18)}%, ${34 + (s % 12)}%)`;
        ctx.fillRect(gx, gy, gw, gh);
      }
    }
  }

  // ── 全局轻微噪点，避免色块生硬（确定性 PRNG，消除场景切换闪烁）──
  const imageData = ctx.getImageData(0, 0, size, size);
  if (!imageData || !imageData.data || !imageData.data.length) return null;
  const data = imageData.data;
  let noiseSeed = 42;
  for (let i = 0; i < data.length; i += 4) {
    noiseSeed = (noiseSeed * 1664525 + 1013904223) >>> 0;
    const noise = ((noiseSeed >>> 16) / 65535 - 0.5) * 12;
    data[i] = Math.max(0, Math.min(255, data[i] + noise));
    data[i + 1] = Math.max(0, Math.min(255, data[i + 1] + noise));
    data[i + 2] = Math.max(0, Math.min(255, data[i + 2] + noise));
  }
  ctx.putImageData(imageData, 0, 0);

  _cityBlockTexture = new THREE.CanvasTexture(canvas);
  _cityBlockTexture.wrapS = THREE.RepeatWrapping;
  _cityBlockTexture.wrapT = THREE.RepeatWrapping;
  // 每 200m 一格：20000 / 200 = 100 格
  _cityBlockTexture.repeat.set(100, 100);
  _cityBlockTexture.colorSpace = THREE.SRGBColorSpace;
  return _cityBlockTexture;
}

export function createGroundView(scene) {
  let mesh = null;
  let currentAuthMode = null;

  function selectTexture(authMode) {
    return authMode ? buildNaturalGroundTexture() : buildCityBlockTexture();
  }

  function applyTextureForMode(authMode) {
    if (!mesh || !mesh.material) return;
    if (currentAuthMode === authMode) return;
    const tex = selectTexture(authMode);
    mesh.material.map = tex;
    if (!tex) mesh.material.color.setHex(GROUND_BASE);
    mesh.material.needsUpdate = true;
    currentAuthMode = authMode;
  }

  function build(size = 20000, roadNetwork = null) {
    if (roadNetwork && typeof roadNetwork === 'object') {
      _authoritativeMapMode = isAuthoritativeMap(roadNetwork);
    }
    if (mesh) {
      scene.remove(mesh);
      mesh.geometry.dispose();
      mesh.material.dispose();
      mesh = null;
    }
    if (size <= 0) return;
    const geo = new THREE.PlaneGeometry(size, size);
    const authMode = _authoritativeMapMode;
    const texture = selectTexture(authMode);
    const mat = new THREE.MeshStandardMaterial({
      color: texture ? 0xffffff : GROUND_BASE,
      map: texture,
      roughness: 1.0,
      polygonOffset: true,
      polygonOffsetFactor: 4,
      polygonOffsetUnits: 8,
    });
    mesh = new THREE.Mesh(geo, mat);
    mesh.rotation.x = -Math.PI / 2;
    mesh.position.y = -0.05;
    mesh.receiveShadow = true;
    mesh.frustumCulled = true;
    currentAuthMode = authMode;
    scene.add(mesh);
  }

  function update(store) {
    if (!store || !store.roadNetwork) return;
    const authMode = isAuthoritativeMap(store.roadNetwork);
    _authoritativeMapMode = authMode;
    applyTextureForMode(authMode);
  }

  function setAuthoritativeMode(flag) {
    const authMode = Boolean(flag);
    _authoritativeMapMode = authMode;
    applyTextureForMode(authMode);
  }

  function getMesh() { return mesh; }

  /* SR/BEV 科技风：深色冷底。real 写实风用原纹理；tech 时把材质整体乘一个
   * 深冷灰 tint（color 与 map 相乘），地面压暗变冷，标线/车灯更突出。 */
  function setTechMode(tech) {
    if (!mesh || !mesh.material) return;
    const mat = mesh.material;
    if (tech) {
      mat.color.setHex(SR_GROUND_TINT);   // 深蓝冷灰
      mat.needsUpdate = true;
    } else {
      mat.color.setHex(0xffffff);   // 还原为纯纹理色
      mat.needsUpdate = true;
    }
  }

  return { build, update, getMesh, setTechMode, setAuthoritativeMode };
}
