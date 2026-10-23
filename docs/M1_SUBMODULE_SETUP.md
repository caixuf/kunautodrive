# M1 Submodule 接入指南：Lanelet2（D2-01 文档化）

> 配套：`REQ_L3_DIR2_HDMAP.md` v1.0 §2.1 D2-01 —— **该文档不在本仓库内**
> （仓库根目录无此文件，git 全历史亦无），属于外部需求文档，需要时向需求方索取。
> 配套：[M1_OSM_INTERFACE_CONTRACT.md](M1_OSM_INTERFACE_CONTRACT.md) §5（D2-09 消费本 submodule 产出的 .osm）

## 1. 拉取

> ⚠️ **2026-10-24 更正**：本节原先写"在已有 .gitmodules 条目的前提下跑
> `git submodule update --init --recursive third_party/lanelet2`" —— 但那个 submodule
> **从来没有对应的 gitlink**（`git ls-files -s third_party/lanelet2` 只有占位 README，
> mode 100644），命令必然以 `pathspec did not match` 失败。悬空的 `.gitmodules` 声明
> 已移除，改用下面两条**实际跑通过**的路。

```bash
# 路 1（推荐，本地验证"真 lanelet::Id"路径）：configure 期 FetchContent 拉 master 到 build/_deps
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFLOWENGINE_FETCH_LANELET2=ON
cmake --build build -j"$(nproc)"
FLOWENGINE_FETCH_LANELET2=ON bash build.sh release   # build.sh 会把开关转给插件子项目
# 验证：configure 输出应出现 "Lanelet2 source: …/build/_deps/lanelet2-src"，
#      且不再出现 "Lanelet2 source not found" 那条 WARNING

# 路 2（离线 / 要锁版本）：手动放源码
git clone --depth 1 https://github.com/fzi-forschungszentrum-informatik/Lanelet2.git \
    third_party/lanelet2
ls third_party/lanelet2/lanelet2_core/include/lanelet2_core/LaneletMap.h
# 预期：文件存在
```

**默认 OFF**：configure 期联网是本仓库刻意避免的失败形态（2026-10-22 三个 CI job 因
`libeigen3-dev` 漏装退到联网下载、网络一抖整片红，见 `ci-eigen-gate`）。
CI 一律不开这个开关，继续走"源码缺失 → 编译期关闭真 ID 路径"的默认分支。

锁定的远端：<https://github.com/fzi-forschungszentrum-informatik/Lanelet2.git>
锁定的分支：`master`（Lanelet2 无稳定 tag 体系，master 即稳定）

## 2. 编译依赖

Lanelet2 运行时依赖（上游 README + 本仓库 `cmake/Lanelet2Standalone.cmake` 实际要求）。
2026-10-24 本机逐项核查：
| 依赖 | 本项目现状 |
|---|---|
| C++17 | ✅ 项目用 C++20，满足（`kun_lanelet2_core` 显式 `cxx_std_17`） |
| CMake | ✅ 3.16+ |
| Eigen | ✅ 系统 `/usr/include/eigen3`（或 vendored） |
| Boost（filesystem） | ✅ 已装（`find_package(Boost REQUIRED COMPONENTS filesystem)`） |
| pugixml | ✅ 已装（缺 → configure `FATAL_ERROR`：apt `libpugixml-dev`） |
| GeographicLib | ✅ 已装（缺 → configure `FATAL_ERROR`：apt `libgeographiclib-dev`） |
| Glibmm / GTK | 仅上游 GUI 工具需要，本仓库不编 → 不需要 |

缺失依赖的**表现**：只有把源码放进候选路径时才会 `FATAL_ERROR`；源码缺失时只是
`WARNING` + 编译期关闭真 ID 路径（默认路径不受影响）。

**M1 只做工具层**（`tools/json_to_lanelet.py` + `ci/gates/lanelet_consistency_check.py`，
纯 Python，不依赖本 submodule）。**M2 起** flowsim 启动期 load `lanelet.osm` 时才需要
上面两条路之一。

## 3. 与现有 esmini 子模块的对比

