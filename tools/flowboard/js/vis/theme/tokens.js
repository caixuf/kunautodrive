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
  /* SR/BEV 科技风专用（非锁定语义色，可随换肤调整）：
   *  - lineEmissiveWhite/Yellow：标线自发光色（配合 Bloom 出霓虹辉光）
   *  - srGroundTint：SR/BEV 深色冷底，压暗地面让标线/车灯突出 */
  lineEmissiveWhite: 0x9fb8d8,
  lineEmissiveYellow: 0xffd700,
  srGroundTint: 0x39424d,
  /* M5: 泊车车位、止轮器与路面摩擦系数贴花配色 */
  parkingBox: 0x00e5ff,       // 目标车位框线（科技发光青）
  parkingBoxYellow: 0xffd700, // 目标车位框线（发光黄）
  stopperRubber: 0x222225,    // 橡胶止轮器主体（深黑橡胶）
  stopperStripe: 0xffcc00,    // 止轮器黄色反光警示带
  iceSurface: 0x80d8ff,       // 结冰路面贴花（冰蓝半透明）
  wetSurface: 0x1e2630,       // 湿滑路面贴花（深灰水膜）
  /* 感知 SR / 多模态预测光带 / 动态车周特效配色 */
  percBracket: 0x00e5b0,      // 3D 感知角标框（科技翡翠青）
  percFov: 0x00d8b4,          // 传感器前视包络/地面波束（低透明度科技青）
  predLaneKeep: 0x00e676,     // 预测模态：车道保持（翡翠绿）
  predCutIn: 0xff9100,        // 预测模态：切入告警（琥珀橙）
  predYield: 0x2979ff,        // 预测模态：减速让行（蔚蓝）
  predDefault: 0x00e5ff,      // 预测模态：默认（科技发光青）
  brakeGlow: 0xff2200,        // 紧急制动尾光（亮红）
  hazardOrange: 0xff4400,     // 紧急双闪（橙红）
  hazardYellow: 0xffcc00,     // 紧急双闪（警示黄）
  cutinChevron: 0xff9100,     // 旁车切入贴地警示翼（琥珀橙）
  sprayMist: 0x90b8d8,        // 雨天轮胎水雾尾迹（冷蓝白）
};

/* ── 车道诊断叠加层配色（诊断语义色，不参与 junction_markings 换肤锁定）──
 * 四组数据必须一眼可分，任何一组与其它组撞色都会让"谁跟谁不一致"读不出来：
 *   ① 规划参考线（规划在追的车道中心）  ② 车道格网（系统认为的车道在哪）
 *   ③ 权威车道定位（esmini Frenet 解算） ④ 感知车道线（沙箱合成边界）
 * 刻意避开 SCENE.lineWhite/lineYellow —— 那是路面标线本身，不是诊断量。 */
export const LANE_DIAG = {
  plannerRef: 0x00e5ff,   // 青：规划参考线
  lattice:    0x8b949e,   // 灰：车道格网
  laneMatch:  0x3fb950,   // 绿：权威车道定位
  detected:   0xff9f1c,   // 橙：感知车道线（synthetic）
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
