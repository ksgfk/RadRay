> - 适用: 2026-10-16 确认的 Render Framework CPU 回退修复计划
> - 权威: 经用户授权保存的修复计划与决策；阶段 0 已实施，阶段 1–4 已实施，阶段 5 未纳入，不作为当前框架契约
> - 锚点: `benchmarks/bench_material_scene/`, `modules/runtime/src/render_framework/renderer.cpp`, `modules/runtime/src/render_framework/binding_registry.cpp`, `modules/runtime/src/render_framework/material_template.cpp`, `docs/architecture/render-framework.md`, `docs/guide/performance.md`

# Render Framework CPU 回退修复计划

状态：阶段 0 已执行并记录基线；阶段 1–4 已实施（本文档末尾为实施结果与偏差）；阶段 5 未纳入本轮。基线提交 `06b8c236e10e5af0b2a17777ace9ebce4c767a07`（[重构验收](2026-10-16-render-framework-refactor-validation.md)后的当前版本）。
阶段 0 的原始对照、二进制与源码校验见[基线数据](2026-10-16-render-framework-regression-fix-baseline.json)与[基线 CSV](2026-10-16-render-framework-regression-fix-baseline.csv)。
原因与测量见[CPU 回退定位](2026-10-16-render-framework-regression-diagnosis.md)；本记录补充修复方向，不替换诊断数据。
计划作者只读了 `06b8c23` 的源码，未参与阶段 0 前的构建与测量；阶段 1 之后的目标仍为预估，须由各阶段实测确认。
当前契约见 [Runtime 宿主](../architecture/render-framework.md)、[帧与 GPU](../architecture/frame-and-gpu.md) 与 [性能测量](../guide/performance.md)。

本记录由用户明确授权保存为 `docs/history/2026-10-16-render-framework-regression-fix-plan.md`。

## 已确认决策

1. 阶段 5（减少 SceneDraw 每个 draw 的拷贝）本轮不纳入。该阶段会改 `AddDraw` 公共签名，且收集阶段已从 639 µs 降到 508 µs，不是本次回退主因；等阶段 1–4 验收后再定。
2. 本计划按用户授权存为本文档。
3. 本轮对照基线取开启 `RADRAY_ENABLE_PROFILER` 的 Release 构建（诊断是 profiler OFF）；不再另做 profiler OFF 构建，绝对值差异记为配置与机器差别。

## 目标与不变量

**目标**

- 在最终绑定值种数不变时，Compile 耗时不再随 draw 数线性增长。
- 完成处理的 CPU 成本回到个位数 µs。
- PrepareRecord 不慢于基线（`f20c56f` 加 sampler 补丁）。

**保持不变**

- 显式所有权、真实 queue 完成加 command epoch 的回收协议。
- BindingRegistry 按兼容布局和最终值做完整组判等，descriptor 冗余保持为零。
- LRU 预算和资源退出语义不变，缓存不能延长外部资源驻留。
- AGENTS.md 硬规则：不加全局或函数内 static，不加 try/catch，不加仅供测试的生产字段，`.h/.cpp` 一一对应。

## 阶段 0：固化测量

诊断用的是临时 fixture 副本，先把它变成可重复的入口。

1. 在 `benchmarks/bench_material_scene` 加两类 Google Benchmark case：
   - CompileOnly 规模扫描：N1000、V3、F3、M=1/7/32/100/300，tint 取 `i%7`。
   - CompletionCpu：在 `CompleteFlightIfReady` 之后才恢复计时，排除 fence 等待。
   - 两类都用 PauseTiming/ResumeTiming 选计时段。
2. 在 `Renderer::Compile`、`CompileDraws`、`ProcessCompletions`、`Collect` 加 `RADRAY_PROFILE_SCOPE_N`。这样也能量化诊断里还没拆开的一项：Collect 和容器析构各占多少。
3. 记录当前版本作为本轮的对照，包括二进制 SHA-256 和完整命令。

**门槛：** 能复现报告中的规模曲线（M100 Compile 约 350 µs，完成 CPU 约 60–70 µs）。

