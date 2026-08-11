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
eq('停止线不再误绘为斑马线', layout.crosswalks, 0);
const stopLine = layout.marks.find(mark => mark.width === 0.38);
ok('停止线横跨来车方向的半幅道路',
  stopLine && Math.abs(stopLine.x - 248) < 1e-6 &&
  Math.abs(stopLine.y + 3.5) < 1e-6 &&
  Math.abs(stopLine.length - 7) < 1e-6);
eq('500m 城市路生成 4 个方向箭头', layout.arrows, 4);
eq('空车位四角生成 4 根考试桩杆', layout.poles.length, 4);
ok('全部设施合并为共享路面标记实例', layout.marks.length > 20);

const numericRoad = { edges: [{ ...road.edges[0], name: '0' }] };
const runtimeLayout = inferRoadFacilities(numericRoad, entities, 'auto_parking');
eq('运行时道路名丢失时由场景名保持泊车建模', runtimeLayout.parkingBays, 3);

done();
