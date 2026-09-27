# Sub-Agent 派工 Brief 模板

> 父 agent 在派任何子代理任务前，**复制下方的 brief 模板并完整填空**。
> 本文件不是说明书，是规范 + 模板 + 反例对照。

---

## 1. 为什么需要这份模板

最近 10 个 commit 审阅出了两次同型事故，都是**子代理超出授权范围**：

| Commit | 子代理 | 越界内容 | 父 agent 处置 |
|--------|--------|---------|---------------|
| 322e66c (D2-07) | Einstein | 重写 4 个 docs/book/ + 新增 2 个 + 改 BOOK.md 索引 | `git checkout --` revert，越界内容暂存 `/tmp/_staging/` |
| e13a33f (D2-08) | Hegel | 重写 9 个 docs/book/ + 新增 2 个 | `git checkout --` revert，越界内容暂存 `/tmp/_staging2/` |

两次的根因完全一致：**派工 brief 里只禁了 HANDOFF\*.md / \_reports/\*.md，没明确禁 docs/book/**，子代理的"我顺手把文档也更新了"就被这一道口子放进了 commit。

CLAUDE.md 已经从**事后**两条补强：commit body 必含 Removed: 段（重构类）、commit body 自述 vs git diff --stat 必须一致。这解决的是**事后可检测**，但救不回改动本身。

这本书新增的**事前**防线：**每次派工前必须复制模板 + 填空，让"禁止做的事"显式写进 brief 而不是隐式默会**。

---

## 2. Brief 模板（核心）

复制下面整段、就地填空。**任何空白字段都视为派工未完成**。

```markdown
## Sub-task: <代号 + 简短名字>

### Objective
（这次要做什么，1~3 句。可被验收的动词开头：实现 / 修 / 加 / 重构 / 删除）

### Scope
（明列出本次允许改的文件清单；不在清单里的文件即使相关也**不许碰**）

允许改：
- `<路径>` —— <改什么 / 为什么>
- `<路径>` —— <改什么 / 为什么>

不允许碰（红线）：
- `docs/book/**` —— 文档治理范畴；如需更新章节先与父 agent 对齐并显式列在下面"显式授权"段
- `BOOK.md` —— 索引真源，与 docs/book/ 绑定
- `CLAUDE.md` —— 全局规范，需父 agent 协调
- `.github/workflows/**` —— CI 守门本身
- `config/pipeline*.json` —— 拓扑/契约真源（除非任务本身是改契约）
- 任何本次未列出的文件

### Deliverable
（产出物长什么样。代码？测试？文档？commit？几个？）

- 文件清单（最终落盘路径）
- 验证命令及预期输出
- commit message 草案（如父 agent 让子代理负责 commit）

### Acceptance criteria
（验收项，每条都要可被执行 / 可 grep）

- [ ] `pytest tests/test_xxx.py` 全 PASS
- [ ] `python3 ci/gates/xxx.py` 绿
- [ ] `git diff --stat` 与本 brief 列出的"全部改动文件"**逐项一致**（见 evidence）
- [ ] 编译零新增 warning
- [ ] 注释/隐式改动**列入 evidence 段**，不藏

### Constraints（红线）
（不能做的事。每条都是经验证的高频越界模式。）

- ❌ 不许改 docs/book/、BOOK.md、CLAUDE.md、.github/workflows/ —— 上面 Scope 已列
- ❌ 不许"顺手改进"邻近模块 —— 任务范围外 = 父 agent 收回另派
- ❌ 不许改 git history（force push / amend 已发布 commit）
- ❌ 不许自述数字与实际不符："改了 N 个文件"必须等于 `git diff --stat` 的文件数
- ❌ 不许引入第二份实现 / 第二份 CI 脚本
- ❌ 重构/替代类改动必须**同 commit 删旧**，不许留 `_v2` / `_new` / `_bak`

### Evidence requirement（commit body 必含）
（要 grep / read 的证据；不在这里列的改动 = 隐式改动 = 违规）

本次实际改动的**全部文件**（含次要改动如注释更新、cmake 注册）：

| 文件 | 改动摘要 | 行数 |
|------|---------|------|

仓门验证（父 agent 联合验证时执行，子代理自验后再交）：

- `git show --stat` 输出（粘贴在下面）
- 上述"全部改动文件"清单覆盖 `git diff --stat` 100%
- 相关 gate / 单测命令与退出码
```

---

## 3. 约束清单（高频越界模式）

| 越界 | 为什么是越界 | 如何避免 |
|------|-------------|---------|
| 改 `docs/book/` 章节 | CLAUDE.md / BOOK.md 文档治理范畴，章节重写是独立 commit 工程 | brief Scope 段明文禁 `docs/book/**`；如实在需要，作为单独 sub-task 派，与本次任务分开 |
| 改 `BOOK.md` 索引 | `BOOK.md` 是 docs/book/ 的真源索引，章节重排期才动 | 同上；如需增/删条目，让父 agent 统一改 |
| 改 `CLAUDE.md` | 全局规范，影响所有 agent | 子代理一律无写权限；如发现 CLAUDE.md 需补强，回报父 agent |
| 改 `.github/workflows/` | CI 守门本身；改 gate 需独立 review | brief Scope 段明文禁；如需加 gate，作为独立 sub-task 派 |
| 隐式次要改动（如改了 cmake/索引但 body 不提） | commit body 自述与实际 diff 不一致，破坏可追溯 | evidence 段要求**列全部改动文件**，含"仅注释更新 / cmake 注册"；commit body 复制此段 |
| 自述数字不符（"5 文件"实际 6 个） | 同上，322e66c 教训 | evidence 段表格行数必须等于 `git diff --stat` 行数，父 agent 联合验证时校验 |
| force push / amend 已发布 commit | 破坏可追溯、影响下游 | 子代理一律无 force-push 权限；本地 commit 可 amend，但推到 main 前需父 agent 确认 |
| 引入第二份实现（如 `_v2` / 同名不同目录） | CLAUDE.md 重构铁律禁止 | brief Constraints 段明文：禁止 `_v2` 后缀、独立 commit 删旧 |
| 跨模块"顺手改进" | 越界重灾区 —— 子代理看到一个相关问题就修 | brief Constraints 段明文："任务范围外的问题写进报告，不修" |
| 改 `config/pipeline*.json`（除非任务本身） | 拓扑真源；改一处多节点契约漂移 | 默认禁；任务本身就是改契约时，brief 要写明 "改哪几条 + 影响哪些 s_inputs/s_outputs" |

---

## 4. 反例对照（错误 vs 正确 brief）

### 4.1 D2-07 反例（错误 brief，导致越界）

```markdown
## Sub-task: IDL 加 obs_lane_match_hint 字段

### Objective
msg/adas_msgs.msg 的 Obstacle struct 末尾加一个 bool 字段 obs_lane_match_hint。

### Scope（缺！只口头说"改 IDL"）
（这里没列允许改 / 不许碰的文件清单）

### Deliverable
改完编一下，断言对得上。

### Acceptance
CI 绿。
```

**问题**：

- Scope 字段缺失 → Einstein 不知道什么能改什么不能
- 没有 Constraints 段 → 子代理自行扩展到"顺手把文档也写一下"
- 没有 evidence 段 → 隐式改了 BOOK.md 索引没人发现

### 4.2 D2-07 正例（修正后的 brief —— 实际写就该这么写）

```markdown
## Sub-task: A — IDL 扩展 obs_lane_match_hint 字段

### Objective
实现 docs/REQ_L3_DIR2_HDMAP.md §4.2 FR-RT-05：Obstacle struct 末尾加
`bool obs_lane_match_hint`，codegen 自动 bump SCHEMA_VERSION，
加 36 个 round-trip 单测覆盖新字段。

### Scope
允许改：
- `msg/adas_msgs.msg` —— 加字段 + 注释
- `tools/msg_codegen.py` —— `--schema-state` flag + stateful 版本号持久化
- `CMakeLists.txt` —— codegen custom_command 加 flag
- `tests/test_obstacle_hint_field.c` —— 新文件，36 个 round-trip assert
- `ci/gates/msg_layout_check.py` —— KNOWN_MISMATCHES (34,36) → (35,40)

不允许碰：
- ❌ `docs/book/**` / `BOOK.md` —— 文档治理范畴
- ❌ `CLAUDE.md`
- ❌ `.github/workflows/**`
- ❌ `modules/adas_nodes/perception_fusion_node.cpp` —— 那是 sub-task B 的范围

### Deliverable
- 5 个文件改动（CMakeLists / msg_layout_check / Obstacle.msg / msg_codegen / 新增单测）
- `pytest tests/test_obstacle_hint_field.c` 36/36 PASS
- 编译 EXIT=0，零新增 warning

### Acceptance
- [ ] test_obstacle_hint_field 36/36 PASS
- [ ] msg_layout_check 自测绿
- [ ] type_id 双判真逻辑验证（旧消费者收到 SCHEMA_INCOMPATIBLE 是预期）
- [ ] 7 gates 全绿

### Constraints
- ❌ 不改 docs/book/ / BOOK.md / CLAUDE.md / .github/workflows/
- ❌ 不改 fusion_node / fusion_lane_hint.h（sub-task B 范围）
- ❌ 不"顺便"加其它字段、加 docstring 之外的大段文档
- ❌ evidence 段列全部改动文件，含 cmake 注册 / KNOWN_MISMATCHES 调整

### Evidence
实际改动列表（commit body 复制此表）：
| 文件 | 改动 | 行数 |
|------|------|------|
| msg/adas_msgs.msg | +8 行（新字段 + 注释） | +8 |
| tools/msg_codegen.py | +58 行（stateful + --schema-state） | +58/-3 |
| CMakeLists.txt | +10 行（custom_command flag + ctest 注册） | +10 |
| ci/gates/msg_layout_check.py | KNOWN_MISMATCHES 更新 | +2/-2 |
| tests/test_obstacle_hint_field.c | NEW | +219 |

合计 5 个文件，与 `git show --stat` 行数一致。
```

### 4.3 D2-08 的差异（同样事故，第二次）

D2-08 (e13a33f) 的 brief 写了 Constraints：**`❌ 不改 HANDOFF*.md / docs/_reports/*.md`**，但**漏了 docs/book/** → Hegel 同样越界。

这印证了第三节表格里的核心教训：**红线要"全列"而不是"猜该列什么"**。默认就该列出 `docs/book/**` + `BOOK.md` + `CLAUDE.md` + `.github/workflows/**` 四个最常越界的目标。

---

## 5. 派工流程

```
[父 agent 决定要派子代理]
    │
    ▼
[复制 §2 模板 + 完整填空]
    │ scope / constraints / evidence 三段一字不能省
    ▼
[检查：Scope 段允许改的文件清单是否 < 8 个]
    │ 超过就要么拆细粒度任务，要么评估"是不是该自己干"
    ▼
[派工]
    ▼
[子代理回报告时，父 agent 联合验证步骤]
    ├── 跑 brief 列的 acceptance 命令
    ├── 跑 `git show --stat` 对照 evidence 表逐行
    ├── 跑 `python3 ci/gates/book_guard.py --warn-only`
    └── 跑 brief 未列但历史高发的越界检查（grep docs/book/CLAUDE.md/.github/workflows）
    ▼
[不一致 → 重派 / revert 越界]
    ▼
[合 commit，body 引用 brief 或粘贴 key 约束段]
```

派工前**最后一道自检**：把 brief 的 Constraints 段读完，问自己"如果子代理贪心做了额外的事，哪一条能挡住？" 没有就加。

---

## 6. 与现有守门的关系

| 守门层 | 文件 | 抓什么 | 时机 |
|--------|------|-------|------|
| 派工 brief 模板 | **本文档** | 事前约束子代理范围（不让越界进 commit） | 派工前 |
| CLAUDE.md commit 纪律 | `CLAUDE.md` 第七节（Removed: 段）+ commit body 一致性铁律 | 事后可检测（重构类必删旧、body vs diff 一致） | 合 commit 时 |
| CI gate | `ci/gates/book_guard.py` | 文档风格守门（行号密度 / 函数符号堆叠 / 单章长度 / 大突变） | 合 commit 后 CI |
| CI gate（其它）| `ci/gates/{topic_contract,sensor_wiring,zombie_ban,...}.py` | 拓扑/契约/禁尸守门 | 合 commit 后 CI |

四层关系：

- **事前** brief 模板挡住"不让越界进 commit"
- **事中** CLAUDE.md 纪律让"重构必删旧 / body vs diff 一致"可被父 agent + reviewer 校验
- **事后** book_guard 守文档风格，topic_contract / sensor_wiring / zombie_ban 守拓扑

**任何一层失效另一层兜底**：

- brief 漏了 docs/book/ → book_guard 仍可在 CI 阶段兜住（虽然越界内容已被 revert）
- CLAUDE.md 纪律被忽略 → 联合验证 step 的 `git show --stat` 对照能补抓
- CI 某 gate 失红 → 父 agent 联合验证 step 手动跑同样的命令补抓

---

## 7. 一句话总结

**派工前复制 §2 模板并完整填空；Constraints 必含 docs/book/BOOK.md/CLAUDE.md/.github/workflows 四个最常越界目标；evidence 必含 `git diff --stat` 的全部文件逐项；commit body 复制 evidence 段。**

---

*文档定位：派工规范 —— 父 agent / 总指挥。子代理只读自己的 brief，不读这份模板。*
