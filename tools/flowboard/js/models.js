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

/* ── 静态城市模型（建筑等）────────────────────────────────────────
 * 与车辆模型不同：城市 glTF 是静态装饰件，不做轮轴/车灯适配，也不需要
 * clone() 时深拷贝材质给每辆车独立改色。按名字缓存一个原型，getCityModel
 * 返回 clone 后的 Group 供 BuildingView 沿路摆放。
 *
 * 资产来源：Quaternius Downtown City MegaKit (CC0)，放置到
 * tools/flowboard/models/city/。文件缺失时静默回退到程序化 Box 建筑
 * （BuildingView 现有实现），保证无外网资产时行为不变。 */
const CITY_MODEL_VERSION = '20260811downtown-v1';
const CITY_MODEL_SOURCES = {
  city_a: ['/tools/flowboard/models/city/Building_Small_1.gltf?v=' + CITY_MODEL_VERSION],
  city_b: ['/tools/flowboard/models/city/Building_Medium_2_001.gltf?v=' + CITY_MODEL_VERSION],
  city_c: ['/tools/flowboard/models/city/Building_Large_2.gltf?v=' + CITY_MODEL_VERSION]
};
const _cityCache = {};
let _cityReady = false;

/**
 * 预加载城市建筑 glTF。任一文件缺失只 warn，不阻断；对应缓存位保持 null，
 * 供 BuildingView 回退到程序化几何。
 */
export function initCityModelCache() {
  if (_cityReady) return Promise.resolve();
  if (window._gltfLoaderUnavailable || !THREE || !THREE.GLTFLoader) {
    _cityReady = true;
    return Promise.resolve();
  }
  return new Promise(function(resolve) {
    var loader = new THREE.GLTFLoader();
    if (THREE.MeshoptDecoder && loader.setMeshoptDecoder) {
      loader.setMeshoptDecoder(THREE.MeshoptDecoder);
    }
    var names = Object.keys(CITY_MODEL_SOURCES);
    var pending = names.length;
    function done(name, gltfOrNull) {
      _cityCache[name] = gltfOrNull ? gltfOrNull.scene : null;
      pending--;
      if (pending <= 0) { _cityReady = true; resolve(); }
    }
    for (var i = 0; i < names.length; i++) {
      (function(name) {
        _cityCache[name] = null;
        loader.load(
          CITY_MODEL_SOURCES[name][0],
          function(g) { done(name, g); },
          undefined,
          function(err) {
            console.warn('[models] city model "' + name + '" unavailable (' +
              (err && err.message ? err.message : err) + ') — BuildingView falls back to programmatic boxes');
            done(name, null);
          }
        );
      })(names[i]);
    }
  });
}

/**
 * 取一个城市建筑模型的克隆实例（供沿路摆放）。
 * @param {string} name  'city_a' | 'city_b' | 'city_c'
 * @returns {THREE.Group|null}  模型缺失返回 null，调用方回退程序化几何
 */
export function getCityModel(name) {
  var proto = _cityCache[name];
  return proto ? proto.clone() : null;
}

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
  // 授权 sm_car 的前轴是"左右轮合并成单个网格"。整体偏转这样的网格会让两个
  // 前轮一起绕轴心平移（掉头满舵下各 ~0.47m 脱离轮拱 = 车轮"漂移"）。真实前轮
  // 绕各自 kingpin 原地转向，故把前轴合并网格拆成左右两个独立轮，供 VehicleView
  // 用单轮 rotation.y 做原地转向。生成式 su7.gltf 已是独立 wheel_FL/FR，不触发。
  var frontAxle = null;
  group.traverse(function(c) { if (!frontAxle && c.name === 'axle_front') frontAxle = c; });
  if (frontAxle) _splitMergedFrontAxle(frontAxle);
}

