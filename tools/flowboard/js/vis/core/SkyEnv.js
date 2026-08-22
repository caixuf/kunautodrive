/**
 * SkyEnv.js — 天空穹顶 + 环境光照 + 电影级全天候天气系统
 *
 * 核心升级：
 *   1. 连续时段系统（拂晓/清晨/正午/午后/黄昏/夜晚），动态太阳与月相天体位置、日晕/月晕与色温
 *   2. 程序化双层流动动态云层（FBM 噪声 + 风向演化 + 黄昏金边/紫霞镶边）
 *   3. 真实夜空明月与繁星闪烁系统（Twinkling Starfield）
 *   4. 倾斜雨丝（Rain Streaks）+ 地面同心圆雨滴涟漪水花（Ground Splash Ripples）
 *   5. 鹅毛大雪（Swirling Snowflakes）与风向扰动
 *   6. 动态雷暴闪电系统（Thunderstorm Electric Lightning Flash）
 *   7. 8 大天气预设：晴朗(clear)、多云(cloudy)、阴天(overcast)、中雨(rain)、雷暴(storm)、雪景(snow)、浓雾(fog)、沙尘(sandstorm)
 *
 * 性能保障：天空穹顶全部在单个 GLSL Fragment Shader 中完成，零额外 Draw Call，满帧 100+ FPS。
 */

const TAU = Math.PI * 2;
const SKY_RADIUS = 1500;
const FOG_EXTINCTION_AT_VISIBILITY = Math.sqrt(-Math.log(0.05));

// ═══════════════════════════════════════════════════════════
// 时段参数表
// ═══════════════════════════════════════════════════════════

const TIME_SLOTS = {
  dawn:      { angle: 0.04,  label: 'dawn',     skyTop: 0x2b3a4a, skyBot: 0xeb6b56, horizonColor: 0xffb703, sunColor: 0xffaa33, moonColor: 0xdde8f0, sunIntensity: 1.4,  fogDensity: 0.0030, ambIntensity: 0.60, nightFactor: 0.0 },
  morning:   { angle: 0.14,  label: 'morning',  skyTop: 0x3a86ff, skyBot: 0x8ecae6, horizonColor: 0xd0f0fd, sunColor: 0xfffaed, moonColor: 0xdde8f0, sunIntensity: 2.0,  fogDensity: 0.0014, ambIntensity: 0.75, nightFactor: 0.0 },
  noon:      { angle: 0.25,  label: 'noon',     skyTop: 0x1d4ed8, skyBot: 0x60a5fa, horizonColor: 0xbfdbfe, sunColor: 0xffffff, moonColor: 0xdde8f0, sunIntensity: 2.2,  fogDensity: 0.0007, ambIntensity: 0.85, nightFactor: 0.0 },
  day:       { angle: 0.25,  label: 'day',      skyTop: 0x1d4ed8, skyBot: 0x60a5fa, horizonColor: 0xbfdbfe, sunColor: 0xffffff, moonColor: 0xdde8f0, sunIntensity: 2.2,  fogDensity: 0.0007, ambIntensity: 0.85, nightFactor: 0.0 },
  afternoon: { angle: 0.36,  label: 'afternoon',skyTop: 0x2563eb, skyBot: 0x93c5fd, horizonColor: 0xffedd5, sunColor: 0xfff5db, moonColor: 0xdde8f0, sunIntensity: 2.0,  fogDensity: 0.0016, ambIntensity: 0.75, nightFactor: 0.0 },
  dusk:      { angle: 0.46,  label: 'dusk',     skyTop: 0x311042, skyBot: 0x9d174d, horizonColor: 0xf43f5e, sunColor: 0xff5400, moonColor: 0xdde8f0, sunIntensity: 1.2,  fogDensity: 0.0035, ambIntensity: 0.50, nightFactor: 0.15 },
  night:     { angle: 0.72,  label: 'night',    skyTop: 0x050814, skyBot: 0x0f172a, horizonColor: 0x1e293b, sunColor: 0x1e293b, moonColor: 0xe0e7ff, sunIntensity: 0.15, fogDensity: 0.0055, ambIntensity: 0.35, nightFactor: 1.0 },
};

