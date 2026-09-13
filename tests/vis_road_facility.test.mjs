import { inferRoadFacilities } from '../tools/flowboard/js/vis/view/RoadFacilityView.js';
import { eq, ok, done } from './test-utils.mjs';

console.log('=== road facility layout ===\n');

const road = {
  edges: [{
    id: 0,
    type: 'urban',
    name: 'parking_exam_road',
    lanes: 4,
    lane_width: 3.5,
    nodes: [[0, 0, 0], [500, 0, 0]],
  }],
};
const entities = [
  { id: 101, type: 'car', x: 130, y: -8.75, heading: 0, speed: 0 },
  { id: 102, type: 'car', x: 150, y: -8.75, heading: 0, speed: 0 },
  { id: 9, type: 'tl', x: 250, y: 8.5, stop_x: 248, stop_y: -1.75 },
];

const layout = inferRoadFacilities(road, entities);
eq('两辆停放车辆 + 中间空位生成 3 个车位', layout.parkingBays, 3);
eq('信号灯生成 1 条停止线', layout.stopLines, 1);
eq('停止线后方生成 1 组正确人行横道', layout.crosswalks, 1);
// GB 5768.3 5.8：斑马线条纹应与道路中心线平行（与车同向），沿道路长度短（2.5m）。
const crosswalkMarks = layout.marks.filter(mark => mark.width === 0.48);
ok('斑马线条纹平行于道路（heading=0）且长度短（2.5m）',
  crosswalkMarks.length >= 2 &&
  crosswalkMarks.every(m => Math.abs(m.heading) < 1e-6) &&
  crosswalkMarks.every(m => Math.abs(m.length - 2.5) < 1e-6));
const stopLine = layout.marks.find(mark => mark.width === 0.38);
ok('停止线横跨来车方向的半幅道路',
  stopLine && Math.abs(stopLine.x - 248) < 1e-6 &&
  Math.abs(stopLine.y + 3.5) < 1e-6 &&
  Math.abs(stopLine.length - 7) < 1e-6);
eq('500m 四车道双向：4 个站位 × 每车道 1 箭 = 16', layout.arrows, 16);
const arrowStems = layout.marks.filter(m => m.width === 0.28 && Math.abs(m.length - 2.8) < 1e-6);
ok('箭头箭杆不落在双黄线（|y|≥1.5）',
  arrowStems.length === 16 && arrowStems.every(m => Math.abs(m.y) >= 1.5));
ok('箭头落在车道中心 ±1.75 / ±5.25',
  arrowStems.every(m => {
    const ay = Math.abs(m.y);
    return Math.abs(ay - 1.75) < 0.05 || Math.abs(ay - 5.25) < 0.05;
  }));
eq('空车位四角生成 4 根考试桩杆', layout.poles.length, 4);
ok('全部设施合并为共享路面标记实例', layout.marks.length > 20);

const demoRoad = {
  edges: [{
    id: 0, type: 'highway', name: 'straight_with_s_curve',
    lanes: 4, lane_width: 3.5,
    nodes: [[0, 0, 0], [300, 0, 0]],
  }],
};
const demoLayout = inferRoadFacilities(demoRoad, [], 'straight_road');
const demoStems = demoLayout.marks.filter(m => m.width === 0.28 && Math.abs(m.length - 2.8) < 1e-6);
ok('demo.sh 双向四车道：箭头不画在 y=0 双黄线上',
  demoStems.length > 0 && demoStems.every(m => Math.abs(m.y) >= 1.5));
const westbound = demoStems.filter(m => Math.abs(Math.abs(m.heading) - Math.PI) < 1e-3);
ok('北侧车道（+y）箭头朝西（heading≈π）',
  westbound.length > 0 && westbound.every(m => m.y > 0));

const osmLike = inferRoadFacilities({
  edges: demoRoad.edges,
  lane_data: { straight_with_s_curve: [{ centerline: [[0, 0], [300, 0]], width: 3.5 }] },
}, [], 'osm_zhengdong');
eq('大地图有 lane_data 时沿路箭头交给路口层，不再沿路铺', osmLike.arrows, 0);

const numericRoad = { edges: [{ ...road.edges[0], name: '0' }] };
const runtimeLayout = inferRoadFacilities(numericRoad, entities, 'auto_parking');
eq('运行时道路名丢失时由场景名保持泊车建模', runtimeLayout.parkingBays, 3);

done();
