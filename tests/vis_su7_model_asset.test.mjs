/**
 * vis_su7_model_asset.test.mjs — authorized SU7 asset integrity check.
 *
 * Keeps the external glTF/bin/WebP bundle complete so a static dashboard
 * deployment cannot silently fall back to the generated low-poly model.
 */

import { readFileSync, statSync } from 'fs';
import { resolve, dirname } from 'path';
import { fileURLToPath } from 'url';
import { ok, done } from './test-utils.mjs';
import { _cleanSu7Exterior } from '../tools/flowboard/js/vis/view/VehicleView.js';
import { _relinkWheelUserData, _setVehicleLights } from '../tools/flowboard/js/models.js';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../tools/flowboard/models/su7');
const modelPath = resolve(ROOT, 'sm_car.gltf');
const model = JSON.parse(readFileSync(modelPath, 'utf8'));
const referencedFiles = [
  ...(model.buffers || []).map(buffer => buffer.uri),
  ...(model.images || []).map(image => image.uri),
].filter(Boolean);

ok('SU7 glTF 文件存在', statSync(modelPath).size > 0);
ok('SU7 要求 Meshopt 和 WebP 扩展', [
  'EXT_meshopt_compression',
  'EXT_texture_webp',
].every(name => (model.extensionsRequired || []).includes(name)));
ok('SU7 轮轴节点存在', ['Wheel.001', 'Wheel.002'].every(name =>
  (model.nodes || []).some(node => node.name === name)));
const rawLightNodes = ['Light', 'Light.002', 'Light.003', 'LightGlass', 'LightGlass.004'];
ok('SU7 真实前后灯节点存在', rawLightNodes.every(name =>
  (model.nodes || []).some(node => node.name === name)));
const rawFrontLights = (model.nodes || []).filter(node =>
  ['Light', 'Light.002', 'LightGlass.004'].includes(node.name));
const rawRearLights = (model.nodes || []).filter(node =>
  ['Light.003', 'LightGlass'].includes(node.name));
ok('SU7 真实灯节点前后位置自洽',
  rawFrontLights.every(node => node.translation && node.translation[0] > 0) &&
  rawRearLights.every(node => node.translation && node.translation[0] < 0));
const frontMaterial = {
  emissive: { setHex(value) { this.value = value; } },
  emissiveMap: {},
  emissiveIntensity: 1,
  toneMapped: true,
  side: 0,
  depthTest: true,
  transparent: false,
  depthWrite: true,
};
const rearMaterial = {
  emissive: { setHex(value) { this.value = value; } },
  emissiveMap: {},
  emissiveIntensity: 1,
  toneMapped: true,
  side: 0,
  depthTest: true,
  transparent: false,
  depthWrite: true,
};
const glassMaterial = {
  name: 'Car_window',
  transparent: false,
  toneMapped: true,
  side: 0,
  depthWrite: true,
};
const rawFrontMesh = { name: 'Light', isMesh: true, material: frontMaterial, renderOrder: 0 };
const rawRearMesh = { name: 'Light.003', isMesh: true, material: rearMaterial, renderOrder: 0 };
const rawFrontGlassMesh = { name: 'LightGlass.004', isMesh: true, material: glassMaterial, renderOrder: 0 };
const sanitizedFrontNode = { name: 'Light', parent: null };
const sanitizedFrontMesh = {
  name: 'Object_14003',
  isMesh: true,
  material: frontMaterial,
  parent: sanitizedFrontNode,
};
const rawLightScene = {
  userData: { modelType: 'su7' },
  traverse(visitor) {
    [rawFrontMesh, sanitizedFrontMesh, rawRearMesh, rawFrontGlassMesh].forEach(visitor);
  },
};
_relinkWheelUserData(rawLightScene);
ok('SU7 真实灯材质接入 headlight/brakelight 契约',
  rawLightScene.userData.headlights.includes(rawFrontMesh) &&
  rawLightScene.userData.headlights.includes(sanitizedFrontMesh) &&
  rawLightScene.userData.brakeLights.includes(rawRearMesh));
_setVehicleLights(rawLightScene, { brake: true, head: true }, 0);
ok('SU7 真实灯材质亮度和可见性增强',
  frontMaterial.emissiveIntensity === 8 &&
  rearMaterial.emissiveIntensity === 6 &&
  frontMaterial.emissiveMap === null &&
  rearMaterial.emissiveMap === null &&
  rawFrontMesh.visible === true &&
  rawRearMesh.visible === true &&
  frontMaterial.toneMapped === false &&
  rearMaterial.toneMapped === false);
_setVehicleLights(rawLightScene, { brake: false, head: false, turnL: true }, 0.1);
ok('SU7 转向灯直接复用真实灯罩而非悬浮方块',
  rawLightScene.userData.su7RawLights &&
  frontMaterial.emissiveIntensity === 10 &&
  rearMaterial.emissiveIntensity === 10 &&
  frontMaterial.emissive.value === 0xffa21a &&
  rearMaterial.emissive.value === 0xffa21a);
// 修复：前灯 emissive 材质须穿透 25% 不透明深色灯罩玻璃（Light 在玻璃之后），
// 因此设 depthTest=false + transparent=true，并把灯网格 renderOrder 抬高到玻璃之上。
ok('SU7 前灯 emissive 穿透灯罩玻璃且不透视车身',
  // lamp 仍受车身深度遮挡（depthTest 保持 true）→ 不会从车头看到尾灯
  frontMaterial.depthTest === true &&
  frontMaterial.transparent === true &&
  frontMaterial.depthWrite === false &&
  rearMaterial.depthTest === true &&
  rearMaterial.transparent === true &&
  rearMaterial.depthWrite === false &&
  rawFrontMesh.renderOrder === 10 &&
  rawRearMesh.renderOrder === 10);
ok('SU7 灯罩玻璃不写深度（避免遮挡其后 emissive 灯）',
  rawFrontGlassMesh.renderOrder === 10 &&
  glassMaterial.depthWrite === false);
ok('SU7 clean exterior 节点存在', ['ChePai', 'Logo', 'Logo.001'].every(name =>
  (model.nodes || []).some(node => node.name === name)));
ok('SU7 外部 bin/WebP 资源完整', referencedFiles.length > 0 &&
  referencedFiles.every(file => statSync(resolve(ROOT, file)).size > 0));
ok('SU7 归属说明存在', statSync(resolve(ROOT, 'README.md')).size > 0);

const bodyMaterials = [
  { name: 'Car_body', aoMap: {}, aoMapIntensity: 4 },
  { name: 'M_body_smoothblack', aoMap: {}, aoMapIntensity: 4 },
];
const bodyMesh = { name: 'body', isMesh: true, material: bodyMaterials, visible: true };
const plate = { name: 'ChePai', visible: true };
const logo = { name: 'Logo001', visible: true };
const cleanScene = {
  traverse(visitor) {
    [bodyMesh, plate, logo].forEach(visitor);
  },
};
_cleanSu7Exterior(cleanScene);
ok('SU7 clean exterior 移除车牌/Logo/AO 贴花',
  bodyMaterials[0].aoMap === null &&
  bodyMaterials[1].aoMap !== null &&
  plate.visible === false &&
  logo.visible === false);

done();
