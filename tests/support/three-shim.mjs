/**
 * three-shim.mjs — 最小假 THREE，供 Node.js 测试加载 vis/ 模块
 *
 * 用递归 Proxy 实现"任何属性访问/调用/构造都返回自身"的自动 mock，
 * 无需枚举所有 THREE API。vis/ 模块的 import * as THREE from 'three'
 * 和 window.THREE 都会被解析为此 shim。
 *
 * 设计要点：
 *  - valueOf / Symbol.toPrimitive 返回 0，使算术运算（rotation.x += 0.016）不抛错
 *  - 常见类型守卫属性（.isMesh / .isMaterial 等）返回 undefined（falsy）
 *  - .children 返回 []（空数组），.name 返回 ''（空串）
 *  - instanceof 检查（Symbol.hasInstance）返回 true，避免分支走错
 *  - Array.isArray() 检查返回 false（Proxy 不是数组）
 *
 * 用法：
 *   node --import ./tests/support/three-preload.mjs tests/vis_module_load.test.mjs
 */

// 递归 Proxy 处理器
const handler = {
  get(target, prop) {
    const state = (target.__state ||= Object.create(null));
    if (Object.prototype.hasOwnProperty.call(state, prop)) {
      return state[prop];
    }
    if (prop === '__state') return state;

    // ── 最小可观测位姿对象：支持 position/rotation/scale.x|y|z + set() ──
    // 让测试可以断言 mesh/group 的最终数值，而不仅是"不抛错"。
    if (prop === 'position' || prop === 'rotation' || prop === 'scale') {
      const vec = {
        x: 0, y: 0, z: 0,
        set(x = 0, y = 0, z = 0) { this.x = x; this.y = y; this.z = z; return this; },
        clone() { return { ...this, set: this.set, copy: this.copy, clone: this.clone, add: this.add, sub: this.sub, multiplyScalar: this.multiplyScalar, lerp: this.lerp, normalize: this.normalize, length: this.length }; },
        copy(other) {
          if (other && typeof other === 'object') {
            this.x = Number(other.x) || 0;
            this.y = Number(other.y) || 0;
            this.z = Number(other.z) || 0;
          }
          return this;
        },
        add(other) { if (other) { this.x += Number(other.x) || 0; this.y += Number(other.y) || 0; this.z += Number(other.z) || 0; } return this; },
        sub(other) { if (other) { this.x -= Number(other.x) || 0; this.y -= Number(other.y) || 0; this.z -= Number(other.z) || 0; } return this; },
        multiplyScalar(s = 1) { const n = Number(s) || 0; this.x *= n; this.y *= n; this.z *= n; return this; },
        lerp(other, alpha = 0) {
          const a = Number(alpha) || 0;
          if (other) {
            this.x += ((Number(other.x) || 0) - this.x) * a;
            this.y += ((Number(other.y) || 0) - this.y) * a;
            this.z += ((Number(other.z) || 0) - this.z) * a;
          }
          return this;
        },
        length() { return Math.sqrt(this.x * this.x + this.y * this.y + this.z * this.z); },
        normalize() {
          const len = this.length() || 1;
          this.x /= len; this.y /= len; this.z /= len;
          return this;
        },
      };
      state[prop] = vec;
      return vec;
    }

    // ── 类型守卫属性：返回 undefined（falsy），避免触发 isMesh 等分支 ──
    if (prop === 'isMesh' || prop === 'isMaterial' ||
        prop === 'isMeshPhysicalMaterial' || prop === 'isMeshStandardMaterial' ||
        prop === 'isMeshBasicMaterial' || prop === 'isMeshPhongMaterial' ||
        prop === 'isMeshToonMaterial' || prop === 'isSpriteMaterial' ||
        prop === 'isGroup' || prop === 'isObject3D' ||
        prop === 'isBufferGeometry' || prop === 'isInstancedMesh' ||
        prop === 'isSkinnedMesh' || prop === 'isBone' ||
        prop === 'isLine' || prop === 'isPoints' || prop === 'isSprite' ||
        prop === 'isCamera' || prop === 'isLight' || prop === 'isScene') {
      return undefined;
    }
    // ── 常见字符串属性：返回空串 ──
    if (prop === 'name' || prop === 'type' || prop === 'uuid') {
      return '';
    }
    // ── 常见数组属性：返回 Proxy 数组，索引访问返回新 Proxy ──
    if (prop === 'children') {
      return new Proxy([], {
        get(target, p) {
          if (p === 'length') return 0;
          if (p === Symbol.iterator) return function* () { /* empty */ };
          if (p === 'push' || p === 'pop' || p === 'shift' || p === 'unshift' ||
              p === 'splice' || p === 'slice' || p === 'concat' || p === 'filter' ||
              p === 'map' || p === 'forEach' || p === 'find' || p === 'findIndex' ||
              p === 'indexOf' || p === 'includes' || p === 'some' || p === 'every' ||
              p === 'reduce' || p === 'reduceRight' || p === 'sort' || p === 'reverse') {
            return () => createProxy();
          }
          return createProxy();
        }
      });
    }
    // ── 常见布尔属性 ──
    if (prop === 'visible' || prop === 'castShadow' || prop === 'receiveShadow' ||
        prop === 'frustumCulled' || prop === 'matrixAutoUpdate' ||
        prop === 'matrixWorldNeedsUpdate') {
      return true;
    }
    // ── 数值属性：返回 0 ──
    if (prop === 'length' || prop === 'id') {
      return 0;
    }
    // ── 特殊符号 ──
    if (prop === Symbol.toPrimitive) {
      return () => 0;
    }
    if (prop === 'valueOf') {
      return () => 0;
    }
    if (prop === 'toString') {
      return () => '[THREE proxy]';
    }
    if (prop === Symbol.hasInstance) {
      return () => true;
    }
    if (prop === Symbol.iterator) {
      return function* () { /* empty */ };
    }
    // ── 默认：返回新 Proxy ──
    return createProxy();
  },

  apply(target, thisArg, args) {
    return createProxy();
  },

  construct(target, args) {
    return createProxy();
  },

  set(target, prop, value) {
    const state = (target.__state ||= Object.create(null));
    state[prop] = value;
    return true; // 静默接受任何赋值
  },
};

