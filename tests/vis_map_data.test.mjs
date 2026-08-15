/** vis_map_data.test.mjs — MapData（P2 车道级数据通道）纯函数回归
 *
 * 锁三件事：
 *   1. resolveMapId 场景名 → allowlist 地图 id（直连/别名/去 _map 后缀/不匹配 null）
 *   2. setMapData 注入后 mapDataForScenario 返回 {junctions, laneData} 索引
 *   3. laneData 只收有 lanes 的 road；junctions 原样透传
 *
 * fetch 路径（live 拉取）不在此测（无头环境），由 junction_markings 的
 * lane_data 消费测试 + 浏览器验证兜底。
 */
import { resolveMapId, setMapData, resetMapData, mapDataForScenario }
  from '../tools/flowboard/js/vis/model/MapData.js';
import { ok, done } from './test-utils.mjs';

console.log('=== MapData 数据通道 ===\n');

// ── 1. resolveMapId ──
ok('场景名直连（osm_lujiazui）', resolveMapId('osm_lujiazui') === 'osm_lujiazui');
ok('别名（osm_city_map → osm_test）', resolveMapId('osm_city_map') === 'osm_test');
ok('去 _map 后缀（city_ring_map → city_ring）', resolveMapId('city_ring_map') === 'city_ring');
ok('非 OSM 场景返回 null（straight_road）', resolveMapId('straight_road') === null);
ok('空场景名返回 null', resolveMapId('') === null && resolveMapId(undefined) === null);

// ── 2/3. setMapData + 索引 ──
resetMapData();
const fakeMap = {
  roads: [
    { id: '东泰路', lanes: [{ id: '东泰路.lane.1', centerline: [[0, 0, 0], [10, 0, 0]], width: 3, markings: [] }] },
    { id: '无车道城市路' },   // 无 lanes 字段 → 不进 laneData
    { id: '空车道路', lanes: [] },  // 空数组 → 不进
  ],
  junctions: [{ id: 0, type: 'fork', incoming_road: '东泰路', connecting_roads: [] }],
};
setMapData('osm_lujiazui', fakeMap);
const md = mapDataForScenario('osm_lujiazui');
ok('注入后按场景名取回数据', md !== null);
ok('laneData 只收有 lanes 的 road',
  md && Object.keys(md.laneData).length === 1 && Array.isArray(md.laneData['东泰路']));
ok('junctions 原样透传', md && md.junctions.length === 1 && md.junctions[0].type === 'fork');
ok('别名场景取同一地图', mapDataForScenario('osm_city_map') === null);   // 未加载 osm_test → null（会触发 fetch，无头环境失败转 _failed）

resetMapData();
ok('reset 后未加载场景返回 null', mapDataForScenario('city_ring') === null);

done();
