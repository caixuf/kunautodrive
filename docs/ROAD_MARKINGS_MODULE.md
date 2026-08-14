# Road Markings 模块（路面标线 / 道路设施）

> 本模块负责在 3D 场景里渲染**道路标线**（中心线、车道分隔线、边缘线、停止线、斑马线、
> 导向箭头）与**道路设施**（停车位、考试桩）。所有标线方向按国标 **GB 5768.3**
> 《道路交通标志和标线 第3部分：道路交通标线》绘制。
>
> 涉及文件：
> - `tools/flowboard/js/vis/view/RoadView.js` — 沿路标线（中心线/分界线/边缘线）
> - `tools/flowboard/js/vis/view/ConnectorView.js` — 路口标线（斑马线/停止线/导流线）
> - `tools/flowboard/js/vis/view/RoadFacilityView.js` — 信号灯推导的停止线/斑马线 + 设施

---

## 1. 架构位置

```
SceneDirector
  └─ Layer 树
       └─ road 层
            ├─ RoadView            （沿路标线：中心线/分界线/边缘线）
            └─ ConnectorView       （路口：铺装/斑马线/停止线）
            └─ RoadFacilityView    （设施：停止线/斑马线/停车位/箭头/桩）
```

- 三个 View 都只读 `SceneStore` 的 `roadNetwork`/`entities`，单向依赖，不反向调导演。
- 标线统一用 `InstancedMesh`（`RoadFacilityView` 的 `marks` 数组 + `ConnectorView` 的
  `crossInstances`/`stopInstances`）合批，保持低 draw call。
- 坐标一律 `worldToThree`（ENU→THREE），禁止裸 `-y` 翻转（门禁 `vis_coord_property.test.mjs`）。

---

## 2. 数据契约

`RoadFacilityView` 的路面标记统一抽象为：

```javascript
// addMark(layout, x, y, heading, length, width)
layout.marks.push({ x, y, heading, length, width });
```

渲染时：`rotation.y = headingToRotationY(heading)`、`scale.set(length, 1, width)`。
即 **`heading` = 标记长轴方向**，`length` 沿长轴、`width` 垂直长轴。

`ConnectorView` 的实例化条带同理（`rotY` = 长轴方向，`len` 沿长轴，`w` 垂直）。

---

## 3. 标线类型与国标对照

| 标线 | 国标条目 | 正确方向 | 实现位置 |
|------|----------|----------|----------|
| **斑马线（人行横道线）** | 5.8 | **条纹平行于道路中心线**（与车同向）；条纹沿道路长度短（交叉口≥2m，路段≥3m）；条纹宽 40~45cm，间距 60~80cm | `RoadFacilityView.addCrosswalk`、`ConnectorView._buildJunctionPatch/_buildJunctionCap` |
| **停止线** | 5.19 | 白实线，**横跨来车方向**半幅路面（垂直行车）；位于人行横道外侧约 2~3m | `RoadFacilityView`（信号灯推导）、`ConnectorView` |
| **导向箭头** | 5.13 | 指向允许行进方向 | `RoadFacilityView`（urban/exam 路，箭杆沿 `heading`） |
| **可跨越对向车行道分界线** | 6.1 | 双向道路中心**双黄实线** | `RoadView`（`hasOpposingTraffic` 时双黄） |
| **可跨越同向车行道分界线** | 5.2 | 同向车道间**白虚线** | `RoadView.dashedLine` |
| **车行道边缘线** | 5.4 | 路缘**白实线**（外沿实线） | `RoadView.edgeLine` |

---

## 4. 实现要点

### 4.1 斑马线（2026-08-14 方向修正）

历史：早期实现条纹长轴**垂直道路**（横跨整幅路面）→ 观感像"给车走的横条"，与国标相反。
已改为：

- **长轴沿道路**（`heading`/`rotY = directionToRotationY(ux, uz)`，不加 `π/2`）。
- **沿道路长度短**：`CROSSWALK_LENGTH = 2.5m`（交叉口 ≥2m 达标）。
- **跨道路方向铺条**：条带沿法向 `n̂=(-uz, ux)` 排列，间距 `STRIPE_W + GAP`，铺满 `roadW`。

```javascript
// RoadFacilityView
for (const across of [-…0…+]) {
  const p = offsetPoint(center, heading + π/2, across, 0);  // 跨道路排布
  addMark(layout, p.x, p.y, heading, CROSSWALK_LENGTH, CROSSWALK_STRIPE_W); // 长轴沿道路
}
```

### 4.2 停止线（保持垂直）

停止线与斑马线方向**刻意相反**：停止线长轴垂直行车（横跨来向半幅）。实现上
`heading + π/2`（`RoadFacilityView`）或 `rotY + π/2`（`ConnectorView`），
避免与斑马线同方向被误判为标线横条。

### 4.3 方向感知（N-S / 任意角道路）

- 标线方向来自**道路中心线切线**（`headingBetweenPoints` / `directionToRotationY`），
  对 E-W、N-S、斜路、OSM 任意角道路自洽，不写死世界轴。
- 注意：`ConnectorView._buildJunctionCap`（旧简化路径）按 `+X` 轴铺路面，仅用于
  `intersection` 类型 edge；斑马线部分已按规范改，但整体仍未旋转对齐（已知槽点，见 §6）。

---

## 5. 门禁与测试

- `npm run vis:check:all` — 全量渲染门禁。
- `tests/vis_road_facility.test.mjs` — 停止线/斑马线/停车位/桩/箭头 数量 + 位置断言，
  含 **「斑马线条纹平行于道路（heading=0）且长度短（2.5m）」** 方向断言（防止改回错误方向）。
- `tests/vis_road_ramp.test.mjs` — 匝道标线（双黄中心线/白虚线边线/加速车道）。
- 新增标线语义必须在上述测试补断言，否则方向回归无感（斑马线就是教训）。

---

## 6. 已知槽点 / 待办

- [ ] `ConnectorView._buildJunctionCap` 简化斑马线假设道路沿 `+X`，未旋转对齐
      （真实数据用 `_buildJunctionPatch` 数据驱动路径，已正确；此路径待清理或对齐）。
- [ ] 导向箭头目前只画**直行**箭头；可扩展为按车道转向（直行+左/右转、左转待转区箭头，
      对应 GB 5768.3 5.13 组合箭头）。
- [ ] 非机动车横道线（5.18）、减速带/震荡线、渠化导流线（6.5）尚未渲染。
- [ ] 标线随车道数自适应的配色/宽度仍有硬编码（`LINE_W`/`STOP_LINE_W`）。

---

## 7. 演进路线（详见 docs/3D_DETAIL_ROADMAP.md）

标线方向已合规；下一步按路线图 P0（组合箭头）→ P1（路口圆角/渠化岛）推进，
每步「改 View + 补测试断言 + 过 vis:check:all」。
