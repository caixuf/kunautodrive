/**
 * CanvasTextureFactory.js — 统一 Canvas 纹理工厂
 *
 * 所有视图的 Canvas 纹理创建唯一入口，消除重复代码。
 * 统一字体栈、抗锯齿、padding、颜色规范。
 *
 * 提供三类纹理：
 *   1. makeLabelTexture   — 实体标签（speed + ai_state，圆角背景）
 *   2. makeSignTexture    — 交通指示牌（文字 + 背景色 + 边框）
 *   3. makeStripeTexture  — 条纹纹理（施工围栏、警示带等）
 *
 * 所有纹理自动缓存，相同参数返回同一纹理对象。
 */

// ── 统一字体栈 ──
const FONT_MONO = `'JetBrains Mono', 'Inter', monospace, sans-serif`;
const FONT_SANS = `'Inter', 'Microsoft YaHei', 'PingFang SC', 'Noto Sans SC', sans-serif`;
const FONT_SIGN = `'Microsoft YaHei', 'SimHei', 'Noto Sans SC', 'PingFang SC', sans-serif`;

// ── 缓存 ──
const _cache = new Map();
/* 缓存上界：静态纹理（指示牌/条纹/低变化 label）数量有限，防止意外路径
 * 把缓存撑到无界（label 的 speed 是典型高变 key，已走 noCache）。 */
const _CACHE_MAX = 300;

function _cacheKey(...args) {
  return args.map(a => (typeof a === 'object' ? JSON.stringify(a) : String(a))).join('|');
}

/** 通用 canvas 创建 */
function _createCanvas(w, h) {
  const c = document.createElement('canvas');
  c.width = w;
  c.height = h;
  return c;
}

/** 通用 CanvasTexture 创建 */
function _toTexture(canvas, wrapS = THREE.ClampToEdgeWrapping, wrapT = THREE.ClampToEdgeWrapping) {
  const tex = new THREE.CanvasTexture(canvas);
  tex.wrapS = wrapS;
  tex.wrapT = wrapT;
  return tex;
}

// ═══════════════════════════════════════════════════════════
// 1. 实体标签纹理（LabelView 用）
// ═══════════════════════════════════════════════════════════

/**
 * 创建实体标签纹理（speed + ai_state，圆角半透明背景）
 * @param {number|null} speed  速度 (m/s)，null 显示 '?'
 * @param {string} aiState     AI 状态文本，空字符串不显示
 * @param {object} [opts]      可选配置
 * @param {number} [opts.width=256]   画布宽度
 * @param {number} [opts.height=64]   画布高度
 * @param {number} [opts.fontSize=28] 主文字大小
 * @returns {THREE.CanvasTexture}
 */
export function makeLabelTexture(speed, aiState, opts = {}) {
  const { width = 256, height = 64, fontSize = 28, noCache = false } = opts;
  // 车速每帧在变（20.01→20.02…），若按 speed 进缓存则每帧新增一个 key →
  // 缓存无界膨胀（浏览器 5GB 泄漏，2026-08-04 实测）。LabelView 换纹理时
  // 会 dispose 旧纹理，所以 ephemeral label 直接 noCache，不进缓存。
  if (!noCache) {
    const key = _cacheKey('label', speed, aiState, width, height, fontSize);
    if (_cache.has(key)) return _cache.get(key);
    const tex = _buildLabelTexture(speed, aiState, width, height, fontSize);
    // 缓存有界保护：只缓存静态 label（speed 不变才复用）；超限丢最旧。
    if (_cache.size >= _CACHE_MAX) {
      const oldest = _cache.keys().next().value;
      _cache.delete(oldest);
    }
    _cache.set(key, tex);
    return tex;
  }
  return _buildLabelTexture(speed, aiState, width, height, fontSize);
}

