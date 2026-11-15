/**
 * vis_world_engine.test.mjs — 程序化世界引擎前端编译器、客户端同步与道具渲染门禁
 */

import fs from 'node:fs';
import { compileWorld } from '../tools/flowboard/js/vis/world/WorldCompiler.js';
import {
  resolveWorldLayout,
  fetchWorldLayout,
  getCachedWorldLayout,
  clearWorldLayoutCache,
} from '../tools/flowboard/js/vis/model/WorldLayoutClient.js';
import { createWorldPropsView } from '../tools/flowboard/js/vis/view/WorldPropsView.js';
import { createSceneDirector } from '../tools/flowboard/js/vis/director/SceneDirector.js';
import { worldToThree } from '../tools/flowboard/js/vis/math/Coord.js';

let pass = 0, fail = 0;
function ok(name, cond, detail = '') {
  if (cond) {
    console.log(`  PASS  ${name}`);
    pass++;
  } else {
    console.log(`  FAIL  ${name}${detail ? ` — ${detail}` : ''}`);
    fail++;
  }
}

console.log('=== WorldEngine 前端编译器 / 客户端 / Props 渲染验证 ===\n');

const sc = JSON.parse(fs.readFileSync('scenarios/city_procedural_world.json', 'utf8'));

// ── 1. 离线确定性与声明式精修编译 ──
console.log('--- 1. WorldCompiler.js 确定性与精修规则 ---');
const layoutA = compileWorld(sc.road_network, sc.world_gen, sc.random_seed);
const layoutB = compileWorld(sc.road_network, sc.world_gen, sc.random_seed);
ok('编译结果两次运行 bit-identical', JSON.stringify(layoutA) === JSON.stringify(layoutB));
ok('生成路灯 poles > 15', layoutA.poles.length > 15, `got ${layoutA.poles.length}`);
ok('生成行道树 trees > 10', layoutA.trees.length > 10, `got ${layoutA.trees.length}`);
ok('生成天际线建筑 buildings > 10', layoutA.buildings.length > 10, `got ${layoutA.buildings.length}`);
ok('生成交通标志 signs >= 3', layoutA.signs.length >= 3, `got ${layoutA.signs.length}`);
ok('零沥青压占残余', layoutA.sanity.remaining_asphalt_intrusions === 0);

// 验证 exclude_zones (x=200, y=0, radius=32) 移除范围内树和建筑
const inExclTree = layoutA.trees.some(t => Math.hypot(t.x - 200, t.y - 0) < 31.5);
const inExclBldg = layoutA.buildings.some(b => Math.hypot(b.x - 200, b.y - 0) < 31.5);
ok('exclude_zones 精准清空路口视线区内树木与建筑', !inExclTree && !inExclBldg);

// 验证 edge_overrides: skybridge_overpass (x=580..920, z>2) 无树 + overpass portal pier clear of crossing avenue (x=750)
const treesOnOverpass = layoutA.trees.some(t => t.z > 2.0 && t.x > 580 && t.x < 920);
const portalPiers = layoutA.piers.filter(p => p.columns === 2 && p.x > 560);
const pierOnUnderpass = layoutA.piers.some(p => Math.abs(p.x - 750) < 7.5);
ok('edge_overrides 禁止 skybridge_overpass 高架面上生成树木', !treesOnOverpass);
ok('高架桥墩采用双柱门式墩且绝不落入下穿路面', portalPiers.length >= 2 && !pierOnUnderpass,
  `portal=${portalPiers.length} inUnderpass=${pierOnUnderpass}`);

// 验证 road_s 精修道具贴路定位
const welcomeGantry = layoutA.props.find(p => p.id === 'city_welcome_gantry');
ok('road_s 自定义欢迎门架精准落位于 x≈55, y≈0',
  Boolean(welcomeGantry && Math.abs(welcomeGantry.x - 55.0) < 0.5 && Math.abs(welcomeGantry.y) < 0.5));

// 切换预设应当产生产出差异和不同 world_hash
const parkLayout = compileWorld(sc.road_network, { ...sc.world_gen, preset: 'suburban_park' }, sc.random_seed);
ok('不同 world_gen preset 产出不同 world_hash', parkLayout.world_hash !== layoutA.world_hash);