/**
 * 把"左右轮合并成单个网格"的前轴拆成两个独立车轮（wheel_FL / wheel_FR）。
 * 每个轮几何按 z 重定心到自身轮心原点，挂回 axle 并在 z=±轮心 处定位 —— 这样
 * VehicleView 用 rotation.y 转向时是绕各自轮心（kingpin）原地转，轮心不动、不漂移。
 * 若该轴本来就是独立双轮（生成式模型），单个轮网格 z 跨度 < 0.2m，直接返回 null。
 * @returns {Array|null}  [leftMesh, rightMesh] 或 null
 */
function _splitMergedFrontAxle(axle) {
  if (!axle || !axle.children) return null;
  var merged = null;
  axle.children.forEach(function(c) { if (!merged && c.isMesh) merged = c; });
  if (!merged || !merged.geometry) return null;

  var geo = merged.geometry;
  var pos = geo.getAttribute('position');
  if (!pos || pos.count < 3) return null;

  var minZ = Infinity, maxZ = -Infinity;
  for (var i = 0; i < pos.count; i++) {
    var z = pos.getZ(i);
    if (z < minZ) minZ = z;
    if (z > maxZ) maxZ = z;
  }
  if (!isFinite(minZ) || !isFinite(maxZ) || (maxZ - minZ) < 0.2) return null; // 非双轮合并

  var midZ = (minZ + maxZ) / 2;          // 左右轮分界
  var leftCenter = (minZ + midZ) / 2;    // 左轮中心 z
  var rightCenter = (midZ + maxZ) / 2;   // 右轮中心 z

  var index = geo.index ? geo.index.array : null;
  var normal = geo.getAttribute('normal');
  var uv = geo.getAttribute('uv');
  var nTri = Math.floor(index ? index.length / 3 : pos.count / 3);

  function triVert(i, k) { return index ? index[i * 3 + k] : i * 3 + k; }

  // 按面片质心 z 归属左右两侧；顶点跨侧共享时各侧复制一份（轮左右几何独立）。
  var leftVerts = [], rightVerts = [];
  var leftMap = Object.create(null), rightMap = Object.create(null);
  var leftIdx = [], rightIdx = [];

  for (var t = 0; t < nTri; t++) {
    var zs = 0;
    for (var k = 0; k < 3; k++) zs += pos.getZ(triVert(t, k));
    var right = (zs / 3) >= midZ;
    var verts = right ? rightVerts : leftVerts;
    var map = right ? rightMap : leftMap;
    var outIdx = right ? rightIdx : leftIdx;
    for (var k2 = 0; k2 < 3; k2++) {
      var vi = triVert(t, k2);
      var oi = map[vi];
      if (oi === undefined) {
        oi = verts.length;
        map[vi] = oi;
        verts.push(vi);
      }
      outIdx.push(oi);
    }
  }
  if (leftVerts.length < 6 || rightVerts.length < 6) return null;

  function buildHalf(verts, idx, centerZ) {
    var positions = new Float32Array(verts.length * 3);
    var normals = normal ? new Float32Array(verts.length * 3) : null;
    var uvs = uv ? new Float32Array(verts.length * 2) : null;
    for (var i = 0; i < verts.length; i++) {
      var v = verts[i];
      positions[i * 3] = pos.getX(v);
      positions[i * 3 + 1] = pos.getY(v);
      positions[i * 3 + 2] = pos.getZ(v) - centerZ;   // 重定心到轮心
      if (normals) { normals[i * 3] = normal.getX(v); normals[i * 3 + 1] = normal.getY(v); normals[i * 3 + 2] = normal.getZ(v); }
      if (uvs) { uvs[i * 2] = uv.getX(v); uvs[i * 2 + 1] = uv.getY(v); }
    }
    var g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.BufferAttribute(positions, 3));
    if (normals) g.setAttribute('normal', new THREE.BufferAttribute(normals, 3));
    if (uvs) g.setAttribute('uv', new THREE.BufferAttribute(uvs, 2));
    g.setIndex(new THREE.BufferAttribute(new Uint32Array(idx), 1));
    return g;
  }

  var leftGeo = buildHalf(leftVerts, leftIdx, leftCenter);
  var rightGeo = buildHalf(rightVerts, rightIdx, rightCenter);

  var mat = Array.isArray(merged.material) ? merged.material[0] : merged.material;
  var leftMesh = new THREE.Mesh(leftGeo, mat);
  var rightMesh = new THREE.Mesh(rightGeo, mat);
  leftMesh.name = 'wheel_FL';
  rightMesh.name = 'wheel_FR';
  leftMesh.position.z = leftCenter;
  rightMesh.position.z = rightCenter;
  leftMesh.userData.rollAxis = 'z';
  rightMesh.userData.rollAxis = 'z';
  leftMesh.userData.wheelAxle = 'front';
  rightMesh.userData.wheelAxle = 'front';
  leftMesh.castShadow = merged.castShadow;
  rightMesh.castShadow = merged.castShadow;

  axle.remove(merged);
  merged.geometry.dispose();
  axle.add(leftMesh);
  axle.add(rightMesh);
  return [leftMesh, rightMesh];
}

