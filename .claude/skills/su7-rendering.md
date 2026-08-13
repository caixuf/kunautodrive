---
name: su7-rendering
description: SU7 模型渲染经验：新能源车牌按 GA 36-2018 规格渲染 + 模型实测 ChePai 节点定位 + Matrix4.makeBasis 贴尾门斜面、SU7 保留原厂方向盘（interior.009）门控、glTF 复用不重复 clone。改 SU7/车牌/方向盘渲染时使用。
---

# SU7 模型渲染

沉淀自 SU7 车牌/方向盘系列修复。改动前先看 [VIS_3D_RENDERING.md](../../docs/VIS_3D_RENDERING.md) §2-3。

## 什么时候用

- 改 `VehicleView.js` 的 `_applyNewEnergyPlate`、`_makeNewEnergyPlateTexture`、SU7 方向盘门控、`_cleanSu7Exterior`

## SU7 原厂方向盘（L600-603）

- `if (type !== 'su7') scene.add(_createSteeringWheel())`：SU7 是授权高精度模型，驾驶位自带原厂方向盘（`interior.009` 左舵），**不能叠程序化丑方向盘**。
- sedan/suv/truck 由 gen_models.py 生成只有四轮无内饰，需程序化补 `_createSteeringWheel()`。

## 车牌纹理（GA 36-2018，`_makeNewEnergyPlateTexture` L300-354）

- Canvas 1px=1mm：外廓 **440×140mm**、8 字符、字高 90mm、前两字宽 45/后六字 43mm、字距 9mm、第 2/3 字间 8mm 间隔圆点、黑框 2mm、上亮下深渐变绿底、黑字。
- **逐字符测量字宽横向压缩**，避免字符挤出板面（画布宽度不足会显示不全）。
- Linux 需 Noto Sans CJK SC 字体。

## 车牌定位（`_applyNewEnergyPlate` L364-384）

- **不硬编码**（旧 `x=±2.20,y=0.52` 嵌进保险杠/太高）。
- **必须解包 meshopt 压缩后读模型自带 ChePai 节点实测顶点**。ChePai 节点曾因 32 顶点合并进单个 bounding box 被误读为"车底 5.2m 长饰条"。
- **前牌**：垂直平面贴前保险杠凹槽（x≈2.627、y≈0.546、z≈0，0.428×0.133），`front.rotation.y = π/2` 正面朝 +X。
- **后牌**：随尾门**倾斜**（中心(-2.560,0.502,0)，0.428×0.135），用 `Matrix4.makeBasis(right, up, normal)` 把平面旋到贴合尾门斜面，`addScaledVector(normal, 0.003)` 前出 3mm 收进框内防 z-fight。
- 尺寸取 **ChePai 实测值而非国标 0.48×0.14**，恰好落进原厂预留框。

## 关键坑速查

1. 车牌位置不能硬编码，解包 meshopt 实测 ChePai 顶点。
2. 后牌在尾门**斜面**，makeBasis 旋转贴合。
3. 尺寸取模型实测非国标；字宽逐字压缩防显示不全。
4. SU7 保留原厂方向盘，`type !== 'su7'` 才注入程序化方向盘。
5. glTF `getModel()` 返回即 Group，别重复 clone（SU7 169k 三角）。

## 改完必跑

```
npm run vis:check:all
# 手动：刷新 flowboard，确认前后牌贴进预留框、字符完整、方向盘为原厂
```