/** 实际构建 label 纹理（供 noCache 路径与缓存路径共用） */
function _buildLabelTexture(speed, aiState, width, height, fontSize) {
  const c = _createCanvas(width, height);
  const ctx = c.getContext('2d');

  // 速度文本
  const speedText = (speed != null ? speed.toFixed(1) : '?') + ' m/s';
  ctx.font = `bold ${fontSize}px ${FONT_MONO}`;
  const speedW = ctx.measureText(speedText).width;

  // ai_state 文本
  let stateText = '';
  let stateW = 0;
  if (aiState && aiState !== '') {
    stateText = aiState;
    ctx.font = `${fontSize * 0.7}px ${FONT_SANS}`;
    stateW = ctx.measureText(stateText).width;
  }

  // 布局计算
  const gap = stateText ? 16 : 0;
  const totalW = speedW + gap + stateW;
  const startX = (width - totalW) / 2;
  const padX = 14, padY = 8;
  const bgX = startX - padX;
  const bgW = totalW + padX * 2;
  const bgY = (height - fontSize) / 2 - padY;
  const bgH = fontSize + padY * 2;

  // 背景圆角矩形（2026-08 淡化：0.65 太黑，观感像"一块黑的长方形"；
  // 0.4 半透明 + 细边框，文字为主背景为辅）
  ctx.beginPath();
  ctx.roundRect(bgX, bgY, bgW, bgH, 8);
  ctx.fillStyle = 'rgba(10, 16, 24, 0.40)';
  ctx.fill();
  ctx.strokeStyle = 'rgba(255, 255, 255, 0.18)';
  ctx.lineWidth = 1;
  ctx.stroke();

  // 速度文字（加细描边，浅背景上仍可读）
  ctx.font = `bold ${fontSize}px ${FONT_MONO}`;
  ctx.textBaseline = 'middle';
  ctx.lineWidth = 3;
  ctx.strokeStyle = 'rgba(0, 0, 0, 0.7)';
  ctx.strokeText(speedText, startX, height / 2);
  ctx.fillStyle = '#ffffff';
  ctx.fillText(speedText, startX, height / 2);

  // ai_state 文字
  if (stateText) {
    ctx.font = `${fontSize * 0.7}px ${FONT_SANS}`;
    ctx.lineWidth = 2.5;
    ctx.strokeStyle = 'rgba(0, 0, 0, 0.7)';
    ctx.strokeText(stateText, startX + speedW + gap, height / 2);
    ctx.fillStyle = '#c9d4e0';
    ctx.fillText(stateText, startX + speedW + gap, height / 2);
  }

  return _toTexture(c);
}

// ═══════════════════════════════════════════════════════════
// 2. 交通指示牌纹理（ConstructionView / ETCGateView 用）
// ═══════════════════════════════════════════════════════════

/**
 * 创建交通指示牌纹理（文字 + 背景色 + 边框）
 * @param {string} text     指示牌文字
 * @param {string} bg       背景色（CSS 颜色值）
 * @param {string} fg       文字颜色（CSS 颜色值）
 * @param {string} border   边框颜色（CSS 颜色值）
 * @param {object} [opts]   可选配置
 * @param {number} [opts.width=256]   画布宽度
 * @param {number} [opts.height=192]  画布高度
 * @param {number} [opts.fontSize=54] 文字大小
 * @param {number} [opts.borderWidth=5] 边框宽度
 * @returns {THREE.CanvasTexture}
 */
export function makeSignTexture(text, bg, fg, border, opts = {}) {
  const { width = 256, height = 192, fontSize = 54, borderWidth = 5 } = opts;
  const key = _cacheKey('sign', text, bg, fg, border, width, height, fontSize, borderWidth);
  if (_cache.has(key)) return _cache.get(key);

  const c = _createCanvas(width, height);
  const ctx = c.getContext('2d');

  // 背景
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, width, height);

  // 边框
  ctx.strokeStyle = border;
  ctx.lineWidth = borderWidth;
  ctx.strokeRect(borderWidth / 2, borderWidth / 2, width - borderWidth, height - borderWidth);

  // 文字
  ctx.fillStyle = fg;
  ctx.font = `bold ${fontSize}px ${FONT_SIGN}`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(text, width / 2, height / 2);

  const tex = _toTexture(c);
  _cache.set(key, tex);
  return tex;
}

// ═══════════════════════════════════════════════════════════
// 3. 条纹纹理（ConstructionView 施工围栏用）
// ═══════════════════════════════════════════════════════════