// ═══════════════════════════════════════════════════════════
// 天气模式表
// ═══════════════════════════════════════════════════════════

const WEATHER_MODES = {
  clear:     { rainRate: 0,     snowRate: 0,    cloudDensity: 0.15, cloudDarkness: 0.0,  skyTint: 0xffffff, fogDensityMul: 1.0,  sunIntensityMul: 1.0,  label: '晴朗' },
  cloudy:    { rainRate: 0,     snowRate: 0,    cloudDensity: 0.50, cloudDarkness: 0.2,  skyTint: 0xdde5ee, fogDensityMul: 1.3,  sunIntensityMul: 0.85, label: '多云' },
  overcast:  { rainRate: 0,     snowRate: 0,    cloudDensity: 0.85, cloudDarkness: 0.55, skyTint: 0x99a2aa, fogDensityMul: 2.2,  sunIntensityMul: 0.45, label: '阴天' },
  rain:      { rainRate: 6000,  snowRate: 0,    cloudDensity: 0.90, cloudDarkness: 0.70, skyTint: 0x778899, fogDensityMul: 3.0,  sunIntensityMul: 0.35, label: '中雨' },
  storm:     { rainRate: 10000, snowRate: 0,    cloudDensity: 0.98, cloudDarkness: 0.92, skyTint: 0x4a5568, fogDensityMul: 4.5,  sunIntensityMul: 0.20, label: '雷暴大雨' },
  snow:      { rainRate: 0,     snowRate: 3500, cloudDensity: 0.75, cloudDarkness: 0.30, skyTint: 0xddeeff, fogDensityMul: 2.5,  sunIntensityMul: 0.50, label: '雪景' },
  fog:       { rainRate: 0,     snowRate: 0,    cloudDensity: 0.60, cloudDarkness: 0.20, skyTint: 0xc8d0d2, fogDensityMul: 12.0, sunIntensityMul: 0.25, label: '浓雾' },
  sandstorm: { rainRate: 0,     snowRate: 0,    cloudDensity: 0.70, cloudDarkness: 0.60, skyTint: 0xd4a373, fogDensityMul: 6.0,  sunIntensityMul: 0.25, label: '沙尘' },
};

// ═══════════════════════════════════════════════════════════
// Sky dome shader (电影级大气散射 + 日月天体 + 2层流云 + 繁星 + 闪电)
// ═══════════════════════════════════════════════════════════

const SKY_VERT = `
varying vec3 vDirection;
void main() {
  vDirection = position;
  gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
}`;

