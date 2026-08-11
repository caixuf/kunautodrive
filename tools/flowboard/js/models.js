/**
 * models.js — glTF model cache for FlowBoard 3D scene
 *
 * Loads vehicle/pedestrian .gltf files via Three.js GLTFLoader.
 * Falls back to programmatic geometry when GLTFLoader is unavailable
 * or a model file fails to load.
 *
 * glTF 节点约定（gen_models.py 生成）：
 *   wheel_FL / wheel_FR / wheel_RL / wheel_RR  — 四轮节点
 *   body / cabin / cab / cargo / torso / head  — 车身/驾驶舱等
 *
 * 授权 SU7 资产（models/su7/sm_car.gltf）把左右轮合并为两个轮轴网格，
 * 本模块会在加载后包装成 axle_front/axle_rear，复用同一套动画契约。
 * 本模块从命名节点建立 userData.frontAxle / rearAxle / wheels，使 glTF 车辆
 * 与程序化 _buildSedan 一样支持前轴转向 + 全轮滚动动画。
 *
 * Usage:
 *   import { initModelCache, getModel } from './models.js';
 *   await initModelCache();
 *   const sedan = getModel('sedan').clone();
 *   scene.add(sedan);
 */

import { _buildSedan, _buildObstacle, _buildContactShadow } from './utils.js';

const THREE = window.THREE;

/** Model registry: { name: THREE.Group or null (fallback) } */
const _cache = {};

/** Loading state */
let _ready = false;

const MODEL_NAMES = ['sedan', 'su7', 'truck', 'suv', 'pedestrian'];
const MODEL_VERSION = '20260811su7';

/** Primary assets are tried first; generated models remain explicit fallbacks. */
const MODEL_SOURCES = {
  sedan: ['/tools/flowboard/models/sedan.gltf?v=' + MODEL_VERSION],
  su7: [
    '/tools/flowboard/models/su7/sm_car.gltf?v=' + MODEL_VERSION,
    '/tools/flowboard/models/su7.gltf?v=' + MODEL_VERSION
  ],
  truck: ['/tools/flowboard/models/truck.gltf?v=' + MODEL_VERSION],
  suv: ['/tools/flowboard/models/suv.gltf?v=' + MODEL_VERSION],
  pedestrian: ['/tools/flowboard/models/pedestrian.gltf?v=' + MODEL_VERSION]
};

/**
 * The source model has one mesh per axle (each mesh contains its left/right
 * wheel). Wrapping the mesh preserves its glTF transform and gives steering
 * and rolling independent pivots.
 */
function _wrapSu7WheelAsAxle(group, wheelNames, axleName, role) {
  var wheel = null;
  group.traverse(function(c) {
    if (!wheel && wheelNames.indexOf(c.name) >= 0) wheel = c;
  });
  if (!wheel || !wheel.parent || (wheel.parent && wheel.parent.name === axleName)) return;

  var parent = wheel.parent;
  var axle = new THREE.Group();
  axle.name = axleName;
  axle.position.copy(wheel.position);
  axle.quaternion.copy(wheel.quaternion);
  axle.scale.copy(wheel.scale);
  axle.userData.wheelAxle = role;
  parent.add(axle);
  axle.add(wheel);

  wheel.position.set(0, 0, 0);
  wheel.quaternion.identity();
  wheel.scale.set(1, 1, 1);
  wheel.userData.wheelAxle = role;
  wheel.userData.rollAxis = 'z';
}

function _adaptSu7WheelNodes(group) {
  // GLTFLoader sanitizes Blender's dot-suffixed names to Wheel001/Wheel002.
  _wrapSu7WheelAsAxle(group, ['Wheel.001', 'Wheel001'], 'axle_front', 'front');
  _wrapSu7WheelAsAxle(group, ['Wheel.002', 'Wheel002'], 'axle_rear', 'rear');
}

/**
 * The authorized SU7 asset uses real light meshes instead of the generated
 * semantic names. Reuse those meshes for headlights and brake lights; the
 * procedural overlay remains responsible only for side-specific indicators.
 */