/**
 * 创建条纹纹理（横向交替色带 + 反光渐变）
 * @param {string} color1   颜色1（奇数条）
 * @param {string} color2   颜色2（偶数条）
 * @param {object} [opts]   可选配置
 * @param {number} [opts.width=64]    画布宽度
 * @param {number} [opts.height=256]  画布高度
 * @param {number} [opts.stripeCount=8] 条纹数量
 * @param {boolean} [opts.reflective=true] 是否加反光渐变
 * @returns {THREE.CanvasTexture}
 */
export function makeStripeTexture(color1, color2, opts = {}) {
  const { width = 64, height = 256, stripeCount = 8, reflective = true } = opts;
  const key = _cacheKey('stripe', color1, color2, width, height, stripeCount, reflective);
  if (_cache.has(key)) return _cache.get(key);

  const c = _createCanvas(width, height);
  const ctx = c.getContext('2d');
  const stripeH = height / stripeCount;

  for (let i = 0; i < stripeCount; i++) {
    ctx.fillStyle = i % 2 === 0 ? color1 : color2;
    ctx.fillRect(0, i * stripeH, width, stripeH);
  }

  // 反光渐变覆盖
  if (reflective) {
    const g = ctx.createLinearGradient(0, 0, width, 0);
    g.addColorStop(0, 'rgba(255,255,255,0.15)');
    g.addColorStop(0.5, 'rgba(255,255,255,0.0)');
    g.addColorStop(1, 'rgba(255,255,255,0.25)');
    ctx.fillStyle = g;
    ctx.fillRect(0, 0, width, height);
  }

  const tex = _toTexture(c, THREE.RepeatWrapping, THREE.RepeatWrapping);
  _cache.set(key, tex);
  return tex;
}

// ═══════════════════════════════════════════════════════════
// 4. GB 14886-2016 交通信号灯光学透镜与倒计时纹理
// ═══════════════════════════════════════════════════════════

const SIGNAL_COLOR_CSS = {
  red:    { core: '#ff4242', glow: '#ff1a1a', dim: '#2d0808' },
  yellow: { core: '#ffc633', glow: '#ff9d00', dim: '#2e1d05' },
  green:  { core: '#2eff8a', glow: '#00d966', dim: '#062b16' },
};

function _putCachedTex(key, tex) {
  if (_cache.size >= _CACHE_MAX) {
    const oldest = _cache.keys().next().value;
    _cache.delete(oldest);
  }
  _cache.set(key, tex);
  return tex;
}

/**
 * 创建 GB 14886-2016 400mm 信号灯透镜纹理（满盘圆灯 / 左转箭头 / 直行箭头）。
 * 配合 MeshStandardMaterial 的 map + emissiveMap：亮灯时仅图形区域高亮度发光，
 * 熄灯时呈深邃暗色光学孔径，清晰区分黑底面罩与箭头轮廓。
 * @param {'circle'|'left'|'straight'} kind
 * @param {'red'|'yellow'|'green'} colorKey
 * @param {boolean} [active=true]  true=高对比亮态轮廓，false=暗态微弱余晖底纹
 */
