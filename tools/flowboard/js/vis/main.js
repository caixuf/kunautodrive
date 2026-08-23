/**
 * main.js — vis/ 架构入口
 * 组装所有模块，导出 app.js 期望的接口。
 * 取代旧 scene3d_v2.js（3000 行 God Object）。
 *
 * app.js import:
 *   import { init3DScene, resize3D, update3D, sceneReady, scene3d,
 *            setTopoData, setCameraMode, resetCamera, resetMapView,
 *            closeNPCDetail, setPerfTier } from './vis/main.js';
 */

import { createRenderer, createComposer, renderFrame, resize, getRendererInfo, resetRendererInfo, setComposerGTAOPassEnabled, setResolutionScale, isSoftwareRenderer, setBloomTech } from './core/Renderer.js';
import { createCameraRig } from './core/CameraRig.js';
import { createLighting, updateSunShadow } from './core/Lighting.js';
import { createSkyEnv } from './core/SkyEnv.js';
import { PerfMonitor } from './core/PerfMonitor.js';
import { createSceneDirector } from './director/SceneDirector.js';
import { clearCache } from './core/AssetFactory.js';
import { initModels } from './view/VehicleView.js';
import { createStatsView } from './view/StatsView.js';
import { createMinimapHUD } from './MinimapHUD.js';
import { STYLE } from './theme/roadStyle.js';

// ── 模块级状态（只此一处，取代旧架构 51 个 let）──
let _scene = null;
let _renderer = null;
let _composer = null;
let _cameraRig = null;
let _lights = null;
let _lastEnvironmentKey = '';
let _skyEnv = null;
let _director = null;
let _ready = false;
let _lastTopoData = null;
let _statsView = null;
let _lastMinimapDrawMs = 0;
let _renderPaused = false;
const MINIMAP_INTERVAL_MS = 1000 / 30;

let _userEnvOverride = null;

export function setUserEnvironment(env) {
  _userEnvOverride = env ? { ...env } : null;
  if (_director) {
    const store = _director.getStore();
    if (store && env) {
      if (env.lighting) store.env.lighting = env.lighting;
      if (env.weather) store.env.weather = env.weather;
      if (env.visibilityM) store.env.visibilityM = env.visibilityM;
      store.env.userOverride = { ...env };
    }
    _syncEnvironment(store);
  }
}

function _syncEnvironment(store) {
  if (!_skyEnv || !store) return;
  const env = store.env || {};
  const lighting = (_userEnvOverride && _userEnvOverride.lighting) || env.lighting || 'day';
  const weather = (_userEnvOverride && _userEnvOverride.weather) || env.weather || 'clear';
  let visibilityM = (_userEnvOverride && _userEnvOverride.visibilityM) || env.visibilityM;
  if (!Number.isFinite(visibilityM) || visibilityM <= 0) {
    visibilityM = 20000;
  }
  const environmentKey = `${lighting}|${weather}|${Math.round(visibilityM)}`;
  if (environmentKey === _lastEnvironmentKey) return;
  _skyEnv.setTimeOfDay(lighting === 'day' ? 'noon' : lighting);
  _skyEnv.setWeather(weather);
  _skyEnv.setVisibility(visibilityM);
  _lastEnvironmentKey = environmentKey;
}
let _minimap = null;

/* ── 性能档位（Performance Tier）──
 * 默认 low（极速原生模式）：直接前向渲染 + 原生硬件 4x MSAA，0 后处理带宽开销，
 * 在普通集显机器（Intel Iris/UHD, AMD Radeon Graphics）上也能稳定 100~144fps。
 * 档位语义（_applyPerfTier 实现）：
 *   high   — composer 全开（GTAO+Bloom+SMAA），阴影 4096，DPR min(dpr,1.5)
 *   medium — composer 开但关 GTAO，阴影 2048，DPR min(dpr,1.5)
 *   low    — 极速原生渲染（直接前向 + 硬件 MSAA），关阴影，DPR 1（100+ FPS 默认）
 *   ultra  — 同 low，再压低渲染分辨率（0.5x）由 CSS 放大，最后兜底
 * 自动降级由独立 watchdog（PerfMonitor，setInterval 不依赖 rAF）驱动。 */
