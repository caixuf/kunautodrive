/**
 * GroundView.js — 草地/地面
 * 纯色 #3e6b34（深饱和草绿），无 canvas 纹理。
 * 位置 y=-0.05（路面上方 0.10m，防 z-fight）。
 */

const GRASS_COLOR = 0x3e6b34;
let _grassTexture = null;

function buildGrassTexture() {
  if (_grassTexture || typeof document === 'undefined') return _grassTexture;

  const size = 256;
  const canvas = document.createElement('canvas');
  canvas.width = size;
  canvas.height = size;
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#3e6b34';
  ctx.fillRect(0, 0, size, size);

  // Deterministic low-frequency patches give the flat ground a usable
  // scale cue without adding geometry or a network asset dependency.
  for (let i = 0; i < 180; i++) {
    const seed = (i * 1664525 + 1013904223) >>> 0;
    const x = (seed & 0xff);
    const y = ((seed >>> 8) & 0xff);
    const radius = 3 + ((seed >>> 16) & 0x0f);
    const green = 46 + ((seed >>> 20) & 0x1f);
    ctx.fillStyle = `rgba(${35 + ((seed >>> 24) & 0x0f)},${green},${28 + ((seed >>> 12) & 0x0f)},0.22)`;
    ctx.beginPath();
    ctx.arc(x, y, radius, 0, Math.PI * 2);
    ctx.fill();
  }

  _grassTexture = new THREE.CanvasTexture(canvas);
  _grassTexture.wrapS = THREE.RepeatWrapping;
  _grassTexture.wrapT = THREE.RepeatWrapping;
  _grassTexture.repeat.set(64, 64);
  _grassTexture.colorSpace = THREE.SRGBColorSpace;
  return _grassTexture;
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
      const texture = buildGrassTexture();
      const mat = new THREE.MeshStandardMaterial({
        color: texture ? 0xffffff : GRASS_COLOR,
        map: texture,
        roughness: 0.95,
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