export function makeSignalLensTexture(kind, colorKey, active = true) {
  if (typeof document === 'undefined') return null;
  const key = _cacheKey('sigLens', kind, colorKey, active ? 1 : 0);
  if (_cache.has(key)) return _cache.get(key);

  const size = 128;
  const c = _createCanvas(size, size);
  const ctx = c.getContext('2d');
  const pal = SIGNAL_COLOR_CSS[colorKey] || SIGNAL_COLOR_CSS.red;

  ctx.fillStyle = '#080b10';
  ctx.fillRect(0, 0, size, size);

  // 400mm 圆形光学面罩底孔
  ctx.beginPath();
  ctx.arc(64, 64, 56, 0, Math.PI * 2);
  ctx.fillStyle = '#11161f';
  ctx.fill();
  ctx.lineWidth = 3;
  ctx.strokeStyle = '#1f2836';
  ctx.stroke();

  const drawShape = () => {
    if (kind === 'left') {
      ctx.beginPath();
      ctx.moveTo(20, 64);
      ctx.lineTo(54, 30);
      ctx.lineTo(66, 42);
      ctx.lineTo(48, 57);
      ctx.lineTo(106, 57);
      ctx.lineTo(106, 71);
      ctx.lineTo(48, 71);
      ctx.lineTo(66, 86);
      ctx.lineTo(54, 98);
      ctx.closePath();
      ctx.fill();
    } else if (kind === 'straight') {
      ctx.beginPath();
      ctx.moveTo(64, 20);
      ctx.lineTo(98, 54);
      ctx.lineTo(86, 66);
      ctx.lineTo(71, 48);
      ctx.lineTo(71, 106);
      ctx.lineTo(57, 106);
      ctx.lineTo(57, 48);
      ctx.lineTo(42, 66);
      ctx.lineTo(30, 54);
      ctx.closePath();
      ctx.fill();
    } else {
      ctx.beginPath();
      ctx.arc(64, 64, 46, 0, Math.PI * 2);
      ctx.fill();
    }
  };

  if (active) {
    const grad = ctx.createRadialGradient(64, 64, 6, 64, 64, 52);
    grad.addColorStop(0.0, '#ffffff');
    grad.addColorStop(0.28, pal.core);
    grad.addColorStop(1.0, pal.glow);
    ctx.fillStyle = grad;
    drawShape();

    // 细微 LED 像素点阵纹理（增强近景工程质感）
    ctx.fillStyle = 'rgba(0, 0, 0, 0.16)';
    for (let y = 16; y < size - 12; y += 8) {
      for (let x = 16; x < size - 12; x += 8) {
        ctx.fillRect(x, y, 2, 2);
      }
    }
  } else {
    ctx.fillStyle = pal.dim;
    drawShape();
  }

  return _putCachedTex(key, _toTexture(c));
}

/**
 * 创建 GA/T 508 信号灯数字倒计时牌纹理（支持单相或 ←左 / ↑直 双排显示）。
 * 仅在整数秒或相位变化时刷新 key，复用有界缓存。
 */
export function makeSignalCountdownTexture(rows) {
  if (typeof document === 'undefined') return null;
  const sig = rows.map(r => `${r.prefix}:${r.sec}:${r.color}`).join(';');
  const key = _cacheKey('sigCd', sig);
  if (_cache.has(key)) return _cache.get(key);

  const w = 128, h = rows.length > 1 ? 160 : 96;
  const c = _createCanvas(w, h);
  const ctx = c.getContext('2d');

  ctx.fillStyle = '#070a0f';
  ctx.fillRect(0, 0, w, h);
  ctx.strokeStyle = '#d8e0eb';
  ctx.lineWidth = 4;
  ctx.strokeRect(3, 3, w - 6, h - 6);

  const colorCss = (st) => (st === 'green' ? '#36ff94' : st === 'yellow' ? '#ffca3a' : '#ff4747');
  const rowH = h / Math.max(1, rows.length);
  rows.forEach((row, idx) => {
    const cy = rowH * (idx + 0.5);
    const col = colorCss(row.color);
    if (idx > 0) {
      ctx.fillStyle = '#243040';
      ctx.fillRect(10, rowH * idx - 1, w - 20, 2);
    }
    ctx.fillStyle = '#9aa8ba';
    ctx.font = `bold 24px ${FONT_SIGN}`;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
    ctx.fillText(row.prefix || '', 12, cy);

    const secTxt = row.sec >= 0 ? String(row.sec).padStart(2, '0') : '--';
    ctx.fillStyle = col;
    ctx.font = `bold 48px ${FONT_MONO}`;
    ctx.textAlign = 'right';
    ctx.fillText(secTxt, w - 12, cy + 1);
  });

  return _putCachedTex(key, _toTexture(c));
}

// ═══════════════════════════════════════════════════════════
// 5. GB 5768 交通标志与门架横幅程序化纹理
// ═══════════════════════════════════════════════════════════