let _perfTier = 'low';
let _perfMonitor = null;
let _lastReportTs = 0;   // 上报节流：每 5s 最多上报一次可视化健康

/** 暴露 scene 对象（app.js 直接 import scene3d）*/
export let scene3d = null;

/** 同步后处理 pass 状态：档位 + 相机模式共同决定 GTAO。
 *  档位：medium 关 GTAO（最贵一趟），low/ultra 整条管线禁用。
 *  相机：BEV（正交）与 GTAO 不兼容，切过去时关 GTAO；其余视角按档位。
 *  在 _applyPerfTier 与 setCameraMode 两处调用，保证切档/切视角后状态一致。 */
function _syncPostProc() {
  if (!_composer) return;
  const isLow = _perfTier === 'low';
  const isUltra = _perfTier === 'ultra';
  const isMedium = _perfTier === 'medium';
  const bev = !!( _cameraRig && _cameraRig.isBev() );
  // low/ultra 时 composer 整体禁用走 renderer 旁路，此处只需管 medium/high 下
  // 的 GTAO：medium 关（性能）、BEV 关（正交不兼容），high+透视才开。
  setComposerGTAOPassEnabled(_composer, !isLow && !isUltra && !isMedium && !bev);
}

// ── 导出给 app.js 的接口 ──

/** 初始化 3D 场景 */
export function init3DScene(canvas) {
  // 幂等：已初始化则跳过（app.js 可能同时调 init3DScene + switchSceneView → 各 init 一次）
  if (_scene && _renderer) return _scene;

  // app.js 调 init3DScene() 不传参数，自己找/建 canvas
  if (!canvas) {
    canvas = document.getElementById('scene3d-canvas');
    if (!canvas) {
      // 找 #scene3d 容器 div，在里面建 canvas
      const container = document.getElementById('scene3d') || document.body;
      canvas = document.createElement('canvas');
      canvas.id = 'scene3d-canvas';
      canvas.style.width = '100%';
      canvas.style.height = '100%';
      canvas.style.display = 'block';
      container.appendChild(canvas);
    }
  }

  // 检查 Three.js 是否可用
  if (typeof THREE === 'undefined') {
    _showInitError('Three.js 未加载，请检查网络连接 /tools/three.min.js');
    return null;
  }

  try {
    _scene = new THREE.Scene();
    scene3d = _scene;  // 暴露给 app.js
    _renderer = createRenderer(canvas);
  } catch (err) {
    console.error('[vis] WebGL renderer creation failed:', err);
    _showInitError('WebGL 不可用: ' + err.message);
    return null;
  }

  try {
    _cameraRig = createCameraRig(canvas);
    _lights = createLighting(_scene);
    _skyEnv = createSkyEnv(_scene, _lights.sun, _lights.hemi);
    _skyEnv.setCamera(_cameraRig.camera);
    _director = createSceneDirector(_scene);
    _director.init();
    /* 烘焙 PMREM 环境贴图：把当前 scene（天空色 + hemisphere 灯光渐变）
     * 烘成预滤波 mipmap 环境贴图，赋给 scene.environment。
     * 低金属度车漆（metalness=0.15）+ clearcoat 清漆层依赖 envMap
     * 产生高光反射；有了 envMap，车身反射天空渐变高光，才有真车漆质感。
     * 用 fromScene 而非 HDR 文件：离线自洽，无需外部资产。 */
    _bakeEnvironment(_renderer, _scene);
  } catch (err) {
    console.error('[vis] Scene init failed:', err);
    _showInitError('场景初始化失败: ' + err.message);
    return null;
  }

  try {
    _composer = createComposer(_renderer, _scene, _cameraRig.camera);
  } catch (err) {
    console.warn('[vis] Composer creation failed, fallback to direct render:', err.message);
    _composer = null;
  }

  _ready = true;

  // 软件渲染（无 GPU 的 WSL/云 VM）直接低档启动：Bloom+SMAA+阴影在
  // SwiftShader/llvmpipe 下逐像素软件计算会卡成 PPT，等 6-9s 的 PHM
  // 自动降级期间用户已经在看 PPT 了（2026-08-04 后段卡顿实测）。
  if (isSoftwareRenderer(_renderer)) {
    if (_perfTier !== 'low' && _perfTier !== 'ultra') {
      console.warn('[vis] software renderer detected — starting at low tier (no post-processing)');
    }
    _perfTier = 'low';
  }
  // 应用性能档位（medium：关 GTAO、阴影 2048、DPR≤1.5）
  _applyPerfTier(_perfTier);
  _syncPerfTier();

  // 独立 PHM watchdog：setInterval 驱动，不依赖 rAF（GPU 卡死也能降级）
  _perfMonitor = new PerfMonitor({
    windowMs: 1000,
    lowFps: 30,
    downgradeWindows: 3,
    onDowngrade: _onPhmDowngrade,
    onReport: _onPhmReport,
  });
  _perfMonitor.start();

  // 小地图 HUD（叠层 2D canvas，右上角）
  const scene3dContainer = document.getElementById('scene3d');
  if (scene3dContainer) {
    _minimap = createMinimapHUD(scene3dContainer);
  }

  // 性能监控面板（DOM 层，非 3D 场景）
  _statsView = createStatsView();
  document.body.appendChild(_statsView.dom);

  _startRenderLoop();

  // 异步预加载 gltf 车辆模型（SU7/sedan/suv/truck）
  // 加载完成前用程序化 fallback，完成后新车自动用 gltf
  initModels();

  // 立即设置初始尺寸（否则 renderer 默认 300x150，canvas 看起来空白）
  // 用 ResizeObserver 监听容器变化，比 setTimeout 更可靠
  _initResizeObserver(canvas);
  resize3D();

  // 初始化后立即触发一次相机更新，确保第一帧就能看到场景
  try {
    const store = _director.getStore();
    _syncEnvironment(store);
    const roadGroup = store.isViaduct
      ? _director.getViaductView().getGroup()
      : _director.getRoadView().getRoadGroup();
    _cameraRig.update(store.ego, roadGroup, performance.now());
  } catch (e) {
    console.warn('[vis] Initial camera update failed:', e.message);
  }

  console.log('[vis] 3D scene initialized successfully');
  console.log('[vis] isViaduct:', _director.getStore().isViaduct);
  console.log('[vis] ego:', _director.getStore().ego);

  return _scene;
}

