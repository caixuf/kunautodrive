/**
 * vis_city_model_asset.test.mjs — Downtown City asset integrity and budget check.
 */

import { readFileSync, statSync } from 'fs';
import { resolve, dirname } from 'path';
import { fileURLToPath } from 'url';
import { ok, done } from './test-utils.mjs';
import { CITY_MODEL_BUDGET } from '../tools/flowboard/js/vis/view/BuildingView.js';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../tools/flowboard/models/city');
const MODEL_SOURCE = readFileSync(
  resolve(dirname(fileURLToPath(import.meta.url)), '../tools/flowboard/js/models.js'),
  'utf8'
);
const BUILDINGS = [
  'Building_Small_1',
  'Building_Medium_2_001',
  'Building_Large_2',
];

const referencedFiles = new Set();
for (const name of BUILDINGS) {
  const gltfPath = resolve(ROOT, name + '.gltf');
  ok(name + ' glTF exists', statSync(gltfPath).size > 0);
  const gltf = JSON.parse(readFileSync(gltfPath, 'utf8'));
  ok(name + ' is glTF 2.0', gltf.asset && gltf.asset.version === '2.0');
  ok(name + ' has bounded primitive count',
    (gltf.meshes || []).reduce((sum, mesh) => sum + (mesh.primitives || []).length, 0) <= 13);

  for (const buffer of gltf.buffers || []) {
    if (buffer.uri) {
      referencedFiles.add(buffer.uri);
      ok(name + ' buffer byteLength matches file',
        statSync(resolve(ROOT, buffer.uri)).size === buffer.byteLength);
    }
  }
  for (const image of gltf.images || []) {
    if (image.uri) referencedFiles.add(image.uri);
  }
}

for (const file of referencedFiles) {
  ok('city dependency exists: ' + file, statSync(resolve(ROOT, file)).size > 0);
}

const license = readFileSync(resolve(ROOT, 'LICENSE_DOWNTOWN_CITY.txt'), 'utf8');
ok('Downtown City CC0 license is retained', license.includes('CC0 1.0 Universal'));
ok('all selected Downtown models are wired into loader',
  BUILDINGS.every(name => MODEL_SOURCE.includes(name + '.gltf')));
ok('real city model budget remains bounded',
  CITY_MODEL_BUDGET.high === 18 &&
  CITY_MODEL_BUDGET.medium === 8 &&
  CITY_MODEL_BUDGET.low === 0);

done();