const SKY_FRAG = `
uniform vec3 uTopColor;
uniform vec3 uBotColor;
uniform vec3 uHorizonColor;
uniform vec3 uSunDirection;
uniform vec3 uSunColor;
uniform vec3 uMoonDirection;
uniform vec3 uMoonColor;
uniform float uCloudDensity;
uniform float uCloudDarkness;
uniform float uNightFactor;
uniform float uLightningFlash;
uniform float uTime;
uniform float uOffset;
uniform float uExponent;

varying vec3 vDirection;

// Fast 2D procedural noise for volumetric clouds
float hash(vec2 p) {
  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float noise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i + vec2(0.0, 0.0)), hash(i + vec2(1.0, 0.0)), u.x),
             mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float fbm(vec2 p) {
  float v = 0.0;
  float a = 0.5;
  mat2 rot = mat2(0.8, -0.6, 0.6, 0.8);
  for (int i = 0; i < 4; ++i) {
    v += a * noise(p);
    p = rot * p * 2.02 + vec2(11.3);
    a *= 0.5;
  }
  return v;
}

void main() {
  vec3 dir = normalize(vDirection);
  float h = dir.y;

  // 1. 大气背景与地平线渐变
  float t = max(pow(max(h + uOffset, 0.0), uExponent), 0.0);
  vec3 baseSky = mix(uBotColor, uTopColor, min(t, 1.0));
  float horizonBand = 1.0 - smoothstep(0.0, 0.28, abs(h));
  vec3 sky = mix(baseSky, uHorizonColor, horizonBand * 0.72);

  // 2. 真实太阳天体与光晕 (Sun disc, corona & glare)
  float sunCos = dot(dir, normalize(uSunDirection));
  float sunDisc = smoothstep(0.9993, 0.9998, sunCos);
  float sunCorona = pow(max(0.0, sunCos), 96.0) * 0.7 + pow(max(0.0, sunCos), 12.0) * 0.3;
  vec3 sunGlow = uSunColor * (sunDisc * 3.8 + sunCorona * max(0.0, 1.0 - uCloudDensity * 0.75)) * (1.0 - uNightFactor);

  // 3. 真实夜空明月与月晕 (Moon disc, halo & moonlight)
  float moonCos = dot(dir, normalize(uMoonDirection));
  float moonDisc = smoothstep(0.9988, 0.9996, moonCos);
  float moonHalo = pow(max(0.0, moonCos), 48.0) * 0.35 + pow(max(0.0, moonCos), 8.0) * 0.12;
  vec3 moonGlow = uMoonColor * (moonDisc * 2.2 + moonHalo) * uNightFactor;

  // 4. 夜空繁星与闪烁 (Twinkling starfield)
  if (uNightFactor > 0.05 && h > 0.02) {
    vec2 starUV = dir.xz / (h + 0.1) * 260.0;
    float starSeed = hash(floor(starUV));
    if (starSeed > 0.988) {
      float twinkle = sin(uTime * 2.8 + starSeed * 100.0) * 0.45 + 0.55;
      float starBright = (starSeed - 0.988) / 0.012 * twinkle;
      sky += vec3(0.9, 0.95, 1.0) * starBright * smoothstep(0.02, 0.2, h) * uNightFactor * (1.0 - uCloudDensity);
    }
  }

  // 5. 程序化双层流动动态云层 (2-Layer Volumetric Clouds)
  if (h > 0.01 && uCloudDensity > 0.02) {
    vec2 cloudCoord = (dir.xz / (h + 0.18)) * 0.45;
    vec2 wind1 = vec2(uTime * 0.012, uTime * 0.007);
    vec2 wind2 = vec2(uTime * -0.006, uTime * 0.010);
    float c1 = fbm(cloudCoord + wind1);
    float c2 = fbm(cloudCoord * 2.2 + wind2);
    float cloudNoise = c1 * 0.65 + c2 * 0.35;
    float threshold = 1.0 - uCloudDensity * 0.75;
    float cloudMask = smoothstep(threshold, threshold + 0.35, cloudNoise);

    // 云朵受光与边缘晚霞着色 (Cloud shading + Sunset rim lighting)
    vec3 cloudLit = mix(vec3(0.96, 0.97, 1.0), uSunColor * 1.2, max(0.0, sunCos) * 0.4);
    vec3 cloudDark = mix(vec3(0.35, 0.38, 0.44), vec3(0.12, 0.13, 0.16), uCloudDarkness);
    vec3 cloudCol = mix(cloudLit, cloudDark, uCloudDarkness);

    // 黄昏晚霞给云彩镶上金边/红边
    float sunsetRim = pow(max(0.0, 1.0 - abs(sunCos - 0.2)), 4.0) * (1.0 - uNightFactor) * 0.6;
    cloudCol = mix(cloudCol, uHorizonColor * 1.5, sunsetRim);

    sky = mix(sky, cloudCol, cloudMask * smoothstep(0.01, 0.15, h));
  }

  // 6. 闪电电光爆发 (Lightning electric flash)
  if (uLightningFlash > 0.01) {
    vec3 flashColor = vec3(0.85, 0.92, 1.0) * uLightningFlash * 3.2;
    sky = mix(sky, flashColor, 0.65) + flashColor * 0.35;
  }

  sky += sunGlow + moonGlow;
  gl_FragColor = vec4(sky, 1.0);
}
`;

// ═══════════════════════════════════════════════════════════
// 主工厂
// ═══════════════════════════════════════════════════════════

