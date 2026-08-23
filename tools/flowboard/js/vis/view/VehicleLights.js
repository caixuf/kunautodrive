/**
 * VehicleLights.js — 车灯位掩码 + 纯状态推导（零 THREE 依赖，便于单元测试）
 *
 * Step 5 重构：从 VehicleView.js 抽出。
 *  - deriveLightState: 纯函数，无 THREE 依赖，便于单元测试
 *  - createVehicleLights: 在 VehicleView.js 中实现（需 THREE）
 *
 * 车灯位掩码（与 flowsim/vehicle_lights.h 一致）：
 *   bit0=左转 0x01, bit1=右转 0x02, bit2=双闪 0x04,
 *   bit3=远光 0x08, bit4=近光 0x10, bit6=倒车 0x40, bit7=雾灯 0x80
 * 刹车灯由 brake 字段驱动（不在 lights 位掩码里）。
 */

export const LIGHT_TURN_LEFT  = 0x01;
export const LIGHT_TURN_RIGHT = 0x02;
export const LIGHT_HAZARD     = 0x04;
export const LIGHT_HIGH_BEAM  = 0x08;
export const LIGHT_LOW_BEAM   = 0x10;
export const LIGHT_CLEARANCE  = 0x20;
export const LIGHT_REVERSE    = 0x40;
export const LIGHT_FOG        = 0x80;

/** 车灯位掩码 → 渲染 state 对象（纯函数，无 THREE 依赖）
 *  - brake 由 brake 字段驱动（不在 lights 位掩码里）
 *  - hazard（双闪）让左右转向灯同时亮
 *  - head 由 low_beam 或 high_beam 或 fog 触发（或当 env 处于 night/dusk/low_vis 时自动开启）
 *  - clearance（示廓灯/后位灯）由 clearance 位或 head 或 env 处于 night/dusk 时自动开启
 *  - fog 由 fog 位或 env 处于 fog/sandstorm 时开启
 *  @param {number} mask  vehicle_lights.h 的位掩码
 *  @param {number} brake  brake 字段（>0.05 触发刹车灯）
 *  @param {object} [env]  环境状态 { isNight, lighting, weather } 可选
 *  @returns {{brake:boolean, turnL:boolean, turnR:boolean, head:boolean, clearance:boolean, fog:boolean}} */
export function deriveLightState(mask, brake, env) {
  const envDark = !!(env && (env.isNight || env.lighting === 'night' || env.lighting === 'dusk' || env.lighting === 'dawn'));
  const envLowVis = !!(env && (env.weather === 'rain' || env.weather === 'storm' || env.weather === 'snow' || env.weather === 'fog' || env.weather === 'sandstorm' || (Number.isFinite(env.visibilityM) && env.visibilityM <= 500)));
  // 雾灯仅在浓雾、沙尘暴、雷暴大雨或能见度 <= 200m（GB 4785 交规标准）时开启，普通晴天夜间不开雾灯
  const envFoggy = !!(env && (env.weather === 'fog' || env.weather === 'sandstorm' || env.weather === 'storm' || (Number.isFinite(env.visibilityM) && env.visibilityM <= 200)));

  const hasHighBeam = !!(mask & LIGHT_HIGH_BEAM);
  const hasLowBeam = !!(mask & LIGHT_LOW_BEAM);
  const hasClearance = !!(mask & LIGHT_CLEARANCE);
  const hasFog = !!(mask & LIGHT_FOG) || envFoggy;
  const isHead = hasLowBeam || hasHighBeam || hasFog || envDark || envLowVis;
  const isClearance = hasClearance || isHead || envDark;

  return {
    brake: brake > 0.05,
    turnL: !!(mask & (LIGHT_TURN_LEFT | LIGHT_HAZARD)),
    turnR: !!(mask & (LIGHT_TURN_RIGHT | LIGHT_HAZARD)),
    head: isHead,
    clearance: isClearance,
    fog: hasFog,
  };
}
