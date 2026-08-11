/**
 * Keeps generated NPC traffic recognizable but materially simpler than the ego SU7.
 */

import { readFileSync, statSync } from 'fs';
import { resolve, dirname } from 'path';
import { fileURLToPath } from 'url';
import { ok, done } from './test-utils.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../tools/flowboard/models');
const NPC_MODELS = {
  sedan: ['grille', 'bumper_front', 'side_window_L', 'side_window_R'],
  suv: ['grille', 'cladding_L', 'roof_rail_L', 'side_window_L'],
  truck: ['grille', 'bumper', 'mirror_L', 'cargo_rib_1'],
};
const LAMP_NODES = [
  'headlight_L', 'headlight_R',
  'brakelight_L', 'brakelight_R',
  'turnsignal_FL', 'turnsignal_FR', 'turnsignal_RL', 'turnsignal_RR',
];

for (const [modelName, requiredDetails] of Object.entries(NPC_MODELS)) {
  const modelPath = resolve(ROOT, `${modelName}.gltf`);
  const model = JSON.parse(readFileSync(modelPath, 'utf8'));
  const names = new Set((model.nodes || []).map(node => node.name));

  ok(`${modelName} glTF 文件存在`, statSync(modelPath).size > 0);
  ok(`${modelName} 保留车辆灯光语义`, LAMP_NODES.every(name => names.has(name)));
  ok(`${modelName} 具备车型辨识结构`, requiredDetails.every(name => names.has(name)));
  ok(`${modelName} 不带主车 ADS 标识`, ![...names].some(name =>
    name.toLowerCase().includes('ads_indicator')));
  ok(`${modelName} 保持背景车复杂度预算`, (model.meshes || []).length <= 32);
}

done();