**结果（已执行）：** 上述两类入口已落在 `benchmarks/bench_material_scene`，并在 Release benchmark 上跑通。
CompileOnly 固定 N1000/V3/F3、tint 取 `i%7`；CompletionCpu 在 `CompleteFlightIfReady` 之后才恢复计时。中位数：

| 组 | 后端 | M1 | M7 | M32 | M100 | M300 |
|---|---|---:|---:|---:|---:|---:|
| CompileOnly µs | d3d12 | 9.66 | 25.6 | 92.6 | 187.8 | 871.4 |
| CompileOnly µs | vulkan | 8.29 | 20.9 | 89.2 | 311.5 | 986.2 |
| CompletionCpu µs | d3d12 | 2.32 | — | — | 47.0 | — |
| CompletionCpu µs | vulkan | 2.63 | — | — | 56.1 | — |

最终 Material 值只有 7 种时编译耗时仍随 draw 数近似线性增长，形状与[回退定位](2026-10-16-render-framework-regression-diagnosis.md)一致，门槛按曲线形状判定为达成。
绝对值低于诊断（诊断 M100 约 343–352 µs、完成约 63–69 µs）：本次是 RTX 4080 且 `RADRAY_ENABLE_PROFILER=ON`，诊断是 RTX 3060 且 profiler OFF。
固定值种数不变时 Compile 仍随规模增长，符合本次要修的症状；单次重复的绝对值波动不小（d3d12 CompileOnly 的 M32–M300 重复间 CV 约 14–37%），本记录的结论只用曲线形状与量级，不依赖单点绝对值。
`Renderer::Compile`、`CompileDraws`、`ProcessCompletions`、`Collect` 已加 `RADRAY_PROFILE_SCOPE_N`，供 Tracy 分开 Collect 与容器析构；本次未采 Tracy trace。
原始二进制、命令、设备与源码/输出 SHA-256 见[基线数据](2026-10-16-render-framework-regression-fix-baseline.json)，逐项中位数见[基线 CSV](2026-10-16-render-framework-regression-fix-baseline.csv)；本机原始 JSON/日志在 `build_debug/fix-plan-baseline/`，未入库。

## 阶段 1：模板注册时预计算稳定布局事实（对应诊断第 2 点）

这些是布局存活期间不变的事实，现在每次 Resolve 都在重算。

- **按 group 预存稳定数据：** `CompiledMaterialTemplate::Group` 增加三项，都在 `Compile` 里一次性派生，不新建平行 schema。
  - 当前代码每帧在算的 `PipelineLayout*`。
  - 每个输入元素的 `GetParameterElementIndex` 结果。
  - 预填好的 dynamic offset 数组。现在每个 draw 都 `offsets.push_back({handle,0})`。
- **兼容身份驻留（intern）：** 在 BindingRegistry 中把 `ParameterGroupCompatibility` 驻留成小整数 id，按身份内容 hash 并完整比较，不按指针。模板保存这个 id。
  - Entry 比较时先比 id，Resolve 命中路径不再调用 D3D12 的 `GetParameterGroupCompatibility`，那个函数每次都会构建并排序 vector。
  - 原生 `AddBindings` 入口保持通用路径。
- **扁平化 Entry::Values：** `vector<vector<uint64_t>>` 改成一个连续数组加偏移。
- **缓存值身份：** RendererResources 在创建资源时缓存 `GetShaderParameterValueIdentity` 的结果，资源版本本身不可变。这是功能字段，不是测试字段。

**预期：** 单组 Resolve 命中从 D3D12 约 2.8 µs 降下来。诊断已说明这一项不是主因，所以排在主修复前面，只因为后面几个阶段依赖它。

## 阶段 2：Material/Sampler 组在材质版本上编译一次（主修复，对应诊断第 1 点）

1. **在材质上建缓存：** `State::Material` 增加 `[template][group]` 缓存，只覆盖 Domain 为 Material 或 Sampler 的组。
   - 每项保存持久的 writes/dependencies，在 `RegisterMaterial` 时构造。资源类型和 CBuffer 大小已经在这里验证过，帧内不再重复验证。
   - 每项还保存一个非拥有的 `DescriptorBlockId`。