/**
 * The authorized SU7 asset uses real light meshes instead of the generated
 * semantic names. Reuse those meshes for headlights and brake lights, while
 * splitting their authored triangles into left/right material groups. The
 * source asset combines both sides into one mesh, so material-side grouping is
 * the only way to keep the signal on the actual lamp geometry without adding
 * floating overlays.
 */
function _splitSu7LampGeometry(mesh) {
  if (!mesh || !mesh.geometry || !mesh.geometry.getAttribute ||
      !mesh.geometry.index || !mesh.geometry.index.array) return;
  if (mesh.userData && mesh.userData.su7LampSideSplit) return;

  var geometry = mesh.geometry;
  var position = geometry.getAttribute('position');
  var sourceIndex = geometry.index;
  if (!position || !sourceIndex || sourceIndex.count < 3) return;

  var sourceMaterials = Array.isArray(mesh.material) ? mesh.material : [mesh.material];
  var materialCount = sourceMaterials.length || 1;
  var groups = geometry.groups && geometry.groups.length
    ? geometry.groups
    : [{ start: 0, count: sourceIndex.count, materialIndex: 0 }];
  var sideIndices = [[], []];
  for (var side = 0; side < 2; side++) {
    sideIndices[side] = [];
    for (var materialIndex = 0; materialIndex < materialCount; materialIndex++) {
      sideIndices[side].push([]);
    }
  }

  var indices = sourceIndex.array;
  for (var gi = 0; gi < groups.length; gi++) {
    var group = groups[gi];
    var materialIndex = Math.max(0, Math.min(
      materialCount - 1, group.materialIndex || 0
    ));
    var end = Math.min(sourceIndex.count, group.start + group.count);
    for (var offset = group.start; offset + 2 < end; offset += 3) {
      var ia = indices[offset];
      var ib = indices[offset + 1];
      var ic = indices[offset + 2];
      var centroidZ = (
        position.getZ(ia) + position.getZ(ib) + position.getZ(ic)
      ) / 3;
      var side = centroidZ < 0 ? 0 : 1;  // THREE: -Z left, +Z right.
      sideIndices[side][materialIndex].push(ia, ib, ic);
    }
  }

  var leftCount = sideIndices[0].reduce(function(total, list) {
    return total + list.length;
  }, 0);
  var rightCount = sideIndices[1].reduce(function(total, list) {
    return total + list.length;
  }, 0);
  if (!leftCount || !rightCount) return;

  var reordered = [];
  var outputGroups = [];
  var outputOffset = 0;
  for (var outputSide = 0; outputSide < 2; outputSide++) {
    for (var outputMaterial = 0; outputMaterial < materialCount; outputMaterial++) {
      var sideList = sideIndices[outputSide][outputMaterial];
      if (!sideList.length) continue;
      reordered.push.apply(reordered, sideList);
      outputGroups.push({
        start: outputOffset,
        count: sideList.length,
        materialIndex: outputSide * materialCount + outputMaterial,
      });
      outputOffset += sideList.length;
    }
  }

  var IndexArray = sourceIndex.array.constructor;
  geometry.setIndex(new THREE.BufferAttribute(new IndexArray(reordered), 1));
  geometry.clearGroups();
  for (var og = 0; og < outputGroups.length; og++) {
    var outputGroup = outputGroups[og];
    geometry.addGroup(
      outputGroup.start,
      outputGroup.count,
      outputGroup.materialIndex
    );
  }

  var leftMaterials = [];
  var rightMaterials = [];
  for (var mi = 0; mi < materialCount; mi++) {
    var sourceMaterial = sourceMaterials[mi];
    leftMaterials.push(sourceMaterial && sourceMaterial.clone
      ? sourceMaterial.clone() : sourceMaterial);
    rightMaterials.push(sourceMaterial && sourceMaterial.clone
      ? sourceMaterial.clone() : sourceMaterial);
  }
  mesh.material = leftMaterials.concat(rightMaterials);
  if (!mesh.userData) mesh.userData = {};
  mesh.userData.su7LampSideSplit = true;
  mesh.userData.su7LampMaterialCount = materialCount;
}

