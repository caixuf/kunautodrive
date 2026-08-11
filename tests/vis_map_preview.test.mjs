import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { ok, done } from './test-utils.mjs';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const previewHtml = fs.readFileSync(
  path.join(repo, 'tools/flowboard/map_preview.html'), 'utf8');
const previewJs = fs.readFileSync(
  path.join(repo, 'tools/flowboard/js/mapPreview.js'), 'utf8');
const indexHtml = fs.readFileSync(
  path.join(repo, 'tools/flowboard/index.html'), 'utf8');
const appJs = fs.readFileSync(
  path.join(repo, 'tools/flowboard/js/app.js'), 'utf8');

console.log('=== isolated map preview contracts ===\n');

ok('preview owns a real canvas',
  previewHtml.includes('<canvas id="scene3d-canvas"></canvas>'));
ok('preview initializes renderer with the canvas',
  previewJs.includes('init3DScene(canvas)'));
ok('preview never passes its container div as a canvas',
  !previewJs.includes("init3DScene(document.getElementById('scene3d'))"));
ok('preview installs THREE before loading view modules',
  previewJs.includes("import './bootstrap.js'"));
ok('preview does not override renderer software fallback',
  !previewJs.includes("setPerfTier('high')"));
ok('modal has a dedicated draggable panel and handle',
  indexHtml.includes('id="map-preview-panel"') &&
  indexHtml.includes('id="map-preview-handle"'));
ok('dragging uses pointer capture',
  appJs.includes('setPointerCapture') && appJs.includes("'pointermove'"));
ok('modal exposes a close control',
  indexHtml.includes('flowboard.closeMapPreview()'));

done();