2. **没有 immutable 输入的 sampler 组：** 缓存挂在模板上。
3. **帧内路径：** draw 没有对应域的 override 时：
   - 先用 `Bindings.IsValid(block)` 检查 generation。
   - 失效（被 LRU 淘汰或资源退出）就用存好的 writes 重新 Resolve，不重新展开输入。
   - 之后在本帧 pin 一次。依赖资源已由 `UseMaterial` 登记，不需要逐元素 `UseResource`。
4. **有 override 的 draw：** 继续走通用路径。同值 override 仍经 `StoreData` 去重，得到同一 ResourceId，在登记表里命中同一个组，现有的“同值 override 复用默认”行为不变。
5. **Draw 直接存绑定：** Draw 改为保存 `{group, DescriptorBlockId}`，不再必须经过 `Requests`。`Requests` 只留给 `AddBindings`。Record 直接 Bind 这个 block。

**预期：** 在 M100 下，900 次组展开里材质那 300 次变成数组索引加 generation 校验。

## 阶段 3：Scene/View 组在展开前合并

现在是先构造 writes/dependencies，再查 `groupRequests`。

- **统一 key：** SceneFrame、ViewPass 以及有 override 的组都按 `(TemplateId, group, 该组实际读取的有序 ResourceId 列表)` 建帧内备忘。
  - key 只含有序资源版本号；资源版本不可变，所以相等 key 必然解析出同一个 block。
  - Objects 不同或视图不同会落到不同 key；同一视图上的多个 pass 读到相同的 View 资源，命中同一条目。
  - 有 override 的组用 override 资源参与 key，同值 override 经 `StoreData` 去重后命中同一条目。
- **命中：** 直接复用 block（连 `UseResource` 也跳过，依赖已由 `AddView`/`AddPass`/`UseMaterial` 登记）。
- **未命中：** 才展开输入、验证 stride/kind、调用 Resolve。
- **备忘容器：** 帧级扁平 `vector<GroupMemoEntry>` 加资源 key 的 `vector<ResourceId>`；唯一组数量很小，用线性扫描，每帧只 `clear()`，保留容量，帧收集期间不建 hash 节点。

**预期：** 每帧 Scene/View 组只准备唯一的那几份（本负载 M100/V3 为 ViewPass 3 份、SceneFrame 1 份），每个 draw 只剩常数级的查找成本。

## 阶段 4：帧存储与回收（对应诊断第 3 点）

1. **使用清单改为戳记去重：** `Frame::Resources/Bindings/Materials/Templates` 四个 `unordered_set` 改成 vector。
   - 去重靠对应 Entry 上的 `uint64_t LastFrameStamp`。Renderer 内单调帧序号，编译是串行的，所以安全。
   - `UseResource` 等热循环里不再建 hash 节点。
2. **Draw 改为聚合赋值：** Draw 内不再嵌套 `vector<vector<dynamic offset>>`，改为一条 `DrawBinding{group, block, dynamic offset span}`。
   - `ProcessCompletions` 对 `frame->Draws` 调 `clear()`，析构各自释放，不再按历史峰值容量原地清理。
   - `AddDraw` 用整体 `std::move` 收纳描述符与取值，不再逐字段赋值。
   - 早期尝试过“帧内复用同一批 Draw、`AddDraw` 就地逐字段填充”，评审判定它只是把释放成本从完成窗移到 `AddDraw` 并多一次拷贝，已回退。
3. **Collect 改为增量处理：**
   - 计数归零的 Material/Template/Resource 推入候选队列，Collect 只处理候选，不再每次全表扫描。
   - BindingRegistry 维护按资源的依赖计数（反向索引），让 `HasResource` 变成 O(1)。当前 `RendererResources::Collect` 是 do-while 扫全部 slot，每个 slot 再调用 `HasResource` 扫全部 block。
   - 是否做这一项，先看阶段 0 中 Collect zone 的实际占比。

**预期：** 完成 CPU 在 M100 下回到个位数 µs。

## 阶段 5（本轮不纳入）：减少 SceneDraw 每个 draw 的拷贝

已确认不纳入本轮，留待阶段 1–4 验收后决定。

`RendererScenes::Geometry` 每次都会返回一份 `RendererDrawDescriptor` 拷贝。这份拷贝里有带 semantic 字符串的 `GeometryVertexLayout`。SceneDraw 还会给每个 draw 拷一份 `values.Parameters`。另外，PSO 备忘每次都要比较整个 layout。