function _adaptSu7LightNodes(group) {
  var front = [], rear = [];
  function prepare(mesh, kind) {
    if (!mesh.userData) mesh.userData = {};
    mesh.userData.su7LightKind = kind;
    var materials = Array.isArray(mesh.material) ? mesh.material : [mesh.material];
    materials.forEach(function(material) {
      if (!material) return;
      // The source lens normals are not guaranteed to face every camera angle.
      // Double-sided, non-tonemapped emission keeps the real lens visible
      // without replacing its geometry or emissive texture.
      if (THREE.DoubleSide !== undefined) material.side = THREE.DoubleSide;
      if (material.toneMapped !== undefined) material.toneMapped = false;
    });
  }
  group.traverse(function(c) {
    if (!c.isMesh) return;
    if (c.name === 'Light' ||
        c.name === 'Light.002' || c.name === 'Light002' ||
        c.name === 'LightGlass.004' || c.name === 'LightGlass004') {
      front.push(c);
      prepare(c, 'head');
    } else if (c.name === 'Light.003' || c.name === 'Light003' || c.name === 'LightGlass') {
      rear.push(c);
      prepare(c, 'brake');
    }
  });
  if (front.length) group.userData.headlights = front;
  if (rear.length) group.userData.brakeLights = rear;
  group.userData.su7RawLights = front.length > 0 && rear.length > 0;
}

function _setLightMeshMaterial(mesh, intensity, colorHex) {
  var materials = Array.isArray(mesh.material) ? mesh.material : [mesh.material];
  for (var m = 0; m < materials.length; m++) {
    var material = materials[m];
    if (!material) continue;
    if (material.emissive) material.emissive.setHex(colorHex);
    if (material.emissiveIntensity !== undefined) material.emissiveIntensity = intensity;
    // Keep the real SU7 lens readable above the ACES curve and from both
    // sides; semantic glTF lights use the same path when available.
    if (material.toneMapped !== undefined) material.toneMapped = false;
    if (THREE.DoubleSide !== undefined && material.side !== undefined) {
      material.side = THREE.DoubleSide;
    }
  }
}

function _setLightMeshMaterials(meshes, intensity, colorHex) {
  for (var i = 0; i < meshes.length; i++) {
    _setLightMeshMaterial(meshes[i], intensity, colorHex);
  }
}

/**
 * 从 glTF 场景建立带 userData 的车辆 Group。
 * - 保留节点层级与 name（便于按名查找车轮/轴）
 * - 克隆 material 使每个实例独立可改色
 *
 * 车辆转向系统已烘进 glTF 节点层级（gen_models.py）：
 *   axle_front / axle_rear — 带 translation 的 Group 节点（pivot 在轴心）
 *   wheel_FL/FR/RL/RR      — 挂在 axle 节点下，translation = 相对轴心偏移；
 *                            wheel 几何居中在原点（cylinder_vertices(0,0,0,...)）。
 * 因此这里只需按名查找节点并填入 userData，无需 reparent / 重算几何中心 ——
 * 转向 rotation.y 绕轴心自转（不再画弧线），滚动 rotation.z 绕轮轴自转。
 *
 * wheel.userData.rollAxis = 'z' 标记 GLTF 车轮的滚动轴，VehicleView.js 据此选
 * rotation.z（GLTF，cylinder axis=Z）或 rotation.x（程序化 _buildSedan，cylinder axis=X）。
 */
