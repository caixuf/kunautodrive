/**
 * vis_camera_modes_and_dynamics.test.mjs — 动态相机跟踪回归门禁（多视角 + 坡道 + D/R 倒车）
 *
 * 覆盖：
 *   1. driver / bev 模式在帧间隔抖动下的平滑跟随（无二次指数滞后震荡）
 *   2. 高架匝道爬坡（groundY 连续抬升）时相机高度二阶差分有界
 *   3. D/R 换挡掉头（航向转向 + 速度过零倒车）时 DeadReckon + CameraRig 无跳变尖峰
 */

import assert from 'node:assert/strict';
import './support/three-preload.mjs';

import { createCameraRig } from '../tools/flowboard/js/vis/core/CameraRig.js';
import {
  initDeadReckon,
  updateDeadReckon,
  tickDeadReckon,
  getDeadReckonState,
} from '../tools/flowboard/js/vis/core/DeadReckon.js';

let simClockMs = 1000;
globalThis.performance = { now: () => simClockMs };

function makeRig() {
  const canvas = {
    clientWidth: 800,
    clientHeight: 600,
    style: {},
    addEventListener() {},
    removeEventListener() {},
    getRootNode() { return this; },
    getBoundingClientRect() { return { left: 0, top: 0, width: 800, height: 600 }; },
    setPointerCapture() {},
    releasePointerCapture() {},
  };
  return createCameraRig(canvas);
}

function assertSmoothTrajectory(samples, label, maxJump = 1.5, maxAccelJump = 0.8) {
  assert.ok(samples.length >= 10, `${label}: 样本数不足`);
  const steps = [];
  for (let i = 1; i < samples.length; i++) {
    const dx = samples[i].x - samples[i - 1].x;
    const dy = samples[i].y - samples[i - 1].y;
    const dz = samples[i].z - samples[i - 1].z;
    const dist = Math.hypot(dx, dy, dz);
    assert.ok(Number.isFinite(dist), `${label}: 第 ${i} 帧出现 NaN/Inf`);
    assert.ok(dist < maxJump, `${label}: 第 ${i} 帧一阶位移跳变过大 (${dist.toFixed(3)}m >= ${maxJump}m)`);
    steps.push(dist);
  }
  for (let i = 1; i < steps.length; i++) {
    const d2 = Math.abs(steps[i] - steps[i - 1]);
    assert.ok(d2 < maxAccelJump, `${label}: 第 ${i} 帧二阶抖动尖峰 (${d2.toFixed(3)}m >= ${maxAccelJump}m)`);
  }
}

// 1. driver / bev 模式在帧间隔抖动下跟踪平滑
for (const mode of ['driver', 'bev']) {
  initDeadReckon();
  const rig = makeRig();
  rig.setMode(mode);
  const activeCam = rig.getActiveCamera();

  const camPositions = [];
  const dts = [0.016, 0.033, 0.012, 0.025, 0.016, 0.040, 0.014, 0.018];
  let simT = 0;
  for (let i = 0; i < 60; i++) {
    const dt = dts[i % dts.length];
    simT += dt;
    simClockMs = 1000 + simT * 1000;
    if (i % 3 === 0) {
      updateDeadReckon(10.0 * simT, 0, 10.0, 0, 10.0, 0, 0, simT);
    }
    tickDeadReckon();
    const dr = getDeadReckonState();
    rig.update(
      { x: dr.smoothX, y: dr.smoothZ, z: 0, heading: dr.smoothHeading, speed: dr.smoothSpeed },
      null,
      simT
    );
    camPositions.push({ x: activeCam.position.x, y: activeCam.position.y, z: activeCam.position.z });
  }
  assertSmoothTrajectory(camPositions.slice(5), `mode=${mode}`, 1.2, 0.6);
  console.log(`  PASS mode=${mode} 帧抖动平滑跟踪 (${camPositions.length} 帧)`);
}

// 2. 高架匝道爬坡（groundY 连续抬升）
{
  initDeadReckon();
  const rig = makeRig();
  rig.setMode('chase');

  const camPositions = [];
  let simT = 0;
  for (let i = 0; i < 120; i++) {
    const dt = 0.05;
    simT += dt;
    simClockMs = 1000 + simT * 1000;
    const x = 12.0 * simT;
    const groundY = Math.min(8.0, 0.1 * x); // 10% 纵坡抬升至 8m 高架
    if (i % 2 === 0) {
      updateDeadReckon(x, 0, 12.0, 0, 12.0, 0, 0, simT);
    }
    tickDeadReckon();
    const dr = getDeadReckonState();
    rig.update(
      { x: dr.smoothX, y: dr.smoothZ, z: groundY, heading: dr.smoothHeading, speed: dr.smoothSpeed },
      null,
      simT
    );
    camPositions.push({ x: rig.camera.position.x, y: rig.camera.position.y, z: rig.camera.position.z });
  }
  assertSmoothTrajectory(camPositions.slice(4), 'elevated_ramp', 0.8, 0.3);
  assert.ok(camPositions[camPositions.length - 1].y > camPositions[0].y + 5.0, 'elevated_ramp: 相机高度应随匝道抬升');
  console.log(`  PASS elevated_ramp 爬坡相机连续抬升 (Δy=${(camPositions[camPositions.length - 1].y - camPositions[0].y).toFixed(2)}m)`);
}

// 3. D/R 换挡掉头（前进减速 -> 过零倒车 + 转向）
{
  initDeadReckon();
  const rig = makeRig();
  rig.setMode('chase');

  const camPositions = [];
  let simT = 0;
  let x = 0;
  let z = 0;
  let heading = 0;
  for (let i = 0; i < 100; i++) {
    const dt = 0.02;
    simT += dt;
    simClockMs = 1000 + simT * 1000;
    // 前 40 帧前进减速，40..60 过零换 R 挡，60..100 倒车转向
    const speed = i < 40 ? 4.0 * (1 - i / 40) : -2.0 * Math.min(1, (i - 40) / 20);
    const yawRate = i >= 50 ? 0.35 : 0.0;
    heading += yawRate * dt;
    x += speed * Math.cos(heading) * dt;
    z += speed * Math.sin(heading) * dt;

    if (i % 2 === 0) {
      updateDeadReckon(x, z, speed, heading, speed * Math.cos(heading), speed * Math.sin(heading), yawRate, simT);
    }
    tickDeadReckon();
    const dr = getDeadReckonState();
    rig.update(
      { x: dr.smoothX, y: dr.smoothZ, z: 0, heading: dr.smoothHeading, speed: dr.smoothSpeed },
      null,
      simT
    );
    camPositions.push({ x: rig.camera.position.x, y: rig.camera.position.y, z: rig.camera.position.z });
  }
  assertSmoothTrajectory(camPositions.slice(4), 'gear_reversal_uturn', 0.8, 0.35);
  console.log(`  PASS gear_reversal_uturn D/R 换挡倒车无跳变 (${camPositions.length} 帧)`);
}

console.log('ALL vis_camera_modes_and_dynamics checks passed.');