/** 烘焙环境贴图并赋给 scene.environment。
 *  用 PMREMGenerator.fromScene 从场景天空 + 半球光渐变烘成环境贴图。
 *  离线自洽，不依赖外网 HDRI。 */
function _bakeEnvironment(renderer, scene) {
  if (!THREE.PMREMGenerator) {
    console.warn('[vis] PMREMGenerator unavailable, PBR reflections disabled');
    return;
  }
  const pmrem = new THREE.PMREMGenerator(renderer);
  pmrem.compileEquirectangularShader();
  _bakeFromScene(pmrem, scene);
}

/** 回退：用 scene 内的天空 + 灯光烘 PMREM */
function _bakeFromScene(pmrem, scene) {
  const envRT = pmrem.fromScene(scene, 0.04);
  scene.environment = envRT.texture;
  pmrem.dispose();
}

/** 显示初始化错误提示 */
function _showInitError(msg) {
  const el = document.getElementById('scene3d-msg');
  if (el) {
    el.setAttribute('data-init-error', '1');
    el.style.display = '';
    el.style.color = '#f85149';
    el.innerHTML = '<div style="font-size:32px;margin-bottom:10px">⚠</div>' +
      '<div style="color:#f85149;font-size:14px;font-weight:600;margin-bottom:6px">3D 初始化失败</div>' +
      '<div style="color:#8b949e;font-size:11px;font-family:monospace;line-height:1.5;max-width:340px;word-break:break-all">' +
      msg + '</div>';
  }
}

/** 监听 canvas 容器尺寸变化，自动 resize */
function _initResizeObserver(canvas) {
  const container = canvas.parentElement || canvas;
  if (typeof ResizeObserver === 'undefined') return;
  const ro = new ResizeObserver(() => { resize3D(); });
  ro.observe(container);
}