function _buildVehicleFromGltf(name, gltf) {
  var group = new THREE.Group();
  // 保留层级：把 gltf.scene 的顶层子节点克隆进来（保留 name 与 parent/child）
  gltf.scene.children.forEach(function(child) {
    group.add(child.clone());
  });
  // 克隆 material（每个实例独立）
  group.traverse(function(c) {
    if (c.isMesh && c.material) {
      c.material = c.material.clone();
    }
  });

  group.userData.isVehicle = true;
  group.userData.modelType = name;

  // 车辆类型才需要建立 wheel userData（行人无轮）
  if (name !== 'pedestrian') {
    if (name === 'su7') {
      _adaptSu7WheelNodes(group);
      _adaptSu7LightNodes(group);
    }

    var fl = null, fr = null, rl = null, rr = null;
    var fwGroup = null, rwGroup = null;
    var axleWheels = [];
    group.traverse(function(c) {
      var n = c.name;
      if (n === 'axle_front') fwGroup = c;
      else if (n === 'axle_rear') rwGroup = c;
      else if (n === 'wheel_FL') fl = c;
      else if (n === 'wheel_FR') fr = c;
      else if (n === 'wheel_RL') rl = c;
      else if (n === 'wheel_RR') rr = c;
      else if (c.userData && c.userData.wheelAxle) axleWheels.push(c);
    });
    // 标记 GLTF 车轮滚动轴 = Z（cylinder axis=Z），VehicleView.js 据此用 rotation.z 滚动。
    // 程序化 _buildSedan 的车轮没有此标记，默认走 rotation.x（cylinder axis=X）。
    [fl, fr, rl, rr].forEach(function(w) {
      if (w) {
        if (!w.userData) w.userData = {};
        w.userData.rollAxis = 'z';
      }
    });
    var wheels = [];
    // 转向系统已烘进 glTF 节点层级（见 gen_models.py / commit 162e3ea）：
    // axle_front / axle_rear 作为独立 Group 节点导出，无需运行时再用 Box3
    // 算 FL/FR 包围盒中心建 pivot。这里只需把 4 个 wheel 节点 push 进 wheels
    // 供滚动动画使用，并把 axle_front / axle_rear 直接挂到 userData 给
    // VehicleView.js 的转向逻辑读取。
    if (fl) wheels.push(fl);
    if (fr) wheels.push(fr);
    if (rl) wheels.push(rl);
    if (rr) wheels.push(rr);
    axleWheels.forEach(function(w) {
      if (w.userData.rollAxis !== 'z') w.userData.rollAxis = 'z';
      if (wheels.indexOf(w) < 0) wheels.push(w);
    });
    if (fwGroup) group.userData.frontAxle = fwGroup;
    if (rwGroup) group.userData.rearAxle = rwGroup;
    if (wheels.length) group.userData.wheels = wheels;
  }
  // 灯节点扫描：brakelight_L/R, turnsignal_FL/FR/RL/RR, headlight_L/R, ads_indicator_L/R。
  // VehicleView.js 通过 material.emissiveIntensity 切换亮灭（接感知/规划链路）。
  // ads_indicator（自动驾驶小蓝灯）由 _setVehicleLights 设为常亮。
  var brakeLights = [], turnSignals = {}, headlights = [], adsIndicators = [];
  group.traverse(function(c) {
    if (!c.isMesh) return;
    var n = c.name || '';
    if (n.indexOf('brakelight_') === 0) brakeLights.push(c);
    else if (n.indexOf('turnsignal_') === 0) {
      turnSignals[n.substring('turnsignal_'.length)] = c;  // FL/FR/RL/RR
    }
    else if (n.indexOf('headlight_') === 0) headlights.push(c);
    else if (n.indexOf('ads_indicator') === 0) adsIndicators.push(c);
  });
  if (brakeLights.length) group.userData.brakeLights = brakeLights;
  if (Object.keys(turnSignals).length) group.userData.turnSignals = turnSignals;
  if (headlights.length) group.userData.headlights = headlights;
  if (adsIndicators.length) group.userData.adsIndicators = adsIndicators;
  return group;
}

/**
 * Preload all glTF models from the model registry above.
 * If GLTFLoader is unavailable, all models stay null and getModel() returns fallback.
 * Returns a promise that resolves when all models are loaded or failed.
 */