可以改为注册持久几何 ID，并在帧级共享 draw values，但这会改 `AddDraw` 的公共签名。这部分属于收集阶段，不是本次回退的主因。

## 验证（每个阶段单独提交）

**正确性**

- `test_renderer_registry` / `test_raster_draw` 新增以下用例：
  - 跨帧同一材质命中同一个 block，没有新的 Create/Set/Flush。用 fixture probe 检查。
  - `TrimIdleBindings` 或预算设为 0 后，缓存 ID 失效，能重新 Resolve 并正确绘制。
  - 材质纹理 `RequestResourceExit` 后，缓存不阻止退出确认。
  - `ReplaceMaterial` 前后的帧各自持有自己的 pin。
  - F=3 时同一个 block 的 pin 数等于使用它的帧数，完成时每帧只释放一次。
  - Compile 失败的帧仍保留已接纳的资源，直到完成。
- `example_descriptor_reuse` 的完整双后端 21 case：equivalent extra 必须为 0。

**性能**

- 阶段 0 的入口、MaterialScene 原有矩阵，以及前后交替的独立进程。
- 关注 CompileOnly 扫描的斜率、PrepareRecord、CompletionCpu、FullFrame。
- 构建和测试串行进行。

**文档**

- 同步更新 `docs/architecture/render-framework.md`：帧编译合并、BindingRegistry 两段。
- 同步更新 `docs/guide/performance.md`：新增 case。

## 原始证据

- 诊断与测量：[回退定位](2026-10-16-render-framework-regression-diagnosis.md)、[全部原始 JSON](2026-10-16-render-framework-regression-diagnosis.json)、[分阶段 CSV](2026-10-16-render-framework-regression-phases.csv)、[完成 CPU 与 key 微基准 CSV](2026-10-16-render-framework-regression-details.csv)、[固定值种数规模扫描 CSV](2026-10-16-render-framework-regression-scale.csv)。
- 阶段 0 基线（2026-10-16 执行）：[基线数据](2026-10-16-render-framework-regression-fix-baseline.json)、[基线 CSV](2026-10-16-render-framework-regression-fix-baseline.csv)。

## 实施结果（2026-10-17）

阶段 1–4 已实施，阶段 5 未纳入。本机原始 JSON 在 `build_debug/fix-plan-baseline/`，未入库。

**各阶段落点**

- 阶段 1：`CompiledMaterialTemplate::Group` 增加预制 `ElementIndices` 与 `DynamicOffsets`；BindingRegistry 把后端 `ParameterGroupCompatibility` 驻留成小整数 id、Entry 的值改为连续数组加偏移。原生 `AddBindings` 仍走通用路径。
- 阶段 2：`State::Material` 按 `[template][group]` 预建 Material/Sampler 组缓存；无 runtime 输入的 immutable 组缓存挂在 `State::Template`。帧内只做数组下标取 block 加 `IsValid` 校验，失效才用存好的 writes 重建。Draw 直接保存 `{group, block, dynamic offset span}`，不再经过 `Requests`。
- 阶段 3：Scene/Frame、View/Pass 及有 override 的组统一按 `(TemplateId, group, 有序输入 ResourceId 列表)` 建帧级备忘（扁平 `vector` 加线性扫描），每帧 `clear()` 保留容量。
- 阶段 4.1：帧使用清单改为四个 vector，靠各 Entry 的单调帧戳去重。
- 阶段 4.2：Draw 只保留精简的 `DrawBinding{group, block, dynamic offset span}`；`AddDraw` 整体 `std::move` 收纳，`ProcessCompletions` 对 `frame->Draws` 调 `clear()`。（早期“就地复用同批 Draw”已在评审中回退。）
- 阶段 4.3：BindingRegistry 维护按资源的活跃依赖计数，`HasResource` 变 O(1)，`RendererResources::Collect` 不再二级全表扫描。

**偏差**