/** 渲染循环 */
let _frameCount = 0;
let _lastRenderErr = null;

function _startRenderLoop() {
  let _lastFrameTime = 0;

  function loop(timestamp) {
    requestAnimationFrame(loop);
    // 标签页隐藏时跳过渲染（节省 GPU）
    if (document.hidden) return;
    if (_renderPaused) {
      if (_perfMonitor) _perfMonitor.setActive(false);
      return;
    }
    // observe 工作区不可见时降帧到 10fps（Analyze/Operate 工作区）
    const isObserve = document.body.getAttribute('data-workspace') !== 'analyze' &&
                      document.body.getAttribute('data-workspace') !== 'operate';
    // 3D 不可见（非 observe，主动降帧省 GPU）时，PHM 不参与降档判定，
    // 否则"主动降帧的低 FPS"会被误判为卡顿 → 自动降档关后处理 → 画质永久变差。
    if (_perfMonitor) _perfMonitor.setActive(isObserve);
    /* 可见时不做帧率节流：跟随显示器刷新率（60/100/144Hz）满帧渲染。
     * 旧的 minInterval=16.67ms 节流在 60Hz 屏上与 rAF 帧间隔(~16.6ms)
     * 临界竞争——delta 略小于阈值的帧被跳过 → 下一帧间隔变 33ms →
     * 规律性半帧率抖动（"一顿一顿"的前端成分之一）；在 100Hz+ 屏上
     * 则把帧率封死在 ~50fps。节流只保留给 3D 不可见的工作区。 */
    if (!isObserve && timestamp - _lastFrameTime < 100) return; // 10fps when hidden
    const dtSec = _lastFrameTime > 0
      ? Math.min(0.1, (timestamp - _lastFrameTime) / 1000) : 1 / 60;
    _lastFrameTime = timestamp;

    if (!_ready) return;

    try {
      const now = performance.now();
      const store = _director.getStore();
      /* 把当前相机模式透传给各 View（轨迹等需按 BEV/透视调整视觉尺度） */
      store.isBev = _cameraRig.isBev();
      _syncEnvironment(store);
      const roadGroup = store.isViaduct
        ? _director.getViaductView().getGroup()
        : _director.getRoadView().getRoadGroup();

      // ── 死推算：每帧 advance 平滑位置，弥补 SSE 5Hz 离散数据 ──
      // 架构升级：tickAnimation 内部走 Layer 树递归 update ——
      // agent 层 (vehicle) + infra 层 (trafficLight, etcGate) 都由它驱动，
      // 不再单独调 _director.getVehicleView().update(store, now)。
      _director.tickAnimation(now);

      // 太阳阴影相机跟随 ego（仅当开启阴影时）
      if (_renderer && _renderer.shadowMap && _renderer.shadowMap.enabled) {
        updateSunShadow(_lights, store.ego);
      }

      _cameraRig.update(store.ego, roadGroup, now);

      const activeCam = _cameraRig.getActiveCamera();

      // 天空穹顶跟随相机 + 雨粒子动画（真实帧间 dt，高刷屏下速度不失真）
      _skyEnv.tick(dtSec, activeCam);

      // BEV（正交）GTAO 由 _syncPostProc 关掉（正交不兼容），但保留
      // Bloom+SMAA → 车道线/车灯在 BEV 下也有辉光。low/ultra 仍走旁路。
      const noPost = _perfTier === 'low' || _perfTier === 'ultra';
      if (noPost) {
        _renderer.render(_scene, activeCam);
      } else {
        renderFrame(_renderer, _composer, _scene, activeCam);
      }
      _frameCount++;
      if (_perfMonitor) _perfMonitor.tickFrame();   // 供给 PHM watchdog 采样

      // 小地图是独立 Canvas 的完整重绘；它不需要与 60/120Hz 3D 同步。
      // 固定 30Hz 可消除它与 WebGL/DOM 同帧重绘造成的主线程尖峰。
      if (_minimap && now - _lastMinimapDrawMs >= MINIMAP_INTERVAL_MS) {
        _minimap.draw(store);
        _lastMinimapDrawMs = now;
      }

      // 性能监控面板（每 500ms 更新一次 DOM）
      if (_statsView) {
        _statsView.update(_renderer);
      }
    } catch (err) {
      console.error('[vis] render loop error:', err);
      _lastRenderErr = err;
    }
  }
  loop();
}

