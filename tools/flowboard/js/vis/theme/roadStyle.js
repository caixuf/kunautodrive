/** roadStyle.js — 标线/路面视觉样式声明式单一事实源（阶段1）
 *
 * 与 Coord.js / tokens.js 同构：所有标线视觉参数只能从这里出。
 * 目的：换肤（AIGC 一键换肤） = 新增/切换一张 STYLE 集，view 零改动。
 *
 * 两级结构：
 *  - MARKING：标线语义类型 → 基础视觉参数（color/emissive），来自 SCENE token。
 *  - STYLE：整套风格覆盖（real 写实 / sr 科技），控制发光强度、地面 tint、
 *           Bloom 激进与否。view 运行时按当前风格查 STYLE，动态 reapply。
 *
 * 与阶段0的关系：setMarkingEmissive/setTechMode/setBloomTech 的强度参数
 * 从硬编码收敛到这里，main.js _applySceneStyle 改读本表，可扩展新风格。
 */

import { SCENE } from './tokens.js';

/* ── 标线语义 → 基础视觉参数（几何类型分发仍在 RoadView buildLaneMarkingsInto，
 *    这里只收敛"颜色/自发光"这类视觉参数，供材质创建时取值）── */
export const MARKING = {
  white:   { color: SCENE.lineWhite,         emissive: SCENE.lineEmissiveWhite },
  yellow:  { color: SCENE.lineYellow,        emissive: SCENE.lineEmissiveYellow },
  crosswalk: { color: SCENE.crosswalk,       emissive: SCENE.crosswalk },
  guideLine: { color: SCENE.guideLine,       emissive: SCENE.guideLine },
};

/* ── 风格集：换肤 = 新增一张表 ──
 * 各字段含义：
 *  - markingEmissiveWhite/Yellow：白/黄标线自发光强度（0=写实不发光，>0=霓虹）
 *  - groundTint：地面 tint（null=还原纯纹理，hex=深色冷底）
 *  - bloomTech：Bloom 是否激进（true=标线/路牌辉光，false=只真车灯） */
export const STYLE = {
  real: {
    label: '写实',
    markingEmissiveWhite: 0,
    markingEmissiveYellow: 0,
    groundTint: null,
    bloomTech: false,
  },
  sr: {
    label: 'SR科技',
    markingEmissiveWhite: 0.9,
    markingEmissiveYellow: 0.55,
    groundTint: SCENE.srGroundTint,
    bloomTech: true,
  },
};