export function initModelCache() {
  if (_ready) return Promise.resolve();
  if (window._gltfLoaderUnavailable) {
    _ready = true;
    return Promise.resolve();
  }
  if (!THREE || !THREE.GLTFLoader) {
    window._gltfLoaderUnavailable = true;
    _ready = true;
    return Promise.resolve();
  }

  return new Promise(function(resolve) {
    var loader = new THREE.GLTFLoader();
    if (THREE.MeshoptDecoder && loader.setMeshoptDecoder) {
      loader.setMeshoptDecoder(THREE.MeshoptDecoder);
    } else {
      console.warn('[models] MeshoptDecoder unavailable; the authorized SU7 asset will use its generated fallback');
    }
    var pending = MODEL_NAMES.length;

    function onModelLoaded(name, gltf) {
      _cache[name] = _buildVehicleFromGltf(name, gltf);
      pending--;
      if (pending <= 0) { _ready = true; resolve(); }
    }

    function onModelError(name, err) {
      console.warn('[models] ' + name + ' load failed: ' + (err.message || err) + ' — using programmatic fallback');
      _cache[name] = null;
      pending--;
      if (pending <= 0) { _ready = true; resolve(); }
    }

    function loadCandidate(name, index) {
      var sources = MODEL_SOURCES[name];
      var url = sources[index];
      loader.load(
        url,
        function(g) { onModelLoaded(name, g); },
        undefined,
        function(e) {
          if (index + 1 < sources.length) {
            console.warn('[models] ' + name + ' primary asset failed; trying fallback model');
            loadCandidate(name, index + 1);
          } else {
            onModelError(name, e);
          }
        }
      );
    }

    for (var i = 0; i < MODEL_NAMES.length; i++) {
      var name = MODEL_NAMES[i];
      _cache[name] = null;
      loadCandidate(name, 0);
    }
  });
}

/**
 * Get a cached model group by type name.
 * Returns a THREE.Group (clone before adding to scene).
 * If glTF model is unavailable, returns null (caller uses programmatic fallback).
 *
 * @param {string} type  'sedan', 'truck', 'suv', 'pedestrian', or undefined
 * @returns {THREE.Group|null}
 */
export function getModel(type) {
  var name = type || 'car';
  // Map type names to model names
  switch (name) {
    case 'car':    name = 'sedan'; break;
    case 'su7':    name = 'su7'; break;
    case 'truck':  name = 'truck'; break;
    case 'suv':    name = 'suv'; break;
    case 'pedestrian': name = 'pedestrian'; break;
    default:       name = 'sedan';
  }
  var model = _cache[name];
  if (model) {
    var clone = model.clone();
    // Object3D.clone() shares materials.  Give each vehicle its own material
    // set so light animation and per-instance paint upgrades cannot mutate the
    // cached prototype or dispose resources used by another vehicle.
    clone.traverse(function(c) {
      if (!c.isMesh || !c.material) return;
      if (Array.isArray(c.material)) {
        c.material = c.material.map(function(material) {
          return material && material.clone ? material.clone() : material;
        });
      } else if (c.material.clone) {
        c.material = c.material.clone();
      }
    });
    // Scale: glTF models are built in meters (1:1 with scene)
    // Reset any pre-applied transforms
    clone.scale.set(1, 1, 1);
    // clone() 不深拷贝 userData 中的 Group 引用，重建 frontAxle/rearAxle/wheels
    if (model.userData.frontAxle || model.userData.wheels) {
      _relinkWheelUserData(clone);
    }
    return clone;
  }
  return null;
}

/** clone() 后 userData.frontAxle / rearAxle / wheels 引用失效。
 *  clone 已保留完整层级（axle_front / axle_rear 含 wheel_* 子节点），
 *  仅需按名查找并重建 userData 引用，无需重建 Group。
 *  同时为新克隆的 wheel_* 节点重新打 rollAxis='z' 标记（GLTF cylinder axis=Z）。 */