- 阶段 1 的“缓存值身份”：未单独落在 RendererResources。Material/Sampler 组只在注册时算一次 identity，Scene/View 组按帧备忘，逐元素 identity 已不再是热点；单独的资源级缓存没有消费方，故并入上述组缓存，不新增字段。
- 阶段 4.3 的“计数归零推入候选队列”未做。阶段 0 未采 Tracy trace，无法先量 Collect 占比；反向索引已去掉 `Collect` 的双层扫描，Material/Template 数量也小，帧内全表扫描成本低。若后续 Tracy 显示 Collect 占比上升，再补候选队列。
- 阶段 3 的备忘 key 由“pass 或 view”改为“该组实际读取的有序 ResourceId 列表”（评审修正）。原 key 在同一视图下仍按 pass 区分，本负载每个 view 每材质各一个 pass，M100/V3 就是 300 条独立 key，几乎从不命中。
- 命中路径仍把最终值身份整理成连续数组，仍有每组一次的临时分配。因阶段 1 已把 Resolve 调用次数大幅压到每帧唯一组数量级，这一项优先级降低，留待后续。
- `BindingRegistry` 驻留的后端兼容布局 id 只增不回收，数量以出现过的不同布局为上界；已在该头文件与架构文档说明。

**实测（profiler ON、Release、RTX 4080）**

CompileOnly 中位数（µs），前为阶段 0 基线（`06b8c23`，profiler ON，同机）、后为本轮：

| 后端 | M1 | M7 | M32 | M100 | M300 |
|---|---:|---:|---:|---:|---:|
| d3d12 | 9.66→5.06 | 25.6→8.94 | 92.6→23.4 | 187.8→60.2 | 871.4→325 |
| vulkan | 8.29→4.90 | 20.9→8.60 | 89.2→22.9 | 311.5→60.6 | 986.2→351 |

CompletionCpu 中位数（µs）：d3d12 M1 2.32→1.71、M100 47.0→32.8；vulkan M1 2.63→1.56、M100 56.1→32.3。

相对 `06b8c23` 的 A/B（profiler ON、d3d12 N1000/M100/V3/dirty0/F3）：本轮 PrepareRecord 452 µs、FullFrame 531 µs。

**同机 profiler OFF 对真实基线（`f20c56f` + sampler 补丁）的交替 A/B**

`06b8c23` 本身就是引入回退的版本，只对比它无法回答“是否已经修复”。为此另建 profiler OFF 构建，在同一台机器上按 `old→new→new→old`（各 4 个进程）交替运行 `f20c56f`+补丁 与当前版本，参数 N1000/M100/V3/dirty0/F3、`--benchmark_min_time=1000x --benchmark_repetitions=5`。中位数（µs）：

| case | 后端 | `f20c56f`+补丁 | 当前 |
|---|---:|---:|---:|
| PrepareRecord | d3d12 | 579 | 444 |
| PrepareRecord | vulkan | 625 | 481 |
| FullFrame | d3d12 | 637 | 581 |
| FullFrame | vulkan | 643 | 527 |

当前版本四项都更快（PrepareRecord −23%），M1 双方都在噪声范围内，回退已消除。

绝对 µs 与 `--benchmark_min_time` 相关（迭代越多取值越高），因此只做同参数下的相对比较。

**待验证（2026-10-17 评审）**

CompileOnly 在 M100→M300 时 draw 数是 3 倍，Compile 却从 60 涨到约 325 µs（约 5.4 倍）；修复前是 4.6 倍，超线性项不是本轮引入，但说明除每 draw 常数开销外还有一项随材质数增长。因此先前"Compile 只随 draw 数线性增长、剩余成本属于场景侧"的结论**未证实**，本文件不再作为结论：

- 超线性项的来源未定位。`Renderer::Collect` 每次完成都全表扫描 Material、Template、Resources 槽位，是嫌疑之一。
- 完成路径约 32 µs 的构成也未定位，同样怀疑与 `Collect` 的全表扫描及场景侧每帧工作有关。
- 本轮已给 `ProcessCompletions`、`Collect`、`CompileDraws` 加 `RADRAY_PROFILE_SCOPE_N` 区段；需要抓一次 Tracy 并按区段归因后才能定论。在抓到 trace 之前，上面两条保持"待验证"。

**未达成的目标**