// 调试接口（挂到 window 方便控制台诊断）
if (typeof window !== 'undefined') {
  let _wireframeMode = false;
  let _fpsTimes = [];

  window.__vis = {
    get scene() { return _scene; },
    get renderer() { return _renderer; },
    get camera() { return _cameraRig ? _cameraRig.getActiveCamera() : null; },
    get director() { return _director; },
    get ready() { return _ready; },
    get frameCount() { return _frameCount; },
    get lastError() { return _lastRenderErr; },
    get wireframe() { return _wireframeMode; },
    /** 获取渲染性能统计：calls(Draw Call), triangles, geometries, textures */
    get perf() {
      const info = getRendererInfo(_renderer);
      if (!info) return null;
      const now = performance.now();
      _fpsTimes.push(now);
      _fpsTimes = _fpsTimes.filter(t => now - t < 1000);
      return { ...info, fps: _fpsTimes.length };
    },
    /** 获取当前场景状态 */
    get store() {
      return _director ? _director.getStore() : null;
    },
    /** 获取道路组 */
    get roadGroup() {
      return _director ? _director.getRoadView().getRoadGroup() : null;
    },
    /** 获取相机位置和朝向 */
    get cameraInfo() {
      const cam = _cameraRig ? _cameraRig.camera : null;
      if (!cam) return null;
      return {
        pos: { x: cam.position.x, y: cam.position.y, z: cam.position.z },
        fov: cam.fov,
        near: cam.near,
        far: cam.far
      };
    },
    /** 获取实体列表 */
    get entities() {
      const store = _director ? _director.getStore() : null;
      return store ? store.entities : [];
    },
    /** 获取 ego 状态 */
    get ego() {
      const store = _director ? _director.getStore() : null;
      return store ? store.ego : null;
    },
    /** 重置渲染统计 */
    resetPerf() { resetRendererInfo(_renderer); },
    /** 手动渲染一帧（调试用） */
    debugRender() {
      if (!_renderer || !_scene) return 'no renderer/scene';
      try {
        _renderer.render(_scene, _cameraRig.camera);
        return 'rendered ok, frame=' + _frameCount;
      } catch (e) {
        return 'render failed: ' + e.message;
      }
    },
    /** 切换 wireframe 模式 */
    toggleWireframe() {
      _wireframeMode = !_wireframeMode;
      _scene.traverse(o => {
        if (o.material && !o.material.isSpriteMaterial) {
          o.material.wireframe = _wireframeMode;
        }
      });
      return 'wireframe ' + (_wireframeMode ? 'ON' : 'OFF');
    },
    /** 打印场景层级 */
    printHierarchy(root, depth = 0) {
      const obj = root || _scene;
      if (!obj) return;
      const prefix = '  '.repeat(depth);
      const type = obj.type || 'Object3D';
      const name = obj.name || '(unnamed)';
      console.log(prefix + type + (name ? ' "' + name + '"' : ''));
      if (obj.children) {
        obj.children.forEach(c => this.printHierarchy(c, depth + 1));
      }
    },
    /** 显示/隐藏道路 */
    toggleRoad(show) {
      const rg = this.roadGroup;
      if (!rg) return 'no road group';
      rg.visible = show !== false;
      return 'road ' + (rg.visible ? 'SHOWN' : 'HIDDEN');
    },
    /** 显示/隐藏所有 NPC */
    toggleNPCs(show) {
      const store = this.store;
      if (!store) return 'no store';
      const vv = _director ? _director.getVehicleView() : null;
      if (vv) {
        const npcPool = vv.getNPCPool ? vv.getNPCPool() : null;
        if (npcPool) {
          npcPool.forEach(npc => { npc.visible = show !== false; });
          return 'NPCs ' + (show !== false ? 'SHOWN' : 'HIDDEN');
        }
      }
      return 'no NPC pool';
    },
    /** 重置相机位置 */
    resetCamera() {
      if (_cameraRig) _cameraRig.reset(this.roadGroup);
      return 'camera reset';
    },
    /** 设置相机模式 */
    setCameraMode(mode) {
      if (_cameraRig) _cameraRig.setMode(mode);
      return 'camera mode: ' + mode;
    },
    /** 显示当前渲染器配置 */
    get rendererConfig() {
      if (!_renderer) return null;
      return {
        pixelRatio: _renderer.getPixelRatio(),
        shadowMapEnabled: _renderer.shadowMap.enabled,
        shadowMapType: _renderer.shadowMap.type === THREE.PCFSoftShadowMap ? 'PCFSoft'
                     : _renderer.shadowMap.type === THREE.PCFShadowMap ? 'PCF' : 'basic',
        antialias: true,
        toneMapping: 'ACESFilmic',
        outputEncoding: 'sRGB'
      };
    },
    /** 强制重渲染 */
    forceResize() { resize3D(); },
    /** 清除所有错误 */
    clearErrors() { _lastRenderErr = null; },
    /** 测量帧间隔分布（诊断"平均10ms却一卡一卡"的真实抖动源）。
     *  采样 N 帧 rAF 间隔，返回直方图 + 最大/中位/平均。卡顿往往不是
     *  平均帧率问题，而是偶发大间隔（GC/后处理/阴影重建）——这个直方图
     *  能直接暴露 spike 的分布和频率。 */
    frameHistogram(n) {
      const frames = (n && n > 0) ? n : 300;
      return new Promise(function(resolve) {
        let last = performance.now();
        const gaps = [];
        let count = 0;
        function sample() {
          const now = performance.now();
          gaps.push(now - last);
          last = now;
          count++;
          if (count < frames) requestAnimationFrame(sample);
          else {
            const sorted = gaps.slice().sort(function(a, b) { return a - b; });
            const sum = gaps.reduce(function(a, b) { return a + b; }, 0);
            const avg = sum / gaps.length;
            const median = sorted[Math.floor(sorted.length / 2)];
            const max = sorted[sorted.length - 1];
            // 直方图分桶：<17, 17-20, 20-25, 25-33, 33-50, 50-100, >100ms
            const buckets = [0, 0, 0, 0, 0, 0, 0];
            const LABELS = ['<17', '17-20', '20-25', '25-33', '33-50', '50-100', '>100'];
            gaps.forEach(function(g) {
              if (g < 17) buckets[0]++;
              else if (g < 20) buckets[1]++;
              else if (g < 25) buckets[2]++;
              else if (g < 33) buckets[3]++;
              else if (g < 50) buckets[4]++;
              else if (g < 100) buckets[5]++;
              else buckets[6]++;
            });
            const hist = LABELS.map(function(label, i) {
              return label + 'ms: ' + buckets[i] + (buckets[i] ? ' (' + Math.round(buckets[i] / gaps.length * 100) + '%)' : '');
            }).join('\n');
            console.log('[vis] frame histogram (' + frames + ' frames):');
            console.log('  avg=' + avg.toFixed(1) + 'ms  median=' + median.toFixed(1) + 'ms  max=' + max.toFixed(0) + 'ms');
            console.log(hist);
            resolve({ avg, median, max, hist, gaps });
          }
        }
        requestAnimationFrame(sample);
      });
    }
  };
}