export function _relinkWheelUserData(clone) {
  var fwGroup = null, rwGroup = null;
  var fl = null, fr = null, rl = null, rr = null;
  var axleWheels = [];
  clone.traverse(function(c) {
    var n = c.name;
    if (n === 'axle_front') fwGroup = c;
    else if (n === 'axle_rear') rwGroup = c;
    else if (n === 'wheel_FL') fl = c;
    else if (n === 'wheel_FR') fr = c;
    else if (n === 'wheel_RL') rl = c;
    else if (n === 'wheel_RR') rr = c;
    else if (c.userData && c.userData.wheelAxle) axleWheels.push(c);
  });
  // 为每个 wheel 节点重打 rollAxis='z'（与 _buildVehicleFromGltf 保持一致）
  [fl, fr, rl, rr].forEach(function(w) {
    if (w) {
      if (!w.userData) w.userData = {};
      w.userData.rollAxis = 'z';
    }
  });
  var wheels = [];
  if (fwGroup) { clone.userData.frontAxle = fwGroup; if (fl) wheels.push(fl); if (fr) wheels.push(fr); }
  if (rwGroup) { clone.userData.rearAxle = rwGroup; if (rl) wheels.push(rl); if (rr) wheels.push(rr); }
  if (!fwGroup && !rwGroup) { [fl, fr, rl, rr].forEach(function(w) { if (w) wheels.push(w); }); }
  axleWheels.forEach(function(w) {
    if (w.userData.rollAxis !== 'z') w.userData.rollAxis = 'z';
    if (wheels.indexOf(w) < 0) wheels.push(w);
  });
  if (wheels.length) clone.userData.wheels = wheels;
  if (clone.userData && clone.userData.modelType === 'su7') {
    _adaptSu7LightNodes(clone);
  }
  // 灯节点引用也需重建（与 _buildVehicleFromGltf 扫描范围保持一致）
  var brakeLights = [], turnSignals = {}, headlights = [], adsIndicators = [];
  clone.traverse(function(c) {
    if (!c.isMesh) return;
    var n = c.name || '';
    if (n.indexOf('brakelight_') === 0) brakeLights.push(c);
    else if (n.indexOf('turnsignal_') === 0) turnSignals[n.substring('turnsignal_'.length)] = c;
    else if (n.indexOf('headlight_') === 0) headlights.push(c);
    else if (n.indexOf('ads_indicator') === 0) adsIndicators.push(c);
  });
  if (brakeLights.length) clone.userData.brakeLights = brakeLights;
  if (Object.keys(turnSignals).length) clone.userData.turnSignals = turnSignals;
  if (headlights.length) clone.userData.headlights = headlights;
  if (adsIndicators.length) clone.userData.adsIndicators = adsIndicators;
}

/**
 * 将 glTF 车身的 PBR 材质升级为 MeshPhysicalMaterial（clearcoat 车漆），
 * 并整体涂色。仅改 body/cabin/cab/cargo/hood/trunklid/door_* / wiper_* 等
 * 车身件；跳过灯节点（brakelight_* / turnsignal_* / headlight_* 保留发光材质）、
 * 车轮（wheel_* 保持轮胎黑）和玻璃（windshield/rear_window 保持透明）。
 */
