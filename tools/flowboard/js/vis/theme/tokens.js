/** tokens.js — FlowBoard 设计 token 单一事实源（P3 设计系统）
 *
 * 与坐标约定的 Coord.js 同构：所有颜色/字阶/间距/圆角/阴影只能从这里出，
 * view 里写裸 0x 十六进制字面量 = 违规（tests/vis_grep_enforce.mjs 有牙）。
 *
 * 3D 场景配色分层（2026-08-15 定标）：路面/匝道/路肩/人行道/路缘石
 * 五级明度拉开（16%→20%→33%→51%→66%），旧值全挤在 16~60% 暗灰区
 * 是"素"的根源。标线白/黄与绿化色值被 junction_markings 测试锁定，
 * 改值必须同步改测试（测试锁的是语义不是观感）。
 */

/* ── 3D 场景调色板（number，three.js 直接用）── */
export const SCENE = {
  asphalt: 0x24262b,      // 主路路面（最暗带，偏蓝冷灰）
  rampSurface: 0x2e3136,  // 匝道路面（比主路亮一档，读作支线）
  tunnel: 0x1b1b1d,       // 隧道/地道降级（比主路更暗的地下走廊）
  shoulder: 0x4e5257,     // 路肩
  sidewalk: 0x7e8286,     // 人行道
  curb: 0xa9a89e,         // 路缘石（最亮的道路家具）
  verge: 0x355d35,        // 绿化带（测试锁定值）
  groundBase: 0x6b7257,   // 城市地块基底（无纹理兜底）
  lineWhite: 0xcccccc,    // 白色标线（测试锁定值）
  lineYellow: 0xffd700,   // 双黄中心线（测试锁定值）
  crosswalk: 0xf5f5f0,    // 斑马线（测试锁定值）
  guideLine: 0xffffff,    // 转向导流/停止线/匝道斜纹（测试锁定值）
  pier: 0x6b6b6b,         // 桥墩
  barrelRed: 0xd02020,    // 防撞桶红
  barrelWhite: 0xf0f0f0,  // 防撞桶白
};

/* ── 仪表盘 UI 调色板（CSS 字符串，与 css/style.css :root 变量一一对应）── */
export const UI = {
  bg: '#0d1117',          // 页面底
  bgRaised: '#11161d',    // 卡片/弹层面板
  bgInset: '#0a0d14',     // 画布/输入框凹陷区
  border: '#30363d',      // 常规边框
  borderSoft: '#252d3a',  // 弱边框
  text: '#c9d1d9',        // 正文
  textBright: '#f0f6fc',  // 标题/强调
  textDim: '#8b949e',     // 次要
  textFaint: '#484f58',   // 占位/禁用
  accent: '#58a6ff',      // 主色（科技蓝）
  success: '#3fb950',
  warn: '#d29922',
  danger: '#f85149',
  violet: '#bc8cff',
};

/* ── 字阶（数据面板必须 tabular-nums，数字不跳动）── */
export const FONT = {
  xs: '11px', sm: '12px', md: '13px', lg: '16px', xl: '20px',
  numeric: 'font-variant-numeric: tabular-nums',
};

/* ── 间距（4 的倍数）── */
export const SPACE = { s1: '4px', s2: '8px', s3: '12px', s4: '16px', s5: '20px' };

/* ── 圆角 ── */
export const RADIUS = { sm: '6px', md: '10px' };

/* ── 阴影分级 ── */
export const SHADOW = {
  card: '0 2px 8px rgba(0,0,0,.4)',
  popover: '0 18px 48px rgba(0,0,0,.55)',
};