/** 调整大小。不传参数时自动从 canvas 父容器读取尺寸。 */
export function resize3D(width, height) {
  if (!_renderer || !_cameraRig) return;
  if (width === undefined || height === undefined) {
    const canvas = _renderer.domElement;
    const container = canvas ? canvas.parentElement : null;
    if (container) {
      const rect = container.getBoundingClientRect();
      width = rect.width || container.clientWidth || window.innerWidth;
      height = rect.height || container.clientHeight || window.innerHeight;
    } else {
      width = window.innerWidth;
      height = window.innerHeight;
    }
  }
  if (width <= 0 || height <= 0) return;  // 容器还没渲染出来
  resize(_renderer, _composer, _cameraRig.camera, width, height);
  // 同步更新 BEV 正交相机视锥（透视/Composer 已由上面的 resize 处理）
  if (_cameraRig && _cameraRig.resize) _cameraRig.resize(width, height);
}

/** 每帧更新（app.js 调用） */
export function update3D(topoData) {
  if (!_director) return;
  _lastTopoData = topoData;
  _director.update(topoData);
}

/**
 * 设置 topology 数据（app.js setTopoData fan-out）。
 * 流畅专题：原先 setTopoData 直接调 update3D → _director.update，而
 * app.js updateAll 又单独 safeCall('scene3d', update3D) 再调一次，导致
 * 每个 SSE tick 触发两次 _director.update（双倍 CPU + store.entities 重写
 * 双倍 GC）。现改为只缓存数据引用，director 更新统一由 update3D 单点驱动。
 * _lastTopoData 仍在这里刷新，方便调试 / 未来读侧使用。
 */