function _upgradeCarPaint(model, color) {
  var bodyNames = { body: 1, cabin: 1, cab: 1, cargo: 1, rear: 1, hood: 1, trunklid: 1,
                    door_FL: 1, door_FR: 1, door_RL: 1, door_RR: 1,
                    chargeport_cover: 1, wiper_L: 1, wiper_R: 1,
                    // Task 6: SU7 专属车身件（与 sedan 共用 body/cabin/hood/trunklid）
                    spoiler: 1, splitter: 1, lidar_bump: 1,
                    side_skirt_L: 1, side_skirt_R: 1 };
  var SKIP_PREFIXES = ['brakelight_', 'turnsignal_', 'headlight_', 'wheel_', 'ads_indicator'];
  var SKIP_NAMES = { windshield: 1, rear_window: 1, side_window_L: 1, side_window_R: 1 };
  function shouldSkip(name) {
    if (!name) return false;
    if (SKIP_NAMES[name]) return true;
    for (var i = 0; i < SKIP_PREFIXES.length; i++) {
      if (name.indexOf(SKIP_PREFIXES[i]) === 0) return true;
    }
    return false;
  }
  model.traverse(function(c) {
    if (!c.isMesh || !c.material) return;
    if (shouldSkip(c.name)) return;  // 灯/轮/玻璃：保留原材质
    if (c.name && bodyNames[c.name]) {
      // 升级为 MeshPhysicalMaterial 清漆车漆
      var oldMat = c.material;
      var newMat = new THREE.MeshPhysicalMaterial({
        color: color || (oldMat.color ? oldMat.color.getHex() : 0x4488dd),
        metalness: oldMat.metalness !== undefined ? oldMat.metalness : 0.15,
        roughness: oldMat.roughness !== undefined ? oldMat.roughness : 0.25,
        envMapIntensity: 1.1,
        clearcoat: 1.0, clearcoatRoughness: 0.07,
        sheenColor: new THREE.Color(0.4, 0.4, 0.4)
      });
      c.material = newMat;
    } else if (c.material && c.material.color) {
      // 其他未命名车身件：保留原材质，仅改色
      c.material.color.setHex(color);
    }
  });
}

/**
 * _setVehicleLights — 切换车辆灯光状态（接感知 / 规划链路）。
 * 通过修改 material.emissiveIntensity 控制亮灭，不重建材质。
 *
 * @param {THREE.Group} group   车辆 group（含 userData.brakeLights/turnSignals）
 * @param {object} state        { brake: bool, turnL: bool, turnR: bool, head: bool }
 * @param {number} blinkPhase   闪烁相位 0..1（转向灯 1.5Hz 闪烁，VehicleView.js 传入 _animT）
 */
export function _setVehicleLights(group, state, blinkPhase) {
  if (!group || !group.userData) return;
  var ud = group.userData;
  var blinkOn = (blinkPhase !== undefined) ? (Math.sin(blinkPhase * Math.PI * 2 * 1.5) > 0) : true;
  // Keep the source lens and texture, but make the state change unambiguous
  // against the bright outdoor HDRI and the renderer's ACES exposure.
  if (ud.brakeLights) {
    var bi = state.brake ? 6.0 : 0.08;
    _setLightMeshMaterials(ud.brakeLights, bi, 0xff211c);
  }
  // 转向灯：亮闪 6.0（更醒目），灭 0.05
  if (ud.turnSignals) {
    var ts = ud.turnSignals;
    var setSide = function(on, keys) {
      var intensity = on ? (blinkOn ? 6.0 : 0.05) : 0.05;
      for (var k = 0; k < keys.length; k++) {
        if (ts[keys[k]]) _setLightMeshMaterial(ts[keys[k]], intensity, 0xff8a18);
      }
    };
    setSide(state.turnL, ['FL', 'RL']);
    setSide(state.turnR, ['FR', 'RR']);
  }
  // 大灯：亮 8.0（补足白天 HDRI 下的视觉占比），灭 0.05
  if (ud.headlights && state.head !== undefined) {
    var hi = state.head ? 8.0 : 0.05;
    _setLightMeshMaterials(ud.headlights, hi, 0xfff4cf);
  }
  // 自动驾驶小蓝灯（ads_indicator）：车尾左右各一，量产 ADS 标志。
  // 常亮 + 高 emissive（穿透性蓝光），不受 brake/turn 状态影响。
  // 每次 _setVehicleLights 调用时强制覆盖，防止材质被重置而熄灭。
  if (ud.adsIndicators) {
    for (var ai = 0; ai < ud.adsIndicators.length; ai++) {
      if (ud.adsIndicators[ai].material) {
        ud.adsIndicators[ai].material.emissive.setHex(0x4488ff);
        ud.adsIndicators[ai].material.emissiveIntensity = 3.0;
      }
    }
  }
}
