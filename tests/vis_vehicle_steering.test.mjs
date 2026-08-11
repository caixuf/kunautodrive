/**
 * vis_vehicle_steering.test.mjs — 车辆转向显示符号回归测试。
 *
 * 后端约定 steer > 0 为左转，前轴和方向盘必须保持同向显示。
 */

import { _steeringVisualState } from '../tools/flowboard/js/vis/view/VehicleView.js';
import { ok, done } from './test-utils.mjs';

const left = _steeringVisualState(0.2);
const right = _steeringVisualState(-0.2);
const straight = _steeringVisualState(0.003);

ok('正 steer 显示为左转前轴（rotation.y > 0）', left.frontAxleYaw > 0);
ok('负 steer 显示为右转前轴（rotation.y < 0）', right.frontAxleYaw < 0);
ok('前轴和方向盘保持左转同向', left.steeringWheelRoll > 0);
ok('前轴和方向盘保持右转同向', right.steeringWheelRoll < 0);
ok('小于死区的输入不抖动车轮', straight.frontAxleYaw === 0 &&
  straight.steeringWheelRoll === 0);
ok('方向盘显示倍率为 7:1',
  Math.abs(left.steeringWheelRoll - left.frontAxleYaw * 7) < 1e-12);

done();
