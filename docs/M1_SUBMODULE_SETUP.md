# M1 Submodule 接入指南：Lanelet2（D2-01 文档化）

> 配套：[REQ_L3_DIR2_HDMAP.md v1.0](../REQ_L3_DIR2_HDMAP.md) §2.1 D2-01
> 配套：[M1_OSM_INTERFACE_CONTRACT.md](M1_OSM_INTERFACE_CONTRACT.md) §5（D2-09 消费本 submodule 产出的 .osm）

## 1. 拉取

```bash
# 在已有 .gitmodules 条目的前提下：
git submodule update --init --recursive third_party/lanelet2

# 验证：
ls third_party/lanelet2/include/lanelet2_core/LaneletMap.h
# 预期：文件存在
```

锁定的远端：<https://github.com/fzi-forschungszentrum-informatik/Lanelet2.git>
锁定的分支：`master`（Lanelet2 无稳定 tag 体系，master 即稳定）

## 2. 编译依赖

Lanelet2 运行时依赖（按上游 README）：
| 依赖 | 版本下限 | 本项目现状 |
|---|---|---|
| C++14 | — | ✅ 项目用 C++20，满足 |
| CMake | 3.10 | ✅ 项目用 3.22+ |
| Eigen | 3.3 | ✅ third_party/eigen vendored |
| Boost | 1.71 | ❌ **缺失**——vendored 无，需 FetchContent 或 apt |
| pugixml | 1.8 | ❌ 缺失 |
| Glibmm / GTK | — | 仅 GUI 工具需要，runtime 不需要 |

**M1 不集成编译**，仅在 `.gitmodules` + `third_party/lanelet2/README.md` 占位。
**M2 集成时**需先评估 Boost 引入成本（apt vs FetchContent vs vendored）。

## 3. 与现有 esmini 子模块的对比

`third_party/esmini` 已落地（hash `-77028d83e6d402de52f7187e5384d1b1c02b7d1f`），其集成模式可参考：
- `CMakeLists.txt` 第 ~370-450 行：`add_subdirectory(${_esmini_path})` + `target_link_libraries(... esmini)`
- esmini 走完整的 add_subdirectory，Lanelet2 因依赖更多，建议走 **INTERFACE library + FetchContent** 模式（仅暴露头文件 + 链接 lanelet2_core / lanelet2_io / lanelet2_projection / lanelet2_matching 子集）

## 4. CI 集成策略（建议）

参照 esmini 的"四级 fallback"，Lanelet2 也建议：

```cmake
# CMakeLists.txt (M2 阶段落地)
set(_LANELET2_CANDIDATES
    "${CMAKE_CURRENT_SOURCE_DIR}/../lanelet2"           # 同级目录
    "${CMAKE_CURRENT_SOURCE_DIR}/build/_deps/lanelet2-src"  # FetchContent 缓存
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lanelet2"  # submodule
)
foreach(_cand IN LISTS _LANELET2_CANDIDATES)
    if(EXISTS "${_cand}/lanelet2_core/include/LaneletMap.h")
        add_subdirectory("${_cand}" "${CMAKE_BINARY_DIR}/_deps/lanelet2-build")
        break()
    endif()
endforeach()
```

## 5. 已知坑

1. **Lanelet2 无 tag 体系**：master 即稳定，但 API 偶有 breaking change（v1.1 → v1.2 时 `LaneletMap::load` 签名变过一次）。M2 实施时记录实际 commit hash 到 `third_party/lanelet2/README.md`。
2. **Boost 引入会拖慢 CI 编译**：从 0 到 vendored Boost 约 +2 分钟构建时间。评估是否值得。
3. **macOS / Windows 兼容性**：Lanelet2 在 macOS 上 Boost 路径可能踩坑；本项目主 target 是 Linux，暂不考虑。
4. **与 esmini OpenDRIVE 共存**：M1 阶段 esmini 仍读 .xodr 渲染地图，Lanelet2 走 .osm 做规划输入，两套链路并存（详见 [REQ_L3_DIR2_HDMAP.md v1.0](../REQ_L3_DIR2_HDMAP.md) §1.3 选型）。

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