// ── 2. WorldPropsView.js 3D 渲染与坐标对齐 ──
console.log('\n--- 2. WorldPropsView.js 几何生成与 Coord 对齐 ---');
const scene = new THREE.Scene();
const propsView = createWorldPropsView(scene);
propsView.build(sc.road_network, { worldLayout: layoutA });
const isRealThree = Boolean(new THREE.Group().isObject3D);
if (isRealThree) {
  ok('WorldPropsView 构建子节点非空', propsView.getGroup().children.length >= 6,
    `children=${propsView.getGroup().children.length}`);
  const [expX, , expZ] = worldToThree(55.0, 0.0, 0.0);
  const nearGantryChild = propsView.getGroup().children.some(ch =>
    ch.position && Math.hypot(ch.position.x - expX, ch.position.z - expZ) < 1.0 && ch.position.y > 5.0
  );
  ok('欢迎门架横梁在 Three 世界坐标与 worldToThree(55,0,0) 对齐且高于路面', nearGantryChild);
} else {
  ok('WorldPropsView build 在 headless THREE shim 下无异常完成', Boolean(propsView.getGroup()));
}
propsView.dispose();
ok('dispose 清空全部子节点', propsView.getGroup().children.length === 0);

// ── 3. WorldLayoutClient + SceneDirector 权威通道升级测试 ──
console.log('\n--- 3. WorldLayoutClient 异步接管 C++ 权威布局 ---');
clearWorldLayoutCache();
const authHash = 'deadbeef12345678';
const mockAuthLayout = {
  ...layoutA,
  authoritative: 'world_engine',
  world_hash: authHash,
  poles: [
    { x: 90.0, y: -8.5, z: 0.0, nx: 0.0, ny: -1.0, heading: 0.0, side: 1 },
  ],
  trees: [],
};

const origFetch = globalThis.fetch;
let layoutFetchCount = 0;
globalThis.fetch = async (url) => {
  const s = String(url);
  if (s.includes('/api/world/layout')) {
    layoutFetchCount++;
    ok('/api/world/layout 请求携带对应 hash 参数', s.includes(authHash));
    return { ok: true, json: async () => mockAuthLayout };
  }
  return { ok: false, status: 404, json: async () => null };
};

try {
  const dirScene = new THREE.Scene();
  const director = createSceneDirector(dirScene);
  director.update({
    metrics: {
      scene: {
        scenario_name: 'city_procedural_world',
        world_layout_hash: authHash,
        road_network: sc.road_network,
        ego: { x: 20, y: -1.75, z: 0, heading: 0, speed: 10 },
        entities: [],
      },
    },
  });

  // 首帧同步先用 fallback 渲染，零闪烁空窗
  const initialStore = director.getStore();
  ok('首帧同步回退 WorldCompiler 布局立即就绪', Boolean(initialStore.worldLayout && initialStore.worldLayout.poles.length > 0));

  // 等待微任务让 mock fetch 返回并触发异步热升级
  await new Promise(r => setTimeout(r, 20));
  const upgradedStore = director.getStore();
  ok('fetch 只请求一次（hash 去重）', layoutFetchCount === 1, `layoutFetchCount=${layoutFetchCount}`);
  ok('权威缓存已命中 authHash', Boolean(getCachedWorldLayout(authHash)));
  ok('SceneDirector 自动热切换到 C++ world_engine 权威布局',
    upgradedStore.worldLayout &&
    upgradedStore.worldLayout.authoritative === 'world_engine' &&
    upgradedStore.worldLayout.poles.length === 1);

  // 验证 resolveWorldLayout 直接命中缓存
  const resolved = resolveWorldLayout({ worldLayoutHash: authHash, roadNetwork: sc.road_network });
  ok('resolveWorldLayout 命中的权威布局 poles=1', resolved && resolved.poles.length === 1);
  // 也覆盖一遍显式 fetchWorldLayout 调用
  const refetched = await fetchWorldLayout(authHash);
  ok('fetchWorldLayout 命中本地缓存不再发请求', Boolean(refetched && layoutFetchCount === 1));

  director.dispose();
} finally {
  globalThis.fetch = origFetch;
  clearWorldLayoutCache();
}

console.log(`\n--- 总结: ${pass} pass, ${fail} fail ---`);
process.exit(fail > 0 ? 1 : 0);