function createProxy() {
  return new Proxy(function () { return createProxy(); }, handler);
}

// 根 THREE 对象（也是 Proxy）
const THREE = createProxy();

// 挂载全局 export
export default THREE;
export { THREE };

// 常用导出（具名导入兼容）
export const Scene = THREE;
export const Group = THREE;
export const Mesh = THREE;
export const Object3D = THREE;
export const Shape = THREE;
export const ExtrudeGeometry = THREE;
export const BoxGeometry = THREE;
export const PlaneGeometry = THREE;
export const CylinderGeometry = THREE;
export const SphereGeometry = THREE;
export const CircleGeometry = THREE;
export const ConeGeometry = THREE;
export const TubeGeometry = THREE;
export const TorusGeometry = THREE;
export const BufferGeometry = THREE;
export const InstancedMesh = THREE;
export const SkinnedMesh = THREE;
export const Bone = THREE;
export const Line = THREE;
export const LineBasicMaterial = THREE;
export const LineSegments = THREE;
export const Points = THREE;
export const PointsMaterial = THREE;
export const Sprite = THREE;
export const SpriteMaterial = THREE;

export const MeshStandardMaterial = THREE;
export const MeshPhysicalMaterial = THREE;
export const MeshBasicMaterial = THREE;
export const MeshPhongMaterial = THREE;
export const MeshToonMaterial = THREE;
export const MeshLambertMaterial = THREE;
export const ShaderMaterial = THREE;
export const RawShaderMaterial = THREE;

/* Vector3 需要真实数值字段：Curve.js 用 new THREE.Vector3(x,y,z) 建控制点，
 * CatmullRomCurve3 构造时 Number(p.x) 读坐标。若用 Proxy，.x 返回 Proxy
 * (valueOf→0) → 所有控制点坍缩到原点 → spine 全零 → 家具落路中间。
 * 这里给出较完整的 Vector3，覆盖 vis/ 测试 + three/addons (OrbitControls) 用到的
 * 全部方法。applyQuaternion/applyMatrix4 等涉及 Quaternion/Matrix4（仍是 Proxy）
 * 的方法做安全 no-op（返回 this），因为测试只验"不抛错"不验旋转正确性。 */