export function setTopoData(data) {
  _lastTopoData = data;
}

/** 场景是否就绪 */
export function sceneReady() {
  return _ready;
}

export function setRenderPaused(paused) {
  _renderPaused = paused === true;
  if (_perfMonitor) _perfMonitor.setActive(!_renderPaused);
}

// scene3d 已在模块顶部 export let 声明，init3DScene 里赋值

/** 切换视角 */
export function setCameraMode(mode) {
  if (_cameraRig) _cameraRig.setMode(mode);
  _syncPostProc();   // BEV 切走 GTAO，其余视角按档位恢复
  _applySceneStyle(mode);  // BEV 升级 SR 科技风，其余视角恢复写实风
}

/** 应用场景风格：BEV（升级后的 SR 视角）用科技风，其余视角保持写实风。
 *  通过各 View 暴露的 setter 动态改材质，不建两套场景、不污染原 3D 界面。
 *  风格参数全部来自 theme/roadStyle.js 的 STYLE 表（换肤即换表）。 */
function _applySceneStyle(mode) {
  if (!_director) return;
  const style = STYLE[mode === 'bev' ? 'sr' : 'real'];
  if (!style) return;
  // 标线发光：SR/BEV 抬高 emissive 配合 Bloom 出霓虹辉光，透视恢复 0（写实）
  const road = _director.getRoadView();
  if (road && road.setMarkingEmissive) {
    road.setMarkingEmissive(style.markingEmissiveWhite, style.markingEmissiveYellow);
  }
  // 地面：SR/BEV 深色冷底，透视还原纯纹理
  const ground = _director.getGroundView();
  if (ground && ground.setTechMode) ground.setTechMode(!!style.groundTint);
  // Bloom：SR/BEV 激进（标线辉光），透视保守
  setBloomTech(style.bloomTech);
}

/** 重置相机 */
export function resetCamera() {
  if (_cameraRig && _director) {
    const store = _director.getStore();
    const roadGroup = store.isViaduct
      ? _director.getViaductView().getGroup()
      : _director.getRoadView().getRoadGroup();
    _cameraRig.reset(roadGroup);
  }
}

/** 重置 Map 视角 */
export function resetMapView() {
  resetCamera();
}

/** orbit 预览：切换左键动作（rotate/pan），键盘快捷键用（见 mapPreview.js） */
export function setOrbitLeftAction(action) {
  if (_cameraRig) return _cameraRig.setOrbitLeftAction(action);
  return 'rotate';
}

/** 同步档位到 store（视图可读：TrajectoryView 低档跳过装饰性加法混合层） */
function _syncPerfTier() {
  if (_director) _director.getStore().perfTier = _perfTier;
}

/** 设置性能档位 */
export function setPerfTier(tier) {
  if (!tier || !['low', 'medium', 'high', 'ultra'].includes(tier)) return;
  _perfTier = tier;
  if (_perfMonitor) _perfMonitor.pause();   // 手动设档后暂停自动降级，避免被覆盖
  _applyPerfTier(tier);
  _syncPerfTier();

  /* 手动设档后重置分辨率，避免曾降到 ultra 的低分辨率残留 */
  if (!_renderer) return;
  if (tier === 'ultra') setResolutionScale(_renderer, 0.5);
  else setResolutionScale(_renderer, 1);
}

