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
ok('SU7 外部 bin/WebP 资源完整', referencedFiles.length > 0 &&
  referencedFiles.every(file => statSync(resolve(ROOT, file)).size > 0));
ok('SU7 归属说明存在', statSync(resolve(ROOT, 'README.md')).size > 0);

done();