完成 CPU 从 M100 的 47/56 µs 降到约 32 µs，未到"个位数 µs"。剩余 32 µs 的构成**未定位**（见上一节"待验证"）：`Collect` 的全表扫描与场景侧每帧工作都是嫌疑项，需要 Tracy trace 归因，超出本轮范围。

早期“25/23 µs”的完成成本来自把每 draw 释放搬到 `AddDraw`，评审判定这是成本转移而非消除，已在阶段 4.2 回退，故完成成本回到 32 µs。

**评审修正（2026-10-17）**

- 第 1 条：阶段 3 的 Scene/View 备忘 key 从 `(TemplateId, group, pass 或 view, Objects, Parameters)` 改为该组实际读取的有序 ResourceId 列表，覆盖 override 组并去掉 Pass/View 特判；同时把帧级备忘从线性扫描改为帧级扁平开放寻址索引（桶数组跨帧保留容量，清空只归零上一帧占用的桶，命中/未命中都先比较完整 key），避免 override 组多时退化成 O(D²)。
- 第 2 条：回退阶段 4.2 的 `AddDraw` 原地逐字段填充，改回整体 `std::move`；`ProcessCompletions` 对 `frame->Draws` 用 `clear()`，不再按峰值容量遍历。
- 第 4 条：新增 `MaterialGroupWritesReuseOneBlockAndRebuildAfterTrim`（Registry probe 计数）、`FrameGroupMemoSharesIdenticalInputsAndSplitsDistinctOnes`（转发 encoder 观测绑定身份）、`ExitingMaterialTextureRejectsDrawAndConfirmsAfterCachedFrame`（退出发生在缓存建好之后），并把 `FrameStampRetainPinsOnceAndReverseIndexDropsOnDestroy` 扩展到 3 个 flight。
- 第 5/6/7 条：Resolve 命中路径的临时分配因第 1 条后调用次数很少而延后；`_compatibilities` 只增不回收已在头文件与架构文档说明；架构文档措辞已修正（所有组都预派生元素下标与动态偏移、key 描述、override 路径、帧收集不建 hash 节点）。
- 其余小问题：删掉备忘分支末尾重复的 `pin`/`push_back`/`continue`（并入公共尾部）；`buildWrites` 在 `UseResource` 失败时恢复 `Missing frame input '<name>'` 错误信息。带 sampler override 的 draw 每帧调用 `CreateSampler` 再按身份去重，属于既有热路径、非本轮引入，留待后续处理。

**验证**

- `test_radray_runtime` 的 `Backends/RendererRegistry` 30 例通过。
  - block 身份用 fixture 里的转发 encoder 观测：`Record` 的 encoder 由调用方传入，`Renderer::Record` 与 `BindingRegistry::Bind` 只依赖 encoder 接口，因此转发 encoder 能在不改生产代码、不依赖真实后端绑定的前提下记录 `BindShaderParameterSet` 收到的 `ShaderParameterSet*`，断言同视图多 pass、不同视图、同值/异值 override、跨帧材质组各自绑定的 set。真实后端绑定路径由 `example_descriptor_reuse` 覆盖。
  - `BindingRegistry` 层的复用计数测试仍保留，用于覆盖 trim 后重建（指针可能落在同一地址，不能靠指针判断）。
- 全量 `test_radray_runtime` 498 例中，除既有的 `RuntimeFoundation.Vulkan{SingleThread,Threaded}CompletionInputBeforeUpdate` 外全部通过；这两例是 Vulkan instance 初始化/测试顺序问题，撤销本轮改动后在 HEAD 上同样失败。
- `example_descriptor_reuse` 双后端 43 例通过，equivalent extra 为 0。
- `MaterialScene/OverrideChurn/` 规模扫描（本轮新增，profiler ON、Release、`--benchmark_min_time=200x --benchmark_repetitions=3`）：每 draw 一份不同 override，帧内唯一组数 = draw 数。中位耗时 d3d12 N100 271 µs / N1000 1512 µs / N4000 8792 µs，vulkan 341 / 1628 / 8573 µs。N1000→N4000 是 4 倍 draw、约 5.4/5.3 倍耗时（线性加常数），若备忘退回线性扫描则应为约 16 倍。该用例用于防止该回退，原始 JSON 见 `build_debug/fix-plan-baseline/override_churn.json`。

本轮改动未提交。
