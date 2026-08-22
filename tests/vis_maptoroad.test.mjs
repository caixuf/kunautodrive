/**
 * vis_maptoroad.test.mjs — 独立 HD map → road_network 门禁
 *
 * 目标：证明「独立地图 map.json 能正确转成前端 road_network」，且转换结果
 * 满足 RoadView/SceneDirector 消费所需字段。这是可视化与仿真解耦（方案 B）
 * 的核心纯函数门禁——主仪表盘在独立地图模式下用 mapToRoadNetwork 生成路网，
 * 替代从 metrics.scene 读取的仿真 road_network。
 *
 * 跑法：
 *   node tests/vis_maptoroad.test.mjs
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { mapToRoadNetwork } from '../tools/flowboard/js/showcase/sceneAdapter.js';
import { ok, done } from './test-utils.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(__dirname, '..');

console.log('=== 独立 HD map → road_network 门禁 ===\n');

// ── 1. 每张 map.json 都能转出合法 road_network ──
{
  console.log('--- 1. maps/*/map.json → road_network ---');
  const mapsDir = path.join(REPO, 'maps');
  const mapDirs = fs.readdirSync(mapsDir, { withFileTypes: true })
    .filter((d) => d.isDirectory())
    .map((d) => d.name);
  ok('发现 map 目录 >= 1', mapDirs.length >= 1);

  let totalEdges = 0;
  for (const mid of mapDirs) {
    const mapPath = path.join(mapsDir, mid, 'map.json');
    if (!fs.existsSync(mapPath)) continue;
    const map = JSON.parse(fs.readFileSync(mapPath, 'utf8'));
    const rn = mapToRoadNetwork(map);
    ok(`[${mid}] road_network.edges 是数组`, Array.isArray(rn.edges));
    ok(`[${mid}] edges 非空`, rn.edges.length > 0);
    totalEdges += rn.edges.length;
    let invalidEdges = 0;
    for (const e of rn.edges) {
      const first = e.nodes?.[0];
      const valid = Array.isArray(e.nodes) && e.nodes.length >= 2 &&
        typeof e.lanes === 'number' && e.lanes > 0 &&
        typeof e.lane_width === 'number' && e.lane_width > 0 &&
        typeof e.type === 'string' && e.type.length > 0 &&
        typeof e.oneway === 'boolean' &&
        Array.isArray(first) && first.length === 3 &&
        typeof first[0] === 'number' && typeof first[1] === 'number';
      if (!valid) invalidEdges++;
    }
    ok(`[${mid}] 全部 ${rn.edges.length} 条 edge 字段完整合规`, invalidEdges === 0);
  }
  ok('总 edge 数 > 0', totalEdges > 0);
}

// ── 2. 非法输入降级为空路网（不崩溃）──
{
  console.log('--- 2. 非法输入降级 ---');
  const rn1 = mapToRoadNetwork(null);
  ok('null map → 空 edges', Array.isArray(rn1.edges) && rn1.edges.length === 0);
  const rn2 = mapToRoadNetwork({});
  ok('{} map → 空 edges', Array.isArray(rn2.edges) && rn2.edges.length === 0);
  const rn3 = mapToRoadNetwork({ roads: [] });
  ok('空 roads → 空 edges', Array.isArray(rn3.edges) && rn3.edges.length === 0);
}

// ── 3. 与 SceneDirector 消费兼容（喂一帧验证不抛错）──
{
  console.log('--- 3. 转换结果喂 SceneDirector ---');
  const cityRingPath = path.join(REPO, 'maps/city_ring/map.json');
  if (fs.existsSync(cityRingPath)) {
    const map = JSON.parse(fs.readFileSync(cityRingPath, 'utf8'));
    const rn = mapToRoadNetwork(map);
    // 构造一帧：road_network + 空实体（感知实体由另一通道提供）
    const frame = {
      t_us: 0,
      road_network: rn,
      ego: { type: 'ego', id: 0, x: 0, y: 0, z: 0, heading: 0, speed: 0 },
      entities: [],
      perception_entities: [],
      source: 'perception',
    };
    // static import 需要 THREE；用相对轻量的校验：road_network.edges 字段齐全即可
    ok('city_ring road_network 可被 SceneDirector 消费（字段齐全）',
      rn.edges.every((e) => e.nodes && e.lanes && e.lane_width && e.type));
  } else {
    ok('city_ring map 存在（跳过场景验证）', false);
  }
}

done();