/* 应用性能档位到渲染器/灯光/后处理。
 * 关键：low/ultra 档「禁用 composer 直接渲染」—— 这是真正的降级，
 * 之前只关阴影/降 DPR 却仍每帧跑 GTAO+Bloom+SMAA，等于没降。 */
function _applyPerfTier(tier) {
  if (!_renderer || !_lights || !_lights.sun) return;

  const isLow = tier === 'low';
  const isUltra = tier === 'ultra';
  const isMedium = tier === 'medium';
  const noPost = isLow || isUltra;   // 禁用整个后处理管线

  /* 阴影：low/ultra 关，medium 2048，high 4096 */
  _renderer.shadowMap.enabled = !noPost;
  const shadowSize = isMedium ? 2048 : 4096;
  const sun = _lights.sun;
  if (sun.shadow.mapSize.x !== shadowSize) {
    sun.shadow.mapSize.set(shadowSize, shadowSize);
    if (sun.shadow.map) sun.shadow.map.dispose();
    sun.shadow.map = null;   // 强制重建阴影贴图
  }

  /* DPR：low/ultra=1，medium/high=min(dpr,1.5) */
  _renderer.setPixelRatio(noPost ? 1 : Math.min(window.devicePixelRatio, 1.5));

  /* ultra 额外压低渲染分辨率（0.5x），CSS 拉伸到全屏 —— 最后兜底 */
  if (isUltra) setResolutionScale(_renderer, 0.5);

  /* 后处理：low/ultra 禁用整个 composer（直接渲染），medium/BEV 关 GTAO
   * 保留 Bloom+SMAA（见 _syncPostProc） */
  _syncPostProc();
}

/* ── PHM 降级/上报回调（由 PerfMonitor watchdog 驱动）──
 * watchdog 用独立 setInterval，即使 rAF 被 GPU 卡死仍能采样降级。 */

/** 自动降级：连续 3 个 1s 窗口 <30fps → 降一档（high→medium→low→ultra） */
function _onPhmDowngrade(fps) {
  const next = _perfTier === 'high' ? 'medium'
            : _perfTier === 'medium' ? 'low'
            : _perfTier === 'low' ? 'ultra' : null;
  if (!next) return;   // 已到最低档 ultra，不再降
  _perfTier = next;
  _applyPerfTier(next);
  _syncPerfTier();
  console.warn(`[vis] PHM: FPS ${fps.toFixed(0)} < 30 for 3s — downgraded to '${next}'`);
}

/** 上报可视化健康到后端（POST /api/vis/health），每 5s 节流一次 */
function _onPhmReport(stats) {
  const now = performance.now();
  if (now - _lastReportTs < 5000) return;
  _lastReportTs = now;
  try {
    const body = JSON.stringify({
      fps: Math.round(stats.fps),
      tier: _perfTier,
      drawCalls: stats.drawCalls,
      jank: stats.jank,
      ts: Date.now(),
    });
    fetch('/api/vis/health', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body,
      keepalive: true,   // 页面关闭时仍尽量送达
    }).catch(() => { /* 后端不可达时静默，不影响渲染 */ });
  } catch (e) { /* 忽略 */ }
}

/** 调试相机（占位，Phase 3 实现） */
export function setDebugCam(mode) {
  setCameraMode(mode);
}

/** 关闭 NPC 详情面板（占位，Phase 3 实现） */
export function closeNPCDetail() {
  // Phase 3 VehicleView 实现 NPC 详情面板后补全
}

/** 切换性能监控面板可见性 */
export function togglePerfOverlay() {
  if (!_statsView) return false;
  const el = _statsView.dom;
  el.style.display = el.style.display === 'none' ? '' : 'none';
  return el.style.display !== 'none';
}
/** 切换小地图可见性 */
export function toggleMinimap() {
  if (!_minimap) return false;
  _minimap.toggle();
  return _minimap.isVisible();
}