function _lampMaterialsBySide(mesh) {
  if (!mesh || !mesh.userData || !mesh.userData.su7LampSideSplit) return null;
  var materials = Array.isArray(mesh.material) ? mesh.material : [];
  var materialCount = mesh.userData.su7LampMaterialCount || (materials.length / 2);
  if (!materialCount || materials.length < materialCount * 2) return null;
  return {
    left: materials.slice(0, materialCount),
    right: materials.slice(materialCount, materialCount * 2),
  };
}

function _isSu7EmissiveMaterial(material) {
  if (!material) return false;
  var name = (material.name || '').toLowerCase();
  // 严格排除非发光件：枪灰反光碗(M_iron)、熏黑灯腔(M_body_smoothblack)、玻璃(Car_window)与车漆
  if (name.indexOf('iron') >= 0 ||
      name.indexOf('black') >= 0 ||
      name.indexOf('window') >= 0 ||
      name.indexOf('body') >= 0 ||
      name.indexOf('chepai') >= 0) {
    return false;
  }
  return name.indexOf('car_ight') >= 0 ||
    name.indexOf('light') >= 0 ||
    name.indexOf('lamp') >= 0 ||
    (!name && !!material.emissive);
}

function _prepareSu7EmissiveMaterial(material) {
  if (!_isSu7EmissiveMaterial(material)) return false;
  /* sm_car 的 Car_ight 对应 SU7 水滴大灯内部四分区十字/"米"字导光条与 4 透镜矩阵 LED。
   * 保留原生基础漫反射贴图（baseColorTexture）与微棱镜漫射特性，仅清除非受控的静态遮罩。 */
  if (material.emissiveMap) {
    material.emissiveMap = null;
    material.needsUpdate = true;
  }
  if (material.transparent !== undefined) material.transparent = true;
  if (material.depthWrite !== undefined) material.depthWrite = false;
  return true;
}

function _normalizeSu7NodeName(name) {
  return String(name || '').toLowerCase().replace(/[._\-\s]/g, '');
}

function _nodeMatchesAny(name, targetNames) {
  var norm = _normalizeSu7NodeName(name);
  for (var i = 0; i < targetNames.length; i++) {
    var t = targetNames[i];
    if (norm === t) return true;
    if (t.length >= 6 && norm.indexOf(t) >= 0) return true;
  }
  return false;
}

function _matchSu7NodeOrAncestor(mesh, targetNames) {
  if (!mesh) return false;
  if (_nodeMatchesAny(mesh.name, targetNames)) return true;
  var parent = mesh.parent;
  while (parent) {
    if (_nodeMatchesAny(parent.name, targetNames)) return true;
    parent = parent.parent;
  }
  return false;
}