/** 创建圆形白底红圈限速标志纹理 (e.g. 60 / 80)。 */
export function makeSpeedSignTexture(speedKph = 60) {
  if (typeof document === 'undefined') return null;
  const val = Math.round(Number(speedKph) || 60);
  const key = _cacheKey('speedSign', val);
  if (_cache.has(key)) return _cache.get(key);

  const size = 128;
  const c = _createCanvas(size, size);
  const ctx = c.getContext('2d');
  ctx.fillStyle = '#0d1117';
  ctx.fillRect(0, 0, size, size);

  ctx.beginPath();
  ctx.arc(64, 64, 58, 0, Math.PI * 2);
  ctx.fillStyle = '#f5f7fa';
  ctx.fill();
  ctx.lineWidth = 12;
  ctx.strokeStyle = '#d62828';
  ctx.stroke();

  ctx.fillStyle = '#11161d';
  ctx.font = `bold 52px ${FONT_SIGN}`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(String(val), 64, 66);

  return _putCachedTex(key, _toTexture(c));
}

/** 创建八角形 STOP 停车让行标志纹理。 */
export function makeStopSignTexture(label = 'STOP') {
  if (typeof document === 'undefined') return null;
  const key = _cacheKey('stopSign', label);
  if (_cache.has(key)) return _cache.get(key);

  const size = 128;
  const c = _createCanvas(size, size);
  const ctx = c.getContext('2d');
  ctx.fillStyle = '#d62828';
  ctx.fillRect(0, 0, size, size);
  ctx.strokeStyle = '#f5f7fa';
  ctx.lineWidth = 6;
  ctx.strokeRect(5, 5, size - 10, size - 10);

  ctx.fillStyle = '#ffffff';
  ctx.font = `bold 36px ${FONT_SIGN}`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(String(label || 'STOP'), 64, 66);

  return _putCachedTex(key, _toTexture(c));
}

/** 创建黄底黑框警告标志纹理。 */
export function makeWarningSignTexture(symbol = '!') {
  if (typeof document === 'undefined') return null;
  const key = _cacheKey('warnSign', symbol);
  if (_cache.has(key)) return _cache.get(key);

  const size = 128;
  const c = _createCanvas(size, size);
  const ctx = c.getContext('2d');
  ctx.fillStyle = '#f4b400';
  ctx.fillRect(0, 0, size, size);
  ctx.strokeStyle = '#181c22';
  ctx.lineWidth = 8;
  ctx.strokeRect(6, 6, size - 12, size - 12);

  ctx.fillStyle = '#181c22';
  ctx.font = `bold 58px ${FONT_SIGN}`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(String(symbol || '!'), 64, 68);

  return _putCachedTex(key, _toTexture(c));
}

/** 创建门架/入口地标横幅纹理。 */
export function makeGantryBannerTexture(title = 'FLOWENGINE CITY', subtitle = 'PROCEDURAL WORLD ENGINE', bg = '#154d8a') {
  if (typeof document === 'undefined') return null;
  const key = _cacheKey('gantryBanner', title, subtitle, bg);
  if (_cache.has(key)) return _cache.get(key);

  const w = 512, h = 128;
  const c = _createCanvas(w, h);
  const ctx = c.getContext('2d');
  ctx.fillStyle = bg;
  ctx.fillRect(0, 0, w, h);
  ctx.strokeStyle = '#e8eef5';
  ctx.lineWidth = 6;
  ctx.strokeRect(6, 6, w - 12, h - 12);

  ctx.fillStyle = '#ffffff';
  ctx.font = `bold 40px ${FONT_SANS}`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(String(title), w / 2, subtitle ? 48 : 64);

  if (subtitle) {
    ctx.fillStyle = '#cfe4ff';
    ctx.font = `bold 20px ${FONT_SANS}`;
    ctx.fillText(String(subtitle), w / 2, 92);
  }

  return _putCachedTex(key, _toTexture(c));
}

// ═══════════════════════════════════════════════════════════
// 工具函数
// ═══════════════════════════════════════════════════════════

/** 清空缓存（测试/切场景时调用） */
export function clearTextureCache() {
  for (const [, tex] of _cache) {
    tex.dispose();
  }
  _cache.clear();
}

/** 获取缓存大小（调试用） */
export function getTextureCacheSize() {
  return _cache.size;
}
