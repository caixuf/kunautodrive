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
  emissiveIntensity: 1,
  toneMapped: true,
  side: 0,
};
const rearMaterial = {
  emissive: { setHex(value) { this.value = value; } },
  emissiveIntensity: 1,
  toneMapped: true,
  side: 0,
};
const rawFrontMesh = { name: 'Light', isMesh: true, material: frontMaterial };
const rawRearMesh = { name: 'Light.003', isMesh: true, material: rearMaterial };
const rawLightScene = {
  userData: { modelType: 'su7' },
  traverse(visitor) {
    [rawFrontMesh, rawRearMesh].forEach(visitor);
  },
};
_relinkWheelUserData(rawLightScene);
ok('SU7 真实灯材质接入 headlight/brakelight 契约',
  rawLightScene.userData.headlights.includes(rawFrontMesh) &&
  rawLightScene.userData.brakeLights.includes(rawRearMesh));
_setVehicleLights(rawLightScene, { brake: true, head: true }, 0);
ok('SU7 真实灯材质亮度和可见性增强',
  frontMaterial.emissiveIntensity === 8 &&
  rearMaterial.emissiveIntensity === 6 &&
  frontMaterial.toneMapped === false &&
  rearMaterial.toneMapped === false);
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
