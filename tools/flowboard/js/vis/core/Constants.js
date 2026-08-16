/**
 * Constants.js — 前端可视化层单一事实源
 * 
 * 所有跨模块共享的常量、魔法数字、枚举值集中定义，消除硬编码漂移。
 */

export const LANE_WIDTH = 3.5;
export const DEFAULT_LANES = 2;

export const EDGE_TYPE = {
  HIGHWAY: 'highway',
  URBAN: 'urban',
  VIADUCT_HIGHWAY: 'viaduct_highway',
  RAMP_CURVE: 'ramp_curve',
  INTERSECTION: 'intersection',
};

export const ENTITY_TYPE = {
  CAR: 'car',
  PEDESTRIAN: 'pedestrian',
  TRAFFIC_LIGHT: 'traffic_light',
  SIGN: 'sign',
};

export const VIADUCT_HEIGHT = 7.0;
export const VIADUCT_RIDE_HEIGHT = 7.7;
export const VIADUCT_VIS_LENGTH = 500;  // 高架可视段长度 (m)，兜底 edge.length 缺失

/* 隧道/地道引道名字特征（OSM tunnel 标签在 osm_to_map 里丢失，只剩名字可辨）。
 * RoadView 用它把隧道降级为暗色无标线路面（俯视不再斜穿街区），
 * ConnectorView 用它抑制隧道口的斑马线/停止线（行人不可穿越）。
 * 2026-08-16：elevation_patch 在 map.json 写 tunnel 字段后，MapData 透传
 * edge.tunnel，优先于 name 正则匹配。 */
export const TUNNEL_NAME_RE = /隧道|地道|tunnel/i;
export function isTunnelEdge(edge) {
  if (!edge) return false;
  if (edge.tunnel === true || edge.tunnel === 'yes') return true;
  return TUNNEL_NAME_RE.test(String((edge.name || edge.id) || ''));
}