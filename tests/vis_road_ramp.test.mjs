import { createRoadView } from '../tools/flowboard/js/vis/view/RoadView.js';
import { eq, ok, done } from './test-utils.mjs';

console.log('=== ramp transition layout ===\n');

function build(network) {
  const scene = new THREE.Scene();
  const view = createRoadView(scene);
  view.build(network);
  return view;
}

const mainEdge = {
  id: 'main', type: 'highway', lanes: 4, lane_width: 3.5,
  nodes: [[0, 0, 0], [300, 0, 0]],
};

const bidirectionalView = build({ edges: [mainEdge] });
eq('双向道路绘制连续双黄中心线', bidirectionalView.getStats().doubleYellowCenterlines, 1);

const oneWayView = build({
  edges: [{ ...mainEdge, id: 'oneway-main', oneway: true }],
});
eq('同向道路不绘制黄色对向中心线', oneWayView.getStats().doubleYellowCenterlines, 0);

const mergeView = build({
  edges: [
    mainEdge,
    {
      id: 'entry-ramp', type: 'ramp_curve', lanes: 1, lane_width: 3.2,
      taper_length_m: 80,
      nodes: [[65, -9, 0], [150, 0, 0]],
    },
  ],
});
eq('平行贴合的入口匝道生成一段加速车道', mergeView.getStats().rampTransitions, 1);
ok('匝道路面、渐变并道线与导流线保持合批渲染',
  mergeView.getRoadGroup().children.length <= 4);

const divergeView = build({
  edges: [
    mainEdge,
    {
      id: 'exit-ramp', type: 'ramp_curve', lanes: 1, lane_width: 3.2,
      taper_length_m: 80,
      nodes: [[150, 0, 0], [235, -9, 0]],
    },
  ],
});
eq('平行分出的出口匝道生成一段减速车道', divergeView.getStats().rampTransitions, 1);

const turnView = build({
  edges: [
    mainEdge,
    {
      id: 'turn-link', type: 'ramp_curve', lanes: 1, lane_width: 3.2,
      nodes: [[300, 0, 0], [320, -18, 0], [320, -60, 0]],
    },
  ],
});
eq('仅在主路端点相接的普通转弯不误绘加速车道',
  turnView.getStats().rampTransitions, 0);

done();