function _adaptSu7LightNodes(group) {
  var front = [], rear = [];
  function prepare(mesh, kind) {
    if (!mesh.userData) mesh.userData = {};
    _splitSu7LampGeometry(mesh);
    mesh.userData.su7LightKind = kind;
    var emissiveMaterials = [];
    var materials = Array.isArray(mesh.material) ? mesh.material : [mesh.material];
    materials.forEach(function(material) {
      if (!material) return;
      var matName = (material.name || '').toLowerCase();
      if (_prepareSu7EmissiveMaterial(material)) {
        emissiveMaterials.push(material);
        if (THREE.DoubleSide !== undefined) material.side = THREE.DoubleSide;
        if (material.toneMapped !== undefined) material.toneMapped = false;
      } else if (matName.indexOf('iron') >= 0) {
        // SU7 水滴大灯 4 透镜枪灰色金属反光碗与光学固定支架
        if (material.metalness !== undefined) material.metalness = 0.85;
        if (material.roughness !== undefined) material.roughness = 0.18;
        if (material.emissive) material.emissive.setHex(0x000000);
        if (material.emissiveIntensity !== undefined) material.emissiveIntensity = 0.0;
      } else if (matName.indexOf('black') >= 0) {
        // ADB 矩阵深邃熏黑灯腔基座
        if (material.metalness !== undefined) material.metalness = 0.25;
        if (material.roughness !== undefined) material.roughness = 0.25;
        if (material.emissive) material.emissive.setHex(0x000000);
        if (material.emissiveIntensity !== undefined) material.emissiveIntensity = 0.0;
      }
    });
    if (mesh.renderOrder !== undefined) mesh.renderOrder = 11;
    mesh.userData.su7LampMaterials = emissiveMaterials;
    mesh.userData.su7LampSideMaterials = _lampMaterialsBySide(mesh);
  }

  function prepareGlass(mesh, isRear) {
    if (!mesh) return;
    if (mesh.renderOrder !== undefined) mesh.renderOrder = 10;
    var materials = Array.isArray(mesh.material) ? mesh.material : [mesh.material];
    materials.forEach(function(mat) {
      if (!mat) return;
      // 真实透光聚碳酸酯水滴灯罩玻璃（前透明高光纯白，后烟熏红晶），不发光但折射透光
      if (mat.transparent !== undefined) mat.transparent = true;
      if (mat.opacity !== undefined) mat.opacity = isRear ? 0.35 : 0.18;
      if (mat.roughness !== undefined) mat.roughness = 0.04;
      if (mat.metalness !== undefined) mat.metalness = 0.06;
      if (mat.clearcoat !== undefined) mat.clearcoat = 1.0;
      if (mat.clearcoatRoughness !== undefined) mat.clearcoatRoughness = 0.04;
      if (mat.depthWrite !== undefined) mat.depthWrite = false;
      if (mat.depthTest !== undefined) mat.depthTest = true;
      if (THREE.DoubleSide !== undefined) mat.side = THREE.DoubleSide;
      if (mat.color && !isRear) mat.color.setHex(0xffffff);
      if (mat.emissive) mat.emissive.setHex(0x000000);
      if (mat.emissiveIntensity !== undefined) mat.emissiveIntensity = 0.0;
    });
  }

  group.traverse(function(c) {
    if (!c.isMesh) return;
    var isFrontGlass = _matchSu7NodeOrAncestor(c, ['lightglass004']);
    var isRearGlass = !isFrontGlass && _matchSu7NodeOrAncestor(c, ['lightglass']);

    if (isFrontGlass) {
      prepareGlass(c, false);
      return;
    }
    if (isRearGlass) {
      prepareGlass(c, true);
      return;
    }

    var isRear = _matchSu7NodeOrAncestor(c, ['light003', 'brakelight']);
    var isFront = !isRear && _matchSu7NodeOrAncestor(c, ['light', 'light002', 'headlight']);

    if (isFront) {
      front.push(c);
      prepare(c, 'head');
    } else if (isRear) {
      rear.push(c);
      prepare(c, 'brake');
    }
  });

  // SU7 后扩散器中央后雾灯（GB 4785 强制国标高穿透红色后雾灯）
  if (THREE && THREE.PlaneGeometry && THREE.MeshBasicMaterial && typeof group.add === 'function') {
    var fogGeo = new THREE.PlaneGeometry(0.24, 0.08);
    var fogMat = new THREE.MeshBasicMaterial({
      color: 0xff0000,
      transparent: true,
      opacity: 0.95,
      side: THREE.DoubleSide,
      depthWrite: false,
    });
    var fogMesh = new THREE.Mesh(fogGeo, fogMat);
    fogMesh.rotation.y = Math.PI / 2;
    fogMesh.position.set(-2.55, 0.33, 0.0);
    fogMesh.renderOrder = 12;
    fogMesh.visible = false;
    group.add(fogMesh);
    group.userData.rearFogLight = fogMesh;
  }

  if (front.length) group.userData.headlights = front;
  if (rear.length) group.userData.brakeLights = rear;
  group.userData.su7RawLights = front.length > 0 && rear.length > 0
    ? { front: front, rear: rear }
    : null;
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

function _getSu7LampMaterials(mesh) {
  if (!mesh) return [];
  var mats = Array.isArray(mesh.material) ? mesh.material : (mesh.material ? [mesh.material] : []);
  var emissive = [];
  for (var i = 0; i < mats.length; i++) {
    var m = mats[i];
    if (m && _prepareSu7EmissiveMaterial(m)) {
      emissive.push(m);
    }
  }
  return emissive;
}

function _setSu7LightMeshMaterials(meshes, intensity, colorHex) {
  for (var i = 0; i < meshes.length; i++) {
    var mesh = meshes[i];
    if (!mesh) continue;
    mesh.visible = true;
    var materials = _getSu7LampMaterials(mesh);
    if (!materials.length && mesh.material) {
      materials = Array.isArray(mesh.material) ? mesh.material : [mesh.material];
    }
    for (var j = 0; j < materials.length; j++) {
      var material = materials[j];
      if (!_prepareSu7EmissiveMaterial(material)) continue;
      if (material.emissive) material.emissive.setHex(colorHex);
      if (material.emissiveIntensity !== undefined) {
        material.emissiveIntensity = intensity;
      }
      if (material.toneMapped !== undefined) material.toneMapped = false;
      if (THREE.DoubleSide !== undefined && material.side !== undefined) {
        material.side = THREE.DoubleSide;
      }
    }
  }
}

function _setSu7LightMeshSideMaterials(meshes, side, intensity, colorHex) {
  for (var i = 0; i < meshes.length; i++) {
    var mesh = meshes[i];
    if (!mesh) continue;
    mesh.visible = true;
    var sideMaterials = _lampMaterialsBySide(mesh);
    var materials = sideMaterials && sideMaterials[side];
    if (!materials || !materials.length) {
      _setSu7LightMeshMaterials([mesh], intensity, colorHex);
      continue;
    }
    for (var j = 0; j < materials.length; j++) {
      var material = materials[j];
      if (!_prepareSu7EmissiveMaterial(material)) continue;
      if (material.emissive) material.emissive.setHex(colorHex);
      if (material.emissiveIntensity !== undefined) {
        material.emissiveIntensity = intensity;
      }
      if (material.toneMapped !== undefined) material.toneMapped = false;
      if (THREE.DoubleSide !== undefined && material.side !== undefined) {
        material.side = THREE.DoubleSide;
      }
    }
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
  // 克隆 material（每个实例独立，支持数组材质）
  group.traverse(function(c) {
    if (c.isMesh && c.material) {
      if (Array.isArray(c.material)) {
        c.material = c.material.map(function(m) {
          return m && m.clone ? m.clone() : m;
        });
      } else if (c.material.clone) {
        c.material = c.material.clone();
      }
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
  /* The authorized SU7 has combined front/rear lamp meshes, not four
   * semantic turn-signal nodes. The mesh is grouped by its authored local Z
   * coordinate, so a turn signal changes only the corresponding real lamp
   * geometry instead of painting a floating rectangular overlay. */
  // 车尾后雾灯控制（独立后雾灯节点）
  if (ud.rearFogLight) {
    ud.rearFogLight.visible = !!state.fog;
  }

  if (ud.su7RawLights) {
    // 基础前大灯与日行灯状态（未转向的一侧保持此状态）：
    // - 近光大灯开启：7.5 强悍高亮暖白矩阵光束（视觉对比度极高）
    // - 示廓灯/示宽灯开启：2.5 晶莹轮廓光
    // - 昼间行车（熄灯默认）：0.6 柔和水滴十字日行灯微光（不遮盖大灯开启效果）
    var baseFrontIntensity = state.head ? 7.5 : (state.clearance ? 2.5 : 0.6);
    var baseFrontColor = state.head ? 0xfffaee : 0xffffff;

    var isRearOn = !!(state.brake || state.clearance || state.head || state.fog);
    var baseRearIntensity = (state.brake && state.fog) ? 9.0 : (state.fog ? 8.0 : (state.brake ? 5.5 : (isRearOn ? 2.5 : 0.6)));
    var baseRearColor = state.fog ? 0xff0000 : 0xff211c;

    // 先重置两侧为基础灯光
    _setSu7LightMeshSideMaterials(ud.su7RawLights.front, 'left', baseFrontIntensity, baseFrontColor);
    _setSu7LightMeshSideMaterials(ud.su7RawLights.front, 'right', baseFrontIntensity, baseFrontColor);
    _setSu7LightMeshSideMaterials(ud.su7RawLights.rear, 'left', baseRearIntensity, baseRearColor);
    _setSu7LightMeshSideMaterials(ud.su7RawLights.rear, 'right', baseRearIntensity, baseRearColor);

    // 左侧转向灯激活：亮闪 6.5 琥珀金，暗闪 0.05（日行灯避让闪烁，醒目分明）
    if (state.turnL) {
      var turnLIntensity = blinkOn ? 6.5 : 0.05;
      var turnLColor = 0xff9000;
      _setSu7LightMeshSideMaterials(ud.su7RawLights.front, 'left', turnLIntensity, turnLColor);
      _setSu7LightMeshSideMaterials(ud.su7RawLights.rear, 'left', turnLIntensity, turnLColor);
    }

    // 右侧转向灯激活：亮闪 6.5 琥珀金，暗闪 0.05（日行灯避让闪烁，醒目分明）
    if (state.turnR) {
      var turnRIntensity = blinkOn ? 6.5 : 0.05;
      var turnRColor = 0xff9000;
      _setSu7LightMeshSideMaterials(ud.su7RawLights.front, 'right', turnRIntensity, turnRColor);
      _setSu7LightMeshSideMaterials(ud.su7RawLights.rear, 'right', turnRIntensity, turnRColor);
    }
  } else {
    // Keep the source lens and texture, but make the state change unambiguous
    // against the bright outdoor HDRI and the renderer's ACES exposure.
    if (ud.brakeLights) {
      var isRearOnGeneric = !!(state.brake || state.clearance || state.head || state.fog);
      var bi = (state.brake && state.fog) ? 10.0 : (state.fog ? 8.5 : (state.brake ? 6.0 : (isRearOnGeneric ? 2.2 : 0.08)));
      var rearCol = state.fog ? 0xff0000 : 0xff211c;
      _setLightMeshMaterials(ud.brakeLights, bi, rearCol);
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
    // 大灯 / 示廓灯
    if (ud.headlights) {
      var hi = state.head ? 8.0 : (state.clearance ? 2.5 : 0.05);
      _setLightMeshMaterials(ud.headlights, hi, 0xfff4cf);
    }
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