class _Vector3 {
  constructor(x = 0, y = 0, z = 0) { this.x = Number(x)||0; this.y = Number(y)||0; this.z = Number(z)||0; }
  set(x = 0, y = 0, z = 0) { this.x = Number(x)||0; this.y = Number(y)||0; this.z = Number(z)||0; return this; }
  setScalar(s = 0) { const n = Number(s)||0; this.x = this.y = this.z = n; return this; }
  setComponent(i, v) { const n = Number(v)||0; if (i===0) this.x=n; else if (i===1) this.y=n; else this.z=n; return this; }
  getComponent(i) { return i===0 ? this.x : (i===1 ? this.y : this.z); }
  copy(o) { if (o) { this.x = Number(o.x)||0; this.y = Number(o.y)||0; this.z = Number(o.z)||0; } return this; }
  clone() { return new _Vector3(this.x, this.y, this.z); }
  add(o) { if (o) { this.x += Number(o.x)||0; this.y += Number(o.y)||0; this.z += Number(o.z)||0; } return this; }
  addScalar(s = 0) { const n = Number(s)||0; this.x += n; this.y += n; this.z += n; return this; }
  addVectors(a, b) { this.x = (Number(a?.x)||0) + (Number(b?.x)||0); this.y = (Number(a?.y)||0) + (Number(b?.y)||0); this.z = (Number(a?.z)||0) + (Number(b?.z)||0); return this; }
  /* OrbitControls.update() 每帧 scope.object.position.addScaledVector(scope.target, ±1)
   * 做"相机相对 target 平移"，缺少该方法会让 CameraRig 集成测试直接抛
   * "scope.target.addScaledVector is not a function"（9413ccc 引入 zoomToCursor/
   * enableDamping 改动后 update 路径走到）。真实 three 实现同此。 */
  addScaledVector(v, s = 1) { const n = Number(s)||0; if (v) { this.x += (Number(v.x)||0) * n; this.y += (Number(v.y)||0) * n; this.z += (Number(v.z)||0) * n; } return this; }
  sub(o) { if (o) { this.x -= Number(o.x)||0; this.y -= Number(o.y)||0; this.z -= Number(o.z)||0; } return this; }
  subScalar(s = 0) { const n = Number(s)||0; this.x -= n; this.y -= n; this.z -= n; return this; }
  subVectors(a, b) { this.x = (Number(a?.x)||0) - (Number(b?.x)||0); this.y = (Number(a?.y)||0) - (Number(b?.y)||0); this.z = (Number(a?.z)||0) - (Number(b?.z)||0); return this; }
  multiplyScalar(s = 1) { const n = Number(s)||0; this.x *= n; this.y *= n; this.z *= n; return this; }
  multiplyVectors(a, b) { this.x = (Number(a?.x)||0) * (Number(b?.x)||0); this.y = (Number(a?.y)||0) * (Number(b?.y)||0); this.z = (Number(a?.z)||0) * (Number(b?.z)||0); return this; }
  divideScalar(s = 1) { const n = Number(s); if (n && n !== 0) { this.x /= n; this.y /= n; this.z /= n; } return this; }
  min(o) { if (o) { this.x = Math.min(this.x, Number(o.x)||0); this.y = Math.min(this.y, Number(o.y)||0); this.z = Math.min(this.z, Number(o.z)||0); } return this; }
  max(o) { if (o) { this.x = Math.max(this.x, Number(o.x)||0); this.y = Math.max(this.y, Number(o.y)||0); this.z = Math.max(this.z, Number(o.z)||0); } return this; }
  clamp(min, max) { if (min && max) { this.x = Math.max(Number(min.x)||0, Math.min(this.x, Number(max.x)||0)); this.y = Math.max(Number(min.y)||0, Math.min(this.y, Number(max.y)||0)); this.z = Math.max(Number(min.z)||0, Math.min(this.z, Number(max.z)||0)); } return this; }
  clampScalar(lo = 0, hi = 0) { this.x = Math.max(lo, Math.min(this.x, hi)); this.y = Math.max(lo, Math.min(this.y, hi)); this.z = Math.max(lo, Math.min(this.z, hi)); return this; }
  clampLength(minL = 0, maxL = 0) { const l = this.length(); if (l > 0) { const c = Math.max(minL, Math.min(l, maxL)) / l; this.x *= c; this.y *= c; this.z *= c; } return this; }
  floor() { this.x = Math.floor(this.x); this.y = Math.floor(this.y); this.z = Math.floor(this.z); return this; }
  ceil() { this.x = Math.ceil(this.x); this.y = Math.ceil(this.y); this.z = Math.ceil(this.z); return this; }
  round() { this.x = Math.round(this.x); this.y = Math.round(this.y); this.z = Math.round(this.z); return this; }
  negate() { this.x = -this.x; this.y = -this.y; this.z = -this.z; return this; }
  dot(o) { return this.x * (Number(o?.x)||0) + this.y * (Number(o?.y)||0) + this.z * (Number(o?.z)||0); }
  cross(o) { const x = this.x, y = this.y, z = this.z; this.x = y * (Number(o?.z)||0) - z * (Number(o?.y)||0); this.y = z * (Number(o?.x)||0) - x * (Number(o?.z)||0); this.z = x * (Number(o?.y)||0) - y * (Number(o?.x)||0); return this; }
  crossVectors(a, b) { this.x = (Number(a?.y)||0) * (Number(b?.z)||0) - (Number(a?.z)||0) * (Number(b?.y)||0); this.y = (Number(a?.z)||0) * (Number(b?.x)||0) - (Number(a?.x)||0) * (Number(b?.z)||0); this.z = (Number(a?.x)||0) * (Number(b?.y)||0) - (Number(a?.y)||0) * (Number(b?.x)||0); return this; }
  length() { return Math.sqrt(this.x*this.x + this.y*this.y + this.z*this.z); }
  lengthSq() { return this.x*this.x + this.y*this.y + this.z*this.z; }
  manhattanLength() { return Math.abs(this.x) + Math.abs(this.y) + Math.abs(this.z); }
  normalize() { const l = this.length() || 1; this.x /= l; this.y /= l; this.z /= l; return this; }
  setLength(l = 0) { const cur = this.length(); if (cur > 0) this.multiplyScalar(l / cur); return this; }
  lerp(o, a = 0) { const t = Number(a)||0; if (o) { this.x += (Number(o.x)-this.x)*t; this.y += (Number(o.y)-this.y)*t; this.z += (Number(o.z)-this.z)*t; } return this; }
  lerpVectors(a, b, t = 0) { this.x = (Number(a?.x)||0) + ((Number(b?.x)||0) - (Number(a?.x)||0)) * t; this.y = (Number(a?.y)||0) + ((Number(b?.y)||0) - (Number(a?.y)||0)) * t; this.z = (Number(a?.z)||0) + ((Number(b?.z)||0) - (Number(a?.z)||0)) * t; return this; }
  distanceTo(o) { return Math.sqrt(this.distanceToSquared(o)); }
  distanceToSquared(o) { const dx = this.x - (Number(o?.x)||0), dy = this.y - (Number(o?.y)||0), dz = this.z - (Number(o?.z)||0); return dx*dx + dy*dy + dz*dz; }
  manhattanDistanceTo(o) { return Math.abs(this.x - (Number(o?.x)||0)) + Math.abs(this.y - (Number(o?.y)||0)) + Math.abs(this.z - (Number(o?.z)||0)); }
  angleTo(o) { const d = this.dot(o) / (this.length() * (Math.sqrt((Number(o?.x)||0)**2 + (Number(o?.y)||0)**2 + (Number(o?.z)||0)**2) || 1)); return Math.acos(Math.max(-1, Math.min(1, d))); }
  fromArray(a, off = 0) { this.x = Number(a?.[off])||0; this.y = Number(a?.[off+1])||0; this.z = Number(a?.[off+2])||0; return this; }
  toArray(arr = [], off = 0) { arr[off] = this.x; arr[off+1] = this.y; arr[off+2] = this.z; return arr; }
  /* 以下方法涉及 Quaternion/Matrix4/Euler（仍是 Proxy），安全 no-op 返回 this */
  applyQuaternion(_q) { return this; }
  applyAxisAngle(_axis, _angle) { return this; }
  applyEuler(_euler) { return this; }
  applyMatrix4(_m) { return this; }
  applyMatrix3(_m) { return this; }
  project(_camera) { return this; }
  unproject(_camera) { return this; }
  setFromSpherical(_s) { return this; }
  setFromMatrixPosition(_m) { return this; }
  setFromMatrixColumn(_m, _i) { return this; }
  setFromMatrixScale(_m) { return this; }
  transformDirection(_m) { return this; }
}
export const Vector2 = THREE;
export const Vector3 = _Vector3;
export const Vector4 = THREE;
export const Euler = THREE;
export const Quaternion = THREE;
export const Matrix4 = THREE;
export const Matrix3 = THREE;
export const Color = THREE;
export const Box3 = THREE;
export const Sphere = THREE;
export const Raycaster = THREE;
export const Plane = THREE;
export const Frustum = THREE;

