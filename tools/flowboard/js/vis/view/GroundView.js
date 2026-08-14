/**
 * GroundView.js — 地面（城市街区卫星图占位）
 *
 * 程序化生成"俯视城市"质感的地面纹理（占位卫星图素材）：
 *   - 不再是纯草地，而是街区网格 + 建筑屋顶色块 + 绿地点缀
 *   - 未来接入真实卫星图（OSM 拉取瓦片）时只需替换 _groundTexture 的
 *     生成来源，其余构建逻辑不变
 * 位置 y=-0.05（路面上方 0.10m，防 z-fight）。
 */

const GROUND_BASE = 0x6b7257;  // 城市地块基底（偏土绿，比纯草更中性）
let _groundTexture = null;

/* 程序化卫星图质感：模拟 OSM 风格俯视——深色街道带 + 灰色建筑块 +
 * 绿色公园/草坪 + 少量树冠点。全部 Canvas 绘制，零外部资产。
 * repeat 按 1 格=200m 路网尺度平铺，与 city_grid 网格对齐。 */
function buildGroundTexture() {
  if (_groundTexture || typeof document === 'undefined') return _groundTexture;

  const CELL = 256;            // 每像素代表 200m/256 ≈ 0.78m
  const cells = 2;             // 纹理含 2×2 个 200m 街区
  const size = CELL * cells;
  const canvas = document.createElement('canvas');
  canvas.width = size; canvas.height = size;
  const ctx = canvas.getContext('2d');

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

  // ── 绿地点缀：每个街区随机 2~4 块绿地 + 树冠点 ──
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

  // ── 全局轻微噪点，避免色块生硬 ──
  const imageData = ctx.getImageData(0, 0, size, size);
  const data = imageData.data;
  for (let i = 0; i < data.length; i += 4) {
    const noise = (Math.random() - 0.5) * 12;
    data[i] = Math.max(0, Math.min(255, data[i] + noise));
    data[i + 1] = Math.max(0, Math.min(255, data[i + 1] + noise));
    data[i + 2] = Math.max(0, Math.min(255, data[i + 2] + noise));
  }
  ctx.putImageData(imageData, 0, 0);

  _groundTexture = new THREE.CanvasTexture(canvas);
  _groundTexture.wrapS = THREE.RepeatWrapping;
  _groundTexture.wrapT = THREE.RepeatWrapping;
  // 每 200m 一格：20000 / 200 = 100 格
  _groundTexture.repeat.set(100, 100);
  _groundTexture.colorSpace = THREE.SRGBColorSpace;
  return _groundTexture;
}

export function createGroundView(scene) {
  let mesh = null;

  function build(size = 20000) {
      if (mesh) {
        scene.remove(mesh);
        mesh.geometry.dispose();
        mesh.material.dispose();
        mesh = null;
      }
      if (size <= 0) return;
      const geo = new THREE.PlaneGeometry(size, size);
      const texture = buildGroundTexture();
      const mat = new THREE.MeshStandardMaterial({
        color: texture ? 0xffffff : GROUND_BASE,
        map: texture,
        roughness: 1.0,
      });
      mesh = new THREE.Mesh(geo, mat);
      mesh.rotation.x = -Math.PI / 2;
      mesh.position.y = -0.05;
      mesh.receiveShadow = true;
      mesh.frustumCulled = true;
      scene.add(mesh);
    }

  function getMesh() { return mesh; }

  return { build, getMesh };
}
