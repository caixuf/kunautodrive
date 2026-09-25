# Lanelet2 submodule (D2-01 placeholder)

> **本目录是占位**：Lanelet2 submodule 实际内容需运行：
> ```bash
> git submodule update --init --recursive third_party/lanelet2
> ```
> 本期（M1）不强制 init，因为：
> - Lanelet2 仅 M2 起在运行时被 `flowsim` 加载
> - 编译依赖 Eigen + Boost + pugixml + Glibmm，CI 集成耗时长（详见 `docs/M1_SUBMODULE_SETUP.md`）
> - M1 工具链（`tools/json_to_lanelet.py` + `ci/gates/lanelet_consistency_check.py`）**不**依赖本目录存在
>
> 上游：[fzi-forschungszentrum-informatik/Lanelet2](https://github.com/fzi-forschungszentrum-informatik/Lanelet2)
> 锁定分支：`master`（Lanelet2 唯一长期维护分支，无稳定 tag 体系）
> 锁定提交：M2 实施时按 `git submodule update` 实际拉取的 commit hash 记录到本 README

## M2 接入时机

- M1 仅文档化（不构建）
- M2 `flowsim_node.cpp` 启动期调 `lanelet::LaneletMap::load("maps/<name>/lanelet.osm")` 时才需要 init
- 若 `lanelet_match_pose()` 调用的 submodule 路径找不到 → 启动失败 + 错误日志带文件路径（见 REQ_L3_DIR2_HDMAP.md FR-RT-01）

## CI 集成策略

参照 `CMakeLists.txt` 中 esmini 的"候选路径 → build cache → third_party → FetchContent"四级 fallback（详见 `docs/M1_SUBMODULE_SETUP.md`）：
- 首选：本仓库 third_party/lanelet2（init 后存在）
- 次选：build/_deps/lanelet2-src（cmake FetchContent 缓存）
- 兜底：网络 clone（CI 默认行为）