/* CatmullRomCurve3 需要真实实现：Curve.js 的 sampleCatmullRom 用它采样弯道
 * spine 点。若用递归 Proxy，getPoint() 返回 Proxy（valueOf→0）→ 所有 spine
 * 点坍缩到 (0,0,0)、法线 (0,0) → 路灯/树落位全到原点 = 路中间。
 * 这里给出最小可用的 centripetal Catmull-Rom，仅覆盖 vis/ 测试用到的
 * getPoint(t) / getPointAt(t) / getPoints(n) 三个 API。 */
class _CatmullRomCurve3 {
  constructor(points = [], closed = false) {
    this.points = points.map(p => ({
      x: Number(p?.x) || 0, y: Number(p?.y) || 0, z: Number(p?.z) || 0,
    }));
    this.closed = !!closed;
  }
  _get(i) {
    const n = this.points.length;
    if (this.closed) return this.points[((i % n) + n) % n];
    if (i < 0) return this.points[0];
    if (i >= n) return this.points[n - 1];
    return this.points[i];
  }
  getPoint(t) {
    const pts = this.points;
    if (pts.length < 2) return { x: 0, y: 0, z: 0 };
    if (pts.length === 2) {
      const a = pts[0], b = pts[1];
      return { x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t, z: a.z + (b.z - a.z) * t };
    }
    const tt = Math.max(0, Math.min(1, t)) * (pts.length - 1);
    const i = Math.floor(tt);
    const f = tt - i;
    const p0 = this._get(i - 1), p1 = this._get(i), p2 = this._get(i + 1), p3 = this._get(i + 2);
    const f2 = f * f, f3 = f2 * f;
    return {
      x: 0.5 * ((2 * p1.x) + (-p0.x + p2.x) * f + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * f2 + (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * f3),
      y: 0.5 * ((2 * p1.y) + (-p0.y + p2.y) * f + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * f2 + (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * f3),
      z: 0.5 * ((2 * p1.z) + (-p0.z + p2.z) * f + (2 * p0.z - 5 * p1.z + 4 * p2.z - p3.z) * f2 + (-p0.z + 3 * p1.z - 3 * p2.z + p3.z) * f3),
    };
  }
  getPointAt(t) { return this.getPoint(t); }
  getPoints(n) { const out = []; for (let i = 0; i <= n; i++) out.push(this.getPoint(i / n)); return out; }
}
export const CatmullRomCurve3 = _CatmullRomCurve3;
export const CubicBezierCurve3 = THREE;
export const QuadraticBezierCurve3 = THREE;
export const LineCurve3 = THREE;
export const SplineCurve = THREE;

export const BufferAttribute = THREE;
export const Float32BufferAttribute = THREE;
export const Uint16BufferAttribute = THREE;
export const Uint32BufferAttribute = THREE;

export const Camera = THREE;
export const PerspectiveCamera = THREE;
export const OrthographicCamera = THREE;
export const ArrayCamera = THREE;

export const AmbientLight = THREE;
export const DirectionalLight = THREE;
export const PointLight = THREE;
export const SpotLight = THREE;
export const HemisphereLight = THREE;
export const RectAreaLight = THREE;
export const Light = THREE;
export const LightShadow = THREE;
export const DirectionalLightShadow = THREE;
export const SpotLightShadow = THREE;
export const PointLightShadow = THREE;
export const CameraHelper = THREE;

export const WebGLRenderer = THREE;
export const WebGL1Renderer = THREE;
export const WebGLRenderTarget = THREE;
export const WebGLCubeRenderTarget = THREE;

export const PMREMGenerator = THREE;
export const TextureLoader = THREE;
export const CubeTextureLoader = THREE;
export const ImageBitmapLoader = THREE;
export const FileLoader = THREE;
export const LoadingManager = THREE;
export const GLTFLoader = THREE;
export const RGBELoader = THREE;
export const DRACOLoader = THREE;
export const KTX2Loader = THREE;
export const EXRLoader = THREE;
export const OBJLoader = THREE;
export const MTLLoader = THREE;
export const FBXLoader = THREE;

export const Texture = THREE;
export const DataTexture = THREE;
export const CanvasTexture = THREE;
export const CompressedTexture = THREE;
export const CubeTexture = THREE;
export const VideoTexture = THREE;
export const DepthTexture = THREE;

export const Clock = THREE;
export const MathUtils = THREE;
export const AnimationClip = THREE;
export const AnimationMixer = THREE;
export const AnimationAction = THREE;
export const KeyframeTrack = THREE;
export const PropertyBinding = THREE;
export const AnimationUtils = THREE;

export const AxesHelper = THREE;
export const GridHelper = THREE;
export const Box3Helper = THREE;
export const BoxHelper = THREE;
export const SkeletonHelper = THREE;
export const ArrowHelper = THREE;
export const PolarGridHelper = THREE;
export const VertexNormalsHelper = THREE;
export const FaceNormalsHelper = THREE;

// ── D-2: OrbitControls 依赖的具名导出 ──
// OrbitControls.js (vendor) `import { EventDispatcher, MOUSE, Spherical, TOUCH, Ray } from 'three'`。
// 这些不能用 Proxy：class OrbitControls extends EventDispatcher 需要真实 constructor，
// `switch (event.button) { case MOUSE.ROTATE }` 需要真实数值常量。
// 因此 EventDispatcher / Spherical / Ray 用最小空类，MOUSE / TOUCH 用与 three.module.js
// 一致的数值常量对象。
class _EventDispatcher {
  addEventListener() { return this; }
  hasEventListener() { return false; }
  removeEventListener() { return this; }
  dispatchEvent() { return this; }
}
class _Spherical {
  constructor(radius = 1, phi = 0, theta = 0) {
    this.radius = radius; this.phi = phi; this.theta = theta;
  }
  set(radius, phi, theta) { this.radius = radius; this.phi = phi; this.theta = theta; return this; }
  copy(other) { this.radius = other.radius; this.phi = other.phi; this.theta = other.theta; return this; }
  setFromVector3() { return this; }
  setFromCamera() { return this; }
  makeSafe() { return this; }
  clone() { return new _Spherical(this.radius, this.phi, this.theta); }
}
class _Ray {
  constructor(origin, direction) { this.origin = origin; this.direction = direction; }
  set(origin, direction) { this.origin = origin; this.direction = direction; return this; }
  at(t, target) { return target; }
  intersectPlane() { return null; }
}
export const EventDispatcher = _EventDispatcher;
export const Spherical = _Spherical;
export const Ray = _Ray;
// 与 three.module.js r160 中的常量保持一致
export const MOUSE = { LEFT: 0, MIDDLE: 1, RIGHT: 2, ROTATE: 0, DOLLY: 1, PAN: 2 };
export const TOUCH = { ROTATE: 0, PAN: 1, DOLLY_PAN: 2, DOLLY_ROTATE: 3 };

// 常量导出
export const DoubleSide = 2;
export const FrontSide = 0;
export const BackSide = 1;
export const SRGBColorSpace = 'srgb';
export const LinearSRGBColorSpace = 'srgb-linear';
export const NoColorSpace = '';
export const DisplayP3ColorSpace = 'display-p3';

export const AdditiveBlending = 2;
export const NormalBlending = 1;
export const SubtractiveBlending = 3;
export const MultiplyBlending = 4;
export const NoBlending = 0;
export const CustomBlending = 5;

export const UnsignedByteType = 1009;
export const FloatType = 1015;
export const HalfFloatType = 1016;
export const UnsignedShortType = 1011;
export const UnsignedIntType = 1013;

export const LinearFilter = 1006;
export const NearestFilter = 1003;
export const LinearMipmapLinearFilter = 1008;
export const NearestMipmapLinearFilter = 1007;
export const LinearMipmapNearestFilter = 1009;
export const NearestMipmapNearestFilter = 1004;

export const ClampToEdgeWrapping = 1001;
export const RepeatWrapping = 1000;
export const MirroredRepeatWrapping = 1002;

export const EquirectangularReflectionMapping = 1;
export const CubeReflectionMapping = 2;
export const CubeRefractionMapping = 3;
export const UVMapping = 300;

export const PCFSoftShadowMap = 2;
export const PCFShadowMap = 1;
export const BasicShadowMap = 0;

export const LoopOnce = 2200;
export const LoopRepeat = 2201;
export const LoopPingPong = 2202;

export const AlwaysStencilFunc = 519;
export const EqualStencilFunc = 514;
export const NotEqualStencilFunc = 517;
export const ReplaceStencilOp = 7687;

export const StaticDrawUsage = 35044;
export const DynamicDrawUsage = 35048;
export const StreamDrawUsage = 35040;

export const RGBAFormat = 1023;
export const RGBFormat = 1022;
export const DepthFormat = 1026;
export const RGBA16F = 1028;

export const NeverDepth = 512;
export const AlwaysDepth = 513;
export const LessDepth = 515;
export const LessEqualDepth = 516;
export const EqualDepth = 514;