`third_party/esmini` 已落地（hash `-77028d83e6d402de52f7187e5384d1b1c02b7d1f`），其集成模式可参考：
- `CMakeLists.txt` 第 ~370-450 行：`add_subdirectory(${_esmini_path})` + `target_link_libraries(... esmini)`
- esmini 走完整的 add_subdirectory，Lanelet2 因依赖更多，建议走 **INTERFACE library + FetchContent** 模式（仅暴露头文件 + 链接 lanelet2_core / lanelet2_io / lanelet2_projection / lanelet2_matching 子集）

## 4. CI 集成策略（**已落地**，2026-10-24）

实现在 `cmake/Lanelet2Standalone.cmake`（**不是** CMakeLists 里的内联片段）。与早期
"建议草图"的三点差别，都是实测踩出来的：

| 早期建议 | 实际实现 | 为什么 |
|---|---|---|
| `add_subdirectory(${_cand} …)` | **只 `FetchContent_Populate` 源码**，随后 `file(GLOB … lanelet2_core/src/*.cpp)` 直接编 | Lanelet2 自己的 `CMakeLists.txt` 依赖 `mrt_cmake_modules`，`add_subdirectory` 直接 configure 失败（本仓库不带 ROS/mrt） |
| 候选链查 `include/LaneletMap.h` | 查 `lanelet2_core/include/lanelet2_core/LaneletMap.h` | 上游真实布局是这样；旧草图的路径少一层、永远命中不了 |
| 找不到就联网 clone | **显式开关** `FLOWENGINE_FETCH_LANELET2`（默认 OFF）才联网 | configure 期联网是本仓库刻意避免的失败形态（2026-10-22 三个 CI job 齐红那次，见 `ci-eigen-gate`）；默认走"源码缺失 → 编译期关闭真 ID 路径" |

候选链（三级，`kun_find_lanelet2_src()`）：`third_party/lanelet2` → `build/_deps/lanelet2-src`
（FetchContent 缓存，插件 configure 会命中它、不重复拉）→ 开关打开时联网拉 master。

**CI 现状**：不开开关 → 编译期关闭真 ID 路径（`flowsim` 用合成 `llt_id`，行为正确）。
要进 CI 覆盖真 ID 路径，需先给 CI 装 `libpugixml-dev` + `libgeographiclib-dev`，并接受
拉源码的时间/网络成本 —— 这一步**尚未做**。

## 5. 已知坑

1. **Lanelet2 无 tag 体系**：master 即稳定，但 API 偶有 breaking change（v1.1 → v1.2 时 `LaneletMap::load` 签名变过一次）。M2 实施时记录实际 commit hash 到 `third_party/lanelet2/README.md`。
2. **Boost 引入会拖慢 CI 编译**：从 0 到 vendored Boost 约 +2 分钟构建时间。评估是否值得。
3. **macOS / Windows 兼容性**：Lanelet2 在 macOS 上 Boost 路径可能踩坑；本项目主 target 是 Linux，暂不考虑。
4. **与 esmini OpenDRIVE 共存**：M1 阶段 esmini 仍读 .xodr 渲染地图，Lanelet2 走 .osm 做规划输入，两套链路并存（详见 `REQ_L3_DIR2_HDMAP.md` v1.0 §1.3 选型）。

## 6. M1 阶段不强制 init 的原因

- M1 工具链（`tools/json_to_lanelet.py` + `ci/gates/lanelet_consistency_check.py`）**纯 Python**，不依赖 Lanelet2 库
- Lanelet2 仅在 M2 `flowsim` 启动期 `lanelet::load()` 时才需要
- CI gate 验证的是 .osm 文件本身的 schema，不验证 Lanelet2 能否 load（那是 M2 验收 A3）

## 7. M2 接入 checklist

- [ ] `git submodule update --init third_party/lanelet2`，记录实际 commit hash
- [ ] CMakeLists.txt 加 INTERFACE library（参考 §4）
- [ ] apt 装 libboost-dev libpugixml-dev 或 FetchContent 引入
- [ ] `third_party/lanelet2/README.md` 更新 commit hash
- [ ] 跑 A3：`flowsim` 启动日志含 `loaded N lanelets`
- [ ] M1 工具链产物（.osm）能被 Lanelet2 解析（一次性验证：写 5 行 wrapper 调 `lanelet::load` 测通即可）
