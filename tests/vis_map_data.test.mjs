/** vis_map_data.test.mjs — MapData（P2 车道级数据通道）纯函数回归
 *
 * 锁三件事：
 *   1. resolveMapId 场景名 → 地图 id（别名表 + 动态直传，C 端检查存在性；
 *      f3fd00d 起不再本地 allowlist：去 _map 后缀 / 非 OSM 返回 null 均移除）
 *   2. setMapData 注入后 mapDataForScenario 返回 {junctions, laneData} 索引
 *   3. laneData 只收有 lanes 的 road；junctions 原样透传
 *
 * fetch 路径（live 拉取）不在此测（无头环境），由 junction_markings 的
 * lane_data 消费测试 + 浏览器验证兜底。
 */
import { resolveMapId, setMapData, resetMapData, mapDataForScenario,
  selectRoadsForPreview, selectBuildingsForPreview } from '../tools/flowboard/js/vis/model/MapData.js';
import { ok, done } from './test-utils.mjs';

console.log('=== MapData 数据通道 ===\n');

// ── 1. resolveMapId ──
ok('场景名直连（osm_lujiazui）', resolveMapId('osm_lujiazui') === 'osm_lujiazui');
ok('别名（osm_city_map → osm_test）', resolveMapId('osm_city_map') === 'osm_test');
// f3fd00d「动态地图发现」后：不再本地去 _map 后缀 / 判非 OSM，一律直传由 C 端检查
ok('动态直传不再去 _map 后缀（city_ring_map）', resolveMapId('city_ring_map') === 'city_ring_map');
ok('动态直传非 OSM 场景（straight_road）', resolveMapId('straight_road') === 'straight_road');
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

// ── 4. 超大地图走廊 LOD ──
(() => {
  const roads = [
    { id: 'route', centerline: [[0, 0, 0], [100, 0, 0]] },
    { id: 'near', centerline: [[200, 0, 0], [300, 0, 0]] },
    { id: 'far', centerline: [[10000, 10000, 0], [10100, 10000, 0]] },
  ];
  // 2403101 把 LARGE_MAP_ROAD_LIMIT 5000→50000（郑东全量渲染）。
  // 小图 = 49999 条（≤50000 全量）；大图 = 50002 条（>50000 才触发走廊裁剪）。
  for (let i = 0; i < 49996; i++) roads.push({
    id: 'filler_' + i, centerline: [[10000 + i, 10000, 0], [10001 + i, 10000, 0]],
  });
  const selected = selectRoadsForPreview(roads, { road_chain: ['route'] });
  ok('小于大图阈值不误裁剪', selected.length === roads.length);
  const large = roads.concat(Array.from({ length: 3 }, (_, i) => ({
    id: 'large_filler_' + i, centerline: [[12000 + i, 12000, 0], [12001 + i, 12000, 0]],
  })));
  const largeSelected = selectRoadsForPreview(large, { road_chain: ['route'] });
  ok('超大图保留路线', largeSelected.some((r) => r.id === 'route'));
  ok('超大图保留路线走廊', largeSelected.some((r) => r.id === 'near'));
  ok('超大图剔除远端细节', !largeSelected.some((r) => r.id === 'far'));
})();

// ── 5. 建筑走廊过滤 ──
(() => {
  const roads = [
    { id: 'route', centerline: [[0, 0, 0], [100, 0, 0]] },
    { id: 'far', centerline: [[10000, 10000, 0], [10100, 10000, 0]] },
  ];
  const buildings = [
    { id: 'b_near', footprint: [[10, -10], [10, 10], [-10, 10], [-10, -10]] },   // 走廊内
    { id: 'b_far', footprint: [[5950, 6000], [6050, 6000], [6050, 6050], [5950, 6050]] },  // 远离所有路
  ];
  const near = selectBuildingsForPreview(buildings, roads);
  ok('建筑走廊内保留', near.some((b) => b.id === 'b_near'));
  ok('建筑走廊外剔除', !near.some((b) => b.id === 'b_far'));
  ok('无道路时建筑不误裁剪', selectBuildingsForPreview(buildings, []).length === buildings.length);
})();

resetMapData();
ok('reset 后未加载场景返回 null', mapDataForScenario('city_ring') === null);

done();
