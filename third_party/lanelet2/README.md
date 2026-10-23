# Lanelet2 源码（占位目录）

> **本目录本身不含 Lanelet2 源码** —— 只有这份说明。`cmake/Lanelet2Standalone.cmake`
> 会按候选链找源码，找不到就 `WARNING` 并把 flowsim 的"真 `lanelet::Id`"路径**编译期
> 关闭**（退回合成 ID，仿真行为仍然正确）。

## ⚠️ 2026-10-24 更正：别再跑 `git submodule update --init`

本目录历史上被写成"Lanelet2 submodule 占位"，`.gitmodules` 里也有一条
`[submodule "third_party/lanelet2"]` —— 但**索引里从来没有对应的 gitlink**
（`git ls-files -s third_party/lanelet2` 只有这份 README，mode 100644），
所以那条命令必然失败（`pathspec did not match`）。该悬空声明已从 `.gitmodules` 移除。

## 两条能真正跑通的路

```bash
# 路 1（本地验证"真 ID"路径，推荐）：configure 期 FetchContent 拉 master
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFLOWENGINE_FETCH_LANELET2=ON
cmake --build build -j"$(nproc)"
FLOWENGINE_FETCH_LANELET2=ON bash build.sh release   # 或让 build.sh 把开关转发给插件子项目
# 源码落到 build/_deps/lanelet2-src（候选链第二级），插件 configure 会直接命中、不重复拉

# 路 2：手动放源码（离线/要锁版本时）
git clone --depth 1 https://github.com/fzi-forschungszentrum-informatik/Lanelet2.git \
    third_party/lanelet2
ls third_party/lanelet2/lanelet2_core/include/lanelet2_core/LaneletMap.h   # 验证
```

**默认 OFF**：configure 期联网是本仓库明令避免的失败形态（2026-10-22 三个 CI job
因 `libeigen3-dev` 漏装退到联网下载、网络一抖整片红，见 `ci-eigen-gate`）。
CI 一律不开这个开关，仍走"编译期关闭"的默认路径。

编译依赖（路 1/路 2 都要，Debian/Ubuntu）：`libboost-filesystem-dev`、`libeigen3-dev`、
`libpugixml-dev`、`libgeographiclib-dev`。

上游：<https://github.com/fzi-forschungszentrum-informatik/Lanelet2>
锁定分支：`master`（Lanelet2 无稳定 tag 体系）。

## 什么时候才需要它

- 只有 flowsim 启动期真去 load `lanelet.osm`、要拿**真 `lanelet::Id`** 时才需要；
- M1 工具链（`tools/json_to_lanelet.py` + `ci/gates/lanelet_consistency_check.py`）是**纯
  Python**，不依赖本目录；
- 地图契约见 `docs/M1_OSM_INTERFACE_CONTRACT.md`，接入指南见 `docs/M1_SUBMODULE_SETUP.md`。