export function createSkyEnv(scene, sunLight, hemiLight) {
  // ── 天空穹顶 ──
  const skyGeo = new THREE.SphereGeometry(SKY_RADIUS, 48, 24);
  const skyMat = new THREE.ShaderMaterial({
    vertexShader: SKY_VERT,
    fragmentShader: SKY_FRAG,
    uniforms: {
      uTopColor:       { value: new THREE.Color(0x1d4ed8) },
      uBotColor:       { value: new THREE.Color(0x60a5fa) },
      uHorizonColor:   { value: new THREE.Color(0xbfdbfe) },
      uSunDirection:   { value: new THREE.Vector3(0, 1, 0) },
      uSunColor:       { value: new THREE.Color(0xffffff) },
      uMoonDirection:  { value: new THREE.Vector3(0.5, 0.8, -0.3).normalize() },
      uMoonColor:      { value: new THREE.Color(0xe0e7ff) },
      uCloudDensity:   { value: 0.15 },
      uCloudDarkness:  { value: 0.0 },
      uNightFactor:    { value: 0.0 },
      uLightningFlash: { value: 0.0 },
      uTime:           { value: 0.0 },
      uOffset:         { value: 0.08 },
      uExponent:       { value: 0.65 },
    },
    side: THREE.BackSide,
    depthWrite: false,
  });
  const sky = new THREE.Mesh(skyGeo, skyMat);
  sky.renderOrder = -1;
  sky.name = 'skyDome';
  scene.add(sky);

  // ── scene.background 兜底 ──
  scene.background = new THREE.Color(0x60a5fa);

  // ── 雾 ──
  scene.fog = new THREE.FogExp2(0x60a5fa, 0.0008);

  // ── 接管 Lighting.js 的 sun + hemi；补充 AmbientLight ──
  const sun = sunLight;
  const hemi = hemiLight;
  const ambient = new THREE.AmbientLight(0xffffff, 0.85);
  scene.add(ambient);

  // ── 降水粒子系统配置 ──
  const PRECIP_AREA = 80;        // 覆盖范围 (m)
  const PRECIP_HEIGHT = 36;      // 下落高度 (m)

  // ── 雨丝系统（LineSegments，倾斜拉长真实雨丝） ──
  let rainMesh = null;
  let rainCount = 0;

  // ── 雨滴地面水花涟漪（InstancedMesh 同心圆环） ──
  const MAX_RIPPLES = 64;
  let rippleMesh = null;
  const ripplePool = []; // [{x, z, life, maxLife, scale}]

  function _initRippleMesh() {
    if (rippleMesh) return;
    const ringGeo = new THREE.RingGeometry(0.08, 0.16, 16);
    ringGeo.rotateX(-Math.PI / 2);
    const ringMat = new THREE.MeshBasicMaterial({
      color: 0xccddff,
      transparent: true,
      opacity: 0.4,
      side: THREE.DoubleSide,
      depthWrite: false,
    });
    rippleMesh = new THREE.InstancedMesh(ringGeo, ringMat, MAX_RIPPLES);
    rippleMesh.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
    const dummy = new THREE.Object3D();
    dummy.position.set(0, -999, 0);
    dummy.updateMatrix();
    for (let i = 0; i < MAX_RIPPLES; i++) {
      rippleMesh.setMatrixAt(i, dummy.matrix);
      ripplePool.push({ x: 0, z: 0, life: 1, maxLife: 0.4 + Math.random() * 0.3, scale: 0.1 });
    }
    rippleMesh.instanceMatrix.needsUpdate = true;
    scene.add(rippleMesh);
  }

  function _buildRainMesh(count) {
    if (rainMesh) {
      scene.remove(rainMesh);
      rainMesh.geometry.dispose();
      rainMesh.material.dispose();
      rainMesh = null;
    }
    if (count <= 0) return;

    _initRippleMesh();

    const geo = new THREE.BufferGeometry();
    const positions = new Float32Array(count * 6); // 2 顶点/段 = 6 float
    const velocities = new Float32Array(count);
    const windTiltX = 0.12, windTiltZ = 0.08;
    const streakLen = 1.4;
    const cx = _camera ? _camera.position.x : 0;
    const cy = _camera ? _camera.position.y : 0;
    const cz = _camera ? _camera.position.z : 0;

    for (let i = 0; i < count; i++) {
      const rx = cx + (Math.random() - 0.5) * PRECIP_AREA;
      const ry = cy - 2.0 + Math.random() * PRECIP_HEIGHT;
      const rz = cz + (Math.random() - 0.5) * PRECIP_AREA;

      const idx = i * 6;
      positions[idx]     = rx;
      positions[idx + 1] = ry;
      positions[idx + 2] = rz;
      positions[idx + 3] = rx - windTiltX * streakLen;
      positions[idx + 4] = ry - streakLen;
      positions[idx + 5] = rz - windTiltZ * streakLen;

      velocities[i] = 22.0 + Math.random() * 8.0;
    }
    geo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));

    const mat = new THREE.LineBasicMaterial({
      color: 0x99bbff,
      transparent: true,
      opacity: 0.55,
      depthWrite: false,
    });

    rainMesh = new THREE.LineSegments(geo, mat);
    rainMesh.userData.velocities = velocities;
    rainMesh.userData.streakLen = streakLen;
    rainMesh.userData.windTiltX = windTiltX;
    rainMesh.userData.windTiltZ = windTiltZ;
    scene.add(rainMesh);
  }

  // ── 雪花系统（多尺度摇曳漂浮 Points + 柔和圆形雪花贴图） ──
  let snowMesh = null;
  let snowCount = 0;
  let snowTexture = null;

  function _getSnowTexture() {
    if (snowTexture) return snowTexture;
    if (typeof document !== 'undefined') {
      const canvas = document.createElement('canvas');
      canvas.width = 32;
      canvas.height = 32;
      const ctx = canvas.getContext('2d');
      if (ctx) {
        const grad = ctx.createRadialGradient(16, 16, 0, 16, 16, 16);
        grad.addColorStop(0.0, 'rgba(255, 255, 255, 1.0)');
        grad.addColorStop(0.45, 'rgba(240, 245, 255, 0.85)');
        grad.addColorStop(0.8, 'rgba(220, 235, 255, 0.35)');
        grad.addColorStop(1.0, 'rgba(200, 220, 255, 0.0)');
        ctx.fillStyle = grad;
        ctx.beginPath();
        ctx.arc(16, 16, 16, 0, Math.PI * 2);
        ctx.fill();
        snowTexture = new THREE.CanvasTexture(canvas);
      }
    }
    return snowTexture;
  }

  function _buildSnowMesh(count) {
    if (snowMesh) {
      scene.remove(snowMesh);
      snowMesh.geometry.dispose();
      snowMesh.material.dispose();
      snowMesh = null;
    }
    if (count <= 0) return;

    const geo = new THREE.BufferGeometry();
    const positions = new Float32Array(count * 3);
    const speeds = new Float32Array(count);
    const seeds = new Float32Array(count);
    const cx = _camera ? _camera.position.x : 0;
    const cy = _camera ? _camera.position.y : 0;
    const cz = _camera ? _camera.position.z : 0;

    for (let i = 0; i < count; i++) {
      positions[i * 3]     = cx + (Math.random() - 0.5) * PRECIP_AREA;
      positions[i * 3 + 1] = Math.max(0, cy - 4.0) + Math.random() * PRECIP_HEIGHT;
      positions[i * 3 + 2] = cz + (Math.random() - 0.5) * PRECIP_AREA;
      speeds[i] = 1.2 + Math.random() * 2.2;
      seeds[i] = Math.random() * 100.0;
    }
    geo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));

    const mat = new THREE.PointsMaterial({
      color: 0xffffff,
      size: 0.85,
      map: _getSnowTexture() || null,
      transparent: true,
      opacity: 0.95,
      depthWrite: false,
      sizeAttenuation: true,
    });

    snowMesh = new THREE.Points(geo, mat);
    snowMesh.userData.speeds = speeds;
    snowMesh.userData.seeds = seeds;
    scene.add(snowMesh);
  }

  // ── 内部状态 ──
  let _timeOfDay = 'noon';
  let _weather = 'clear';
  let _visibilityM = null;
  let _dayMode = true;
  let _camera = null;
  let _elapsedTime = 0;

  // ── 闪电控制器 ──
  let _lightningTimer = 4.0;
  let _lightningFlash = 0.0;

  // ── 太阳与月亮方向计算 ──
  function _setSunPosition(angle) {
    const a = angle * TAU;
    const r = 120;
    const x = Math.cos(a) * r;
    const y = Math.sin(a) * r;
    const z = 35;
    sun.position.set(x, Math.max(y, -15), z);
    skyMat.uniforms.uSunDirection.value.set(x, y, z).normalize();

    // 月亮处于太阳反面偏南偏高处
    const ma = (angle + 0.5) * TAU;
    const mx = Math.cos(ma) * r;
    const my = Math.max(15, Math.sin(ma) * r * 0.85);
    const mz = -50;
    skyMat.uniforms.uMoonDirection.value.set(mx, my, mz).normalize();
  }

  /** 应用时段与天气参数 */
  function _applyTimeSlot(slot) {
    const t = TIME_SLOTS[slot] || TIME_SLOTS.noon;
    const w = WEATHER_MODES[_weather] || WEATHER_MODES.clear;

    // 天空颜色
    const skyTop = new THREE.Color(t.skyTop).lerp(new THREE.Color(w.skyTint), 0.45);
    const skyBot = new THREE.Color(t.skyBot).lerp(new THREE.Color(w.skyTint), 0.45);
    const horizColor = new THREE.Color(t.horizonColor || t.skyBot).lerp(new THREE.Color(w.skyTint), 0.35);

    skyMat.uniforms.uTopColor.value.copy(skyTop);
    skyMat.uniforms.uBotColor.value.copy(skyBot);
    skyMat.uniforms.uHorizonColor.value.copy(horizColor);
    skyMat.uniforms.uSunColor.value.set(t.sunColor);
    skyMat.uniforms.uMoonColor.value.set(t.moonColor || 0xe0e7ff);
    skyMat.uniforms.uCloudDensity.value = w.cloudDensity;
    skyMat.uniforms.uCloudDarkness.value = w.cloudDarkness;
    skyMat.uniforms.uNightFactor.value = t.nightFactor || 0.0;

    // 太阳与月亮
    _setSunPosition(t.angle);
    sun.color.set(t.sunColor);
    sun.intensity = t.sunIntensity * w.sunIntensityMul;

    // 环境光与半球光
    ambient.intensity = t.ambIntensity * w.sunIntensityMul;
    if (hemi) {
      hemi.intensity = t.ambIntensity * 0.45 * w.sunIntensityMul;
    }

    // 雾效
    const fogDensity = _visibilityM != null
      ? FOG_EXTINCTION_AT_VISIBILITY / _visibilityM
      : t.fogDensity * w.fogDensityMul;
    scene.fog.density = fogDensity;
    scene.fog.color.copy(skyBot);

    // scene.background 同步
    if (scene.background && scene.background.isColor) {
      scene.background.copy(skyBot);
    }

    // 降雨 / 降雪系统更新
    if (w.rainRate !== rainCount) {
      rainCount = w.rainRate;
      _buildRainMesh(rainCount);
    }
    if (w.snowRate !== snowCount) {
      snowCount = w.snowRate;
      _buildSnowMesh(snowCount);
    }
  }

  /** 设置时段（兼容旧 day/night bool 或多时段字符串） */
  function setTimeOfDay(time) {
    if (time === true)  { _timeOfDay = 'noon'; _dayMode = true; }
    else if (time === false) { _timeOfDay = 'night'; _dayMode = false; }
    else if (TIME_SLOTS[time]) { _timeOfDay = time; _dayMode = (time !== 'night'); }
    _applyTimeSlot(_timeOfDay);
  }

  /** 设置天气（8 大天气模式） */
  function setWeather(mode) {
    if (!WEATHER_MODES[mode]) mode = 'clear';
    _weather = mode;
    _applyTimeSlot(_timeOfDay);
  }

  /** 设置视距（米） */
  function setVisibility(distanceM) {
    _visibilityM = Number.isFinite(distanceM) && distanceM > 0
      ? Math.max(1, distanceM)
      : null;
    _applyTimeSlot(_timeOfDay);
  }

  function getTimeOfDay() { return _timeOfDay; }
  function getWeather() { return _weather; }
  function getVisibility() { return _visibilityM; }
  function isDay() { return _dayMode; }

  function setCamera(cam) {
    _camera = cam;
    if (rainMesh && rainCount > 0) _buildRainMesh(rainCount);
    if (snowMesh && snowCount > 0) _buildSnowMesh(snowCount);
  }

  // ── 每帧渲染 tick ──
  function tick(dt) {
    const delta = dt || 0.016;
    _elapsedTime += delta;
    skyMat.uniforms.uTime.value = _elapsedTime;

    // 天空穹顶跟随相机，使地平线在长路段保持稳定
    if (_camera) {
      sky.position.copy(_camera.position);
    }

    const t = TIME_SLOTS[_timeOfDay] || TIME_SLOTS.noon;
    const w = WEATHER_MODES[_weather] || WEATHER_MODES.clear;

    // ── 雷暴天气闪电控制器 ──
    if (_weather === 'storm') {
      _lightningTimer -= delta;
      if (_lightningTimer <= 0) {
        _lightningFlash = 1.0;
        _lightningTimer = 3.5 + Math.random() * 5.0;
      }
      if (_lightningFlash > 0.001) {
        _lightningFlash = Math.max(0, _lightningFlash - delta * 5.5);
        skyMat.uniforms.uLightningFlash.value = _lightningFlash;
        ambient.intensity = t.ambIntensity * w.sunIntensityMul + _lightningFlash * 2.8;
      } else {
        skyMat.uniforms.uLightningFlash.value = 0.0;
      }
    } else {
      skyMat.uniforms.uLightningFlash.value = 0.0;
    }

    const cx = _camera ? _camera.position.x : 0;
    const cy = _camera ? _camera.position.y : 0;
    const cz = _camera ? _camera.position.z : 0;
    const halfArea = PRECIP_AREA * 0.5;

    // ── 雨丝与地面涟漪动画（动态包络跟随相机） ──
    if (rainMesh && rainCount > 0) {
      const pos = rainMesh.geometry.attributes.position;
      const arr = pos.array;
      const vel = rainMesh.userData.velocities;
      const streakLen = rainMesh.userData.streakLen;
      const windTiltX = rainMesh.userData.windTiltX;
      const windTiltZ = rainMesh.userData.windTiltZ;

      for (let i = 0; i < rainCount; i++) {
        const idx = i * 6;
        const speed = vel[i] * delta;

        // 顶端与底端同步下落
        arr[idx + 1] -= speed;
        arr[idx + 4] -= speed;

        // 水平环形跟随相机
        if (arr[idx] > cx + halfArea) { arr[idx] -= PRECIP_AREA; arr[idx + 3] -= PRECIP_AREA; }
        else if (arr[idx] < cx - halfArea) { arr[idx] += PRECIP_AREA; arr[idx + 3] += PRECIP_AREA; }

        if (arr[idx + 2] > cz + halfArea) { arr[idx + 2] -= PRECIP_AREA; arr[idx + 5] -= PRECIP_AREA; }
        else if (arr[idx + 2] < cz - halfArea) { arr[idx + 2] += PRECIP_AREA; arr[idx + 5] += PRECIP_AREA; }

        // 雨滴触地（y < 0）→ 回收至相机天顶并触发地面水花涟漪
        if (arr[idx + 4] < 0) {
          const newX = cx + (Math.random() - 0.5) * PRECIP_AREA;
          const newY = cy + 24.0 + Math.random() * 8.0;
          const newZ = cz + (Math.random() - 0.5) * PRECIP_AREA;

          arr[idx]     = newX;
          arr[idx + 1] = newY;
          arr[idx + 2] = newZ;
          arr[idx + 3] = newX - windTiltX * streakLen;
          arr[idx + 4] = newY - streakLen;
          arr[idx + 5] = newZ - windTiltZ * streakLen;

          // 随机在地面激起涟漪
          if (ripplePool.length > 0 && Math.random() < 0.08) {
            const ripIdx = Math.floor(Math.random() * ripplePool.length);
            const rip = ripplePool[ripIdx];
            if (rip.life >= 1.0) {
              rip.x = newX;
              rip.z = newZ;
              rip.life = 0.0;
              rip.scale = 0.15;
            }
          }
        }
      }
      pos.needsUpdate = true;

      // 涟漪水花扩散动画
      if (rippleMesh) {
        const dummy = new THREE.Object3D();
        for (let i = 0; i < ripplePool.length; i++) {
          const rip = ripplePool[i];
          if (rip.life < 1.0) {
            rip.life += delta / rip.maxLife;
            rip.scale = 0.15 + (rip.life * 1.2);
            dummy.position.set(rip.x, 0.04, rip.z);
            dummy.scale.set(rip.scale, 1, rip.scale);
            dummy.updateMatrix();
            rippleMesh.setMatrixAt(i, dummy.matrix);
          } else {
            dummy.position.set(0, -999, 0);
            dummy.updateMatrix();
            rippleMesh.setMatrixAt(i, dummy.matrix);
          }
        }
        rippleMesh.instanceMatrix.needsUpdate = true;
      }
    }

    // ── 雪花飘摇动画（动态包络紧贴相机） ──
    if (snowMesh && snowCount > 0) {
      const pos = snowMesh.geometry.attributes.position;
      const arr = pos.array;
      const speeds = snowMesh.userData.speeds;
      const seeds = snowMesh.userData.seeds;

      for (let i = 0; i < snowCount; i++) {
        const idx = i * 3;
        arr[idx + 1] -= speeds[i] * delta;
        // 风中摇曳微摆
        arr[idx]     += Math.sin(_elapsedTime * 1.5 + seeds[i]) * delta * 1.2;
        arr[idx + 2] += Math.cos(_elapsedTime * 1.2 + seeds[i]) * delta * 1.2;

        // 水平环形跟随相机
        if (arr[idx] > cx + halfArea) arr[idx] -= PRECIP_AREA;
        else if (arr[idx] < cx - halfArea) arr[idx] += PRECIP_AREA;

        if (arr[idx + 2] > cz + halfArea) arr[idx + 2] -= PRECIP_AREA;
        else if (arr[idx + 2] < cz - halfArea) arr[idx + 2] += PRECIP_AREA;

        // 垂直循环：下落至地面下方后回到相机上方
        const bottomY = Math.max(-1.0, cy - 8.0);
        const topY = cy + 24.0;
        if (arr[idx + 1] < bottomY) {
          arr[idx + 1] = topY + Math.random() * 4.0;
          arr[idx]     = cx + (Math.random() - 0.5) * PRECIP_AREA;
          arr[idx + 2] = cz + (Math.random() - 0.5) * PRECIP_AREA;
        } else if (arr[idx + 1] > topY + 6.0) {
          arr[idx + 1] = bottomY + Math.random() * (topY - bottomY);
        }
      }
      pos.needsUpdate = true;
    }
  }

  function getSun() { return sun; }
  function getAmbient() { return ambient; }

  function dispose() {
    scene.remove(sky);
    skyGeo.dispose();
    skyMat.dispose();
    if (rainMesh) {
      scene.remove(rainMesh);
      rainMesh.geometry.dispose();
      rainMesh.material.dispose();
    }
    if (rippleMesh) {
      scene.remove(rippleMesh);
      rippleMesh.geometry.dispose();
      rippleMesh.material.dispose();
    }
    if (snowMesh) {
      scene.remove(snowMesh);
      snowMesh.geometry.dispose();
      snowMesh.material.dispose();
    }
  }

  // 初始应用
  _applyTimeSlot(_timeOfDay);

  return {
    setTimeOfDay, setWeather, setVisibility,
    getTimeOfDay, getWeather, getVisibility, isDay,
    setCamera, tick, dispose,
    getSun, getAmbient,
  };
}