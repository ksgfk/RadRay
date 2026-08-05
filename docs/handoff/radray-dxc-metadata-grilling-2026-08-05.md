# RadRay DXC metadata 方案 grilling 交接

## 下一会话目标

继续围绕 forked DXC + HLSL single-source 方案逐项 grilling。每次只解决一个设计决策；用户
确认 shared understanding 前不要实现代码，也不要生成最终 implementation TODO。全部关键边界
确认后，更新当前契约/ADR，再制定可执行 TODO。

## 先读

- 仓库 `AGENTS.md` 与外部 `RTK.md`；所有 shell 命令必须以 `rtk` 开头。
- `CONTEXT.md`：本轮已把确认过的领域词汇即时写入。
- `docs/research/dxc-embedded-metadata-vs-cpp-trace.md`：本轮扩展的 point-in-time 研究记录。
- `docs/handoff/cpp-trace-shader-handoff.md`：仅作历史背景，旧 C++ trace 方向已被本轮决策关闭。
- `docs/architecture/shader-pipeline.md`：当前 manifest/AOT/JIT/runtime 事实。

研究记录仍保留候选方案比较和部分旧的 reflection 聚合结论；它不是当前契约。本轮后续决策
已经选择 compiler-only active metadata，因此不要把报告中较早的 C++ trace 或 runtime
reflection 建议当成已确认设计。

## 已确认决策

1. 采用 exact-permutation：每个 compiled Variant artifact 只拥有实际使用的 binding 集合。
2. graphics layout 是该 Variant 所有 active stages 的 binding 并集；entry 记录 stage
   visibility。stage projection 不产生独立 RHI ABI layout。
3. binding metadata 完全由 compiler 生成。runtime 信任 compiler artifact，不再通过
   DXIL/SPIR-V reflection 做二次校验。
4. metadata 是 DXC 的独立结构化输出，与 bytecode 共同组成 artifact；不嵌入 DXIL private
   data 或 SPIR-V `OpModuleProcessed`。
5. graphics Variant 的跨-stage metadata 合并必须在 compiler 内完成，不能交给 cook/runtime。
6. 一次 compiler-level request 代表一个完整 Variant，内部生成所有 stage bytecode 和唯一
   Variant-level metadata。
7. 合法 keyword domain 属于 HLSL shader contract；具体 assignment 由调用方传入：cook 为
   AOT 传入，runtime 在允许 JIT 时传入。compiler 校验/编译当前 assignment，不枚举全部
   permutation。
8. binding identity 由 HLSL 源稳定声明；同一 target 的不同 Variant 只改变 active/inactive，
   不重新编号。
9. layout 是 target-specific：同一个 logical Variant assignment 的 DXIL 与 SPIR-V 可以有
   不同 active binding 集合/layout identity。
10. HLSL + forked DXC 是唯一路线。完全关闭 C++ trace、LuisaCompute 和其他 shader authoring/
    codegen 路线。
11. 删除全部作者手写 `.shader.json`。任何作者维护的 shader metadata 都必须在 HLSL；
    machine-generated artifact/index 可以使用 JSON，但不能由作者编辑或成为第二真相。
12. `BakeVariants` 被删除。HLSL 只定义合法 domain；AOT assignment 集合由 cook 调用方按项目、
    平台和内容传入，不写手工 JSON。
13. 一份入口 `.hlsl` 定义一个 Pass，共享代码放 `.hlsli`。stage entry 在 HLSL 声明并由
    compiler 自动发现；调用方不再维护 entry point 列表。
14. 不新增 RadRay binding attribute。DXIL 以标准 `register(..., space...)` 为 binding 真相；
    SPIR-V 以 `[[vk::binding(...)]]` / `[[vk::push_constant]]` 为真相。两套数字不要求相等，
    target 本来就是两套独立 ABI。

## 当前唯一待回答问题

用户指出 DXIL 应复用 HLSL Root Signature。已核对官方 DXC `v1.9.2607` 源码并提出第 15 个
决策，用户尚未确认：

> DXIL 以 HLSL `[RootSignature(...)]` 为声明真相；forked DXC 用当前 Variant 所有 active
> stages 的最终资源并集，在 compiler 内生成 exact projected Root Signature。移除 inactive
> parameter/range/static sampler，删除空 descriptor table，重新验证，并把同一份 projected
> serialized Root Signature 附到所有 DXIL stage bytecode/metadata。不要新增
> `radray::root_descriptor`。

stock DXC 的重要事实：`[RootSignature]` 会被编译/序列化到 DXIL，且有
`DXC_OUT_ROOT_SIGNATURE`；validator 只要求 active shader resources 被覆盖，允许 Root
Signature 含额外项。所以不做 fork projection 只能得到 compatible superset，不能自动满足
exact-permutation。

下一会话应先用 grilling 只问用户是否确认上述第 15 项，不要跳到后续问题。

## 后续仍需逐项 grilling

- Root Signature projection 的精确定义：descriptor range/显式 offset、unbounded range、root
  constants、static sampler、visibility/deny flags，以及多个 stage 的原始 Root Signature 必须
  相同还是允许 compiler 合成。
- stage entry 应复用标准 `[shader("vertex")]` 等 attribute，还是保留概念上的 RadRay entry
  attribute。用户只确认了“由 HLSL 声明并自动发现”，未确认最终语法。
- SPIR-V immutable sampler 的表达/去留；标准 `vk::binding` 不包含 sampler state。
- shader model、optimization/debug、platform/global defines、target 等 request/build options 的
  owner。当前建议是调用输入，但尚未逐项确认。
- `EnableUnbounded` 是否由声明推导、由 request 输入，或被新 compiler contract 删除。
- vertex shader interface 由 compiler metadata 输出；mesh buffer packing 仍归 mesh/PSO caller，
  这点尚未让用户裁决。
- 完整 Variant request 与现有 stage projection 去重的冲突：是否接受每个 Variant 配对重编 VS，
  或要求 compiler 内部 stage cache/projection。当前 forward full domain 是每 target 1 VS + 256 PS；
  不能无意退化为 256 VS + 256 PS。
- metadata schema、layout identity canonicalization、artifact pairing/integrity、fail-closed 行为、
  toolchain/fork/schema version 与 cache invalidation。
- AOT/JIT artifact/index 和 runtime layout cache 的新 ownership；现有 pass-load-time shared layout
  必须改成 per compiled Variant artifact layout。
- compiler fork API/CLI/COM surface、multi-stage output、错误原子性和 partial-output policy。
- fork build/distribution/validator gate，以及最小 compiler-only prototype。
- 首个迁移 Pass、兼容期是否存在、旧 manifest/schema/CLI/ADR 的删除顺序。

## 研究证据摘要

- 当前 SDK `dxc.exe --version` 对应 official DXC tag `v1.9.2607` commit `0d3ee6b5...`。
- official checkout 位于 `%TEMP%\radray-dxc-v1.9.2607`，只读研究使用。
- `DXC_OUT_EXTRA_OUTPUTS`/`IDxcExtraOutputs` 是 public API，但 upstream 没有 metadata producer/tests；
  fork 必须实现 producer、schema 和 output assembly。
- DXIL 与 SPIR-V 是不同 codegen actions，但可以共享 AST consumer；frontend AST 不能天然代表
  post-DCE active resource。
- forward pass 有 8 个 Pixel-only binary keyword 维度：full domain 为 256 variants。当前双 target
  full clean cook 预计 514 次 unique stage compile；当前 default + fully-on sparse coverage 是 6 次。
- DXC fork 热点 `Attr.td`、`SemaHLSL.cpp`、`dxcompilerobj.cpp` 在相邻 tags 间均有变化，需 fork
  regression matrix 和独立 package identity。
- Root Signature 新证据已写入研究报告的“HLSL Root Signature 是 DXIL policy 真相，不是
  active projection”小节。

## 工作树状态

分支 `main`，相对 `origin/main` 无提交。当前修改：

- `CONTEXT.md`
- `docs/research/dxc-embedded-metadata-vs-cpp-trace.md`

`git diff --check` 通过，仅有 Windows LF/CRLF warning。`docs/research` 的改动包含此前完整研究
扩展以及本轮 Root Signature 证据；`CONTEXT.md` 包含已确认领域边界。没有代码实现、没有测试
构建、没有 commit。

此前 `python tools/check_docs.py` 因仓库基线缺少 `docs/guide/build-test.md` 而失败（AGENTS.md 和
README.md 的既有引用，共 4 项）；不要为了本任务顺手修复。

## 建议 skills

- 先调用 `grill-me`（会重定向到 `grilling`），严格一次解决一个决策。
- 继续使用 `domain-modeling`，每次用户确认术语/ownership 后立即更新 `CONTEXT.md`。
- 外部 DXC 行为需要新增事实时使用 `research`；若该 skill 不可用，只用 official DXC source/
  Microsoft primary sources，并继续更新现有唯一研究报告，不新建重复报告。
- 全部决策确认后，再用 ADR/domain-modeling 流程替换过时的 manifest/C++ trace ADR，并编写
  implementation TODO。
