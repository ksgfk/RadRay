> - 适用: ADR-0016 第一阶段实施；正式 cook、publisher、完整 AOT coverage 不在本阶段
> - 权威: 本文是第一阶段实施清单；设计约束以 ADR-0016、`CONTEXT.md` 和 shader pipeline 架构文档为准
> - 状态: 待实施（2026-08）。每个里程碑只有其全部检查站通过后才能标记完成
> - 锚点: `docs/adr/0016-hlsl-and-radray-dxc-are-shader-authority.md`, `docs/research/dxc-embedded-metadata-vs-cpp-trace.md`, `CMakeLists.txt`, `modules/render`, `modules/runtime`, `shaderlib`, `tools`

# HLSL + forked RadRay DXC shader pipeline：第一阶段

## 完成定义

第一阶段完成必须同时满足：

1. pinned RadRay DXC SDK 能通过 `dxcapi_radrayext.h` 完成 source contract discovery 和 concrete Variant 的 DXIL/SPIR-V 编译，并输出独立、版本化、target-specific metadata。
2. `radrayrender` 能直接解码 compiler metadata，构造 D3D12 与 Vulkan 各自的 binding/layout/vertex state；运行时不调用反射校验、不重建 metadata、不依赖 SPIRV-Cross。
3. `radrayruntime` 通过 compiler-produced artifact 完成 D3D12/Vulkan JIT 垂直切片；Variant 选择由 caller/asset orchestration 提供，编译器内部完成 stage merge。
4. 旧的 `radrayshader`、手写 shader JSON、`shader_gen`、旧 reflection/manifest resolver 路径和 C++ trace 路线已经删除，依赖图和文档同步完成。
5. `RADRAY_BUILD_SHADER_COMPILER=OFF` 的 compiler-free 构建与安装检查通过，且只用测试 fixture 验证
   compiler-free artifact consumption boundary；这不宣称正式 cook、artifact loader 或生产 AOT 发布已完成。

## 执行规则

- 里程碑必须按顺序推进；前一里程碑的检查站未全部通过，不得开始后一里程碑的删除性改动。
- 检查站必须留下可重复的测试目标、命令或文件清单；人工“看起来正确”不算通过。
- 检查站失败时修复当前里程碑，不通过添加运行时 fallback 或临时 JSON 绕过。
- 每个里程碑完成后，同步更新 `CONTEXT.md`、受影响的 architecture/guide 文档和本文件状态。
- 所有构建/测试遵守 `docs/guide/build-test.md` 的预设；不得并行运行 build 与 test。

### 检查站记录格式

每个检查站使用稳定的 `M<里程碑>-C<编号>` ID，并且必须同时写明验证命令/静态检查、通过条件、
fixture 或测试输入、证据输出位置。推荐的 CTest suite 名称为：
`RadRayShaderContract`、`RadRayDxcMetadata`、`RadRayDxcAtomicBatch`、
`RadRayRenderShaderArtifact`、`RadRayRuntimeShaderJit`、`RadRayShaderCMake`。
只有对应 ID 的命令退出码、断言和证据都满足，才能把该检查站从 `[ ]` 改为 `[x]`。

## M0：契约冻结与 golden fixture

**目标**：把第一期的 wire、身份、错误和测试向量冻结，避免实现阶段重新发明协议。

**实现项**：

- 建立 RadRay extension ABI、source contract、CompileVariant request/result、metadata envelope 和两个 target payload 的版本化头文件草案。
- 建立最小 HLSL fixture 集：无资源 graphics、仅 vertex、texture+sampler、shadow/static sampler、多个 DXIL RootConstants、单 SPIR-V push block、不同 target binding 集合、完整 nested cbuffer/struct type tree。
- raw golden bytecode/metadata fixture 固定存放在 `modules/render/tests/data/shader_artifacts/`；测试只用 C++ fixture table 映射 case 与 raw blob，不引入生产 index 或 JSON。
- 为每个 fixture 固定 keyword domain、assignment、Defines、CompilePolicy、期望 entry topology、active binding 名称和 target-specific layout facts。
- 写出 bytecode/metadata/hash 覆盖表；明确 `ContractHash`、`CompileInputHash` 与 `GpuArtifactHash`
  的输入，第一期不定义 `ArtifactContentHash` 或 `CpuSchemaHash`。compiler 输出的 type-tree record
  不作为独立 hash 输入，但 source/include bytes 仍属于 `CompileInputHash`；PassName/AssetId 不进入 hash。

**检查站（全部通过才完成）**：

- [ ] **M0-C01**：`git diff --check` 通过，所有 wire record 都是固定宽度整数/offset/length 或受控 blob 引用，不含 pointer、`Vk*`、`D3D12_*` 内存结构；证据为 CI 命令日志。
- [ ] **M0-C02**：`RadRayShaderContract` 的 golden assertion table（仅测试断言输入，不是生产 authoring 文件或 metadata）逐条列出 fixture 的 expected target facts，且 DXIL/SPIR-V 明确允许不同 binding 数字与集合；证据为测试输出和 fixture 表。
- [ ] **M0-C03**：同一 fixture 重复生成的 canonical request/hash 字节逐字节相同；改变 source/include、assignment、Defines、target 或 policy 时至少一个规定 hash 改变；证据为 `ContractHash`/`CompileInputHash`/`GpuArtifactHash` golden 输出。
- [ ] **M0-C04**：hash unit test 固定 bytecode 与 GPU metadata，只替换 type-tree record，要求 `GpuArtifactHash` 不变；不存在任何 type-tree hash 或 schema hash；证据为 `RadRayShaderContract` 断言日志。
- [ ] **M0-C05**：设计评审记录确认第一期终点不包含正式 cook、artifact index/loader 或 content-address publisher；后续阶段不得成为 M1-M8 的隐式依赖。

## M1：RadRay DXC SDK 与扩展 ABI 骨架

**前置**：M0。

**实现项**：

- 在 fork 中新增 `dxcapi_radrayext.h`、RadRay CLSID/IID、`IRadRayDxcCompiler`、`IRadRayDxcResult` 和 version/ABI/schema handshake；upstream `dxcapi.h`、IID、vtable、exports 不变。
- 独立 autobuild 生成 `RadRayDXC` package：`Headers`、`Compiler`、可选 `Validator`、可选 `CLI` imported targets；identity 使用完整 upstream version + `+radray.<release>`，例如 `1.9.2607+radray.1`。
- 保留独立 SDK manifest/prefix/hash 校验；RadRay 主工程不 `add_subdirectory`、`FetchContent` 或 `ExternalProject` DXC。
- SDK 自带最小 ABI consumer/loader probe，stock DXC 和错误 fork 均 fail closed。

**检查站**：

- [ ] **M1-C01**：在干净目录对 SDK 执行 `cmake --install` 后，最小 C++ consumer 只包含 `dxcapi.h` 与 `dxcapi_radrayext.h` 即可编译并创建 RadRay extension；stock DXC 对 RadRay CLSID 返回明确 unsupported；证据为 `RadRayDxcAbiProbe` 输出。
- [ ] **M1-C02**：`find_package(RadRayDXC CONFIG REQUIRED COMPONENTS Headers Compiler)` 在 relocation 后成功，正式 preset 使用 manifest prefix + `NO_DEFAULT_PATH`；伪造的 stock `RadRayDXCConfig.cmake` 放入默认搜索路径时仍被拒绝；`Validator`/`CLI` 不被 `Compiler` 隐式引入；证据为 configure 日志和 target graph。
- [ ] **M1-C03**：修改 SDK manifest/archive 任一 hash 或 ABI/schema/toolchain identity 后，正式 prefix discovery 配置失败；`RADRAY_DXC_SDK_ROOT` 只在 fork development preset 生效，CI/release preset 拒绝 override；证据为正/负 configure 日志。
- [ ] **M1-C04**：编译器输出目录存在 compiler binary；`Compiler` 组件使用 `install(IMPORTED_RUNTIME_ARTIFACTS ...)` 安装到 `ShaderCompiler`；Validator/CLI 组件按选择安装；compiler off 时没有对应 copy/install rule；证据为 build/install 文件清单。
- [ ] **M1-C05**：pinned SDK 的默认 DXIL 通过真实 D3D12 PSO 创建与执行 smoke test；没有把 external `dxil` presence 当作先决条件；证据为 SDK CI 的 `D3D12_Jit`/等价 smoke 输出。

## M2：Source contract discovery 与 HLSL authoring

**前置**：M1。

**实现项**：

- fork frontend 正式解析根 `.hlsl` 的 `#pragma radray_keyword_group`；`.hlsli` 不扩张 Pass domain，pragma 必须位于条件编译之外。
- 通过标准 shader-stage attributes 推导 entry topology；禁止 stage/entry 列表由 caller 维护，禁止一个 source unit 混合 graphics/compute。
- 使用 DXC 内建 `__spirv__`（及 compiler version macros）区分 SPIR-V lane；不再复制/注入手工 `VULKAN` define。
- 实现 compiler-owned `DiscoverSourceContract`：对 DXIL/SPIR-V 各做轻量 discovery，比较 domain、entry names/stages、kind/cardinality，输出一个 canonical `ContractHash`。
- 编译请求只接受结构化 `Defines`、`KeywordAssignments` 和 typed `CompilePolicy`，不接受 raw `-D` 或 raw DXC arguments。

**检查站**：

- [ ] **M2-C01**：`RadRayShaderContract` 中合法 fixture 的两个 target lane 得到相同 topology/domain/hash；`__spirv__` 只改变允许的 target facts；证据为 canonical contract golden。
- [ ] **M2-C02**：重复 group、跨组重复 keyword、非法 assignment、conditional entry、缺 vertex/重复 stage、graphics/compute 混合均在 discovery 阶段失败，并有稳定 diagnostic code；证据为负向测试日志。
- [ ] **M2-C03**：普通 define 不能覆盖 keyword domain；assignment 顺序被 canonicalize；相同逻辑输入的 discovery result 与 hash 稳定；证据为 canonical request dump。
- [ ] **M2-C04**：改变 source/include 的 contract 部分后，`CompileVariant(ExpectedContractHash)` 拒绝 contract drift；只改变函数体或普通 include 内容但保持 topology/domain 时允许编译并产生新的 `CompileInputHash`；不允许 caller 自行扫描 pragma；证据为正/负 discovery/compile 测试。
- [ ] **M2-C05**：现有 shaderlib 至少一个 graphics、一个 depth-only 和一个 compute fixture 已迁移到标准 HLSL topology；旧 JSON 不再参与 discovery；证据为 `RadRayShaderContract` 迁移 suite。

## M3：Compiler 内 concrete Variant 编译与 metadata wire v1

**前置**：M2。

**实现项**：

- 实现 `CompileVariant`：caller 提供一个确定 assignment 和 target mask；DXIL/SPIR-V lane 独立编译、优化、校验，任一 requested lane 失败则整个 batch 失败。
- compiler 内按每个 active stage 资源做 variant-level merge；每个 Variant 只输出实际使用的 binding 集合与 stage visibility。
- DXIL 解析 HLSL RootSignature 并投影 exact active Root Signature；SPIR-V 读取标准 `vk::binding`/`vk::push_constant`；静态 sampler 按 declaration identity 生成 immutable sampler metadata。
- 输出 target-specific metadata wire：entry facts、active bindings、target-native layout、vertex reflection、完整 cbuffer/struct member type tree、RootSignature/immutable sampler facts、bytecode hash、`GpuArtifactHash` 和 toolchain identity；compiler 输出的 type-tree record 不作为独立 hash 输入。
- metadata 原样作为 result blob 返回；不得让 runtime/cook 重序列化或通过 reflection 再推导。

**检查站**：

- [ ] **M3-C01**：`RadRayDxcMetadata` 中同一 concrete assignment 重复编译的 bytecode、metadata 与 `GpuArtifactHash` 逐字节/逐值稳定；不同 assignment 不会共享错误 layout。
- [ ] **M3-C02**：no-resource、texture/sampler、shadow、未使用资源、不同 target binding 集合 fixture 的 active set 与 visibility 逐项匹配；inactive declaration 的 lookup 明确失败；证据为 target payload dump。
- [ ] **M3-C03**：DXIL fixture 证明多个 RootConstants 合法并分别可查；SPIR-V fixture 证明多个 ranges 仍来自单一 push block，第二个 active block 编译失败；证据为 compiler diagnostics 和 metadata decode。
- [ ] **M3-C04**：graphics vertex/pixel 声明不同 RootSignature 时 compiler hard error；DXIL RootSignature 与 active resource 并集不一致时 hard error；compute entry 独立解析自己的 RootSignature；证据为 `RadRayDxcMetadata` 负向测试。
- [ ] **M3-C05**：static sampler 成功关联、缺失 declaration、重复/冲突 policy 三组 fixture 分别得到 expected result；成功案例允许 DXIL register/space 与 SPIR-V set/binding 数字不同；证据为 immutable-sampler payload dump。
- [ ] **M3-C06**：DXIL + SPIR-V batch 中任一 requested lane 故意失败时，`RadRayDxcAtomicBatch` 断言 result 为 failed、所有 target lane 不可访问、没有 publication/persisted blob，且 diagnostics 保留 lane failure；成功 lane 不得作为半成品交付。
- [ ] **M3-C07**：损坏 magic/version/range、截断 blob、错误 target/toolchain identity、type-tree 越界/非法 record 全部被 decoder/consumer fail closed；没有 native pointer 或平台 ABI 落盘；证据为负向测试日志。
- [ ] **M3-C08**：每个 target lane 的完整 type tree 能支持 nested member、array/matrix stride、offset/size 和 CPU upload construction；runtime 只检查 wire safety，不计算/比较 schema hash；固定 bytecode/GPU metadata 替换 type-tree record 时 `GpuArtifactHash` 不变。

## M4：`radrayshadercompiler` client 与 CMake 能力开关

**前置**：M1、M3 ABI 稳定。

**实现项**：

- 新建可选 `radrayshadercompiler`，只依赖 `RadRayDXC::Headers`，动态加载 canonical bare platform library name，使用 extension CLSID/ABI/toolchain identity 校验，不回退 upstream compiler API。
- 将 `DiscoverSourceContract`/`CompileVariant` request/result、COM lifetime、diagnostics 和 blob ownership 映射为 RadRay C++ API；不拥有 render layout、asset identity、coverage、cook 或 JIT policy。
- CMake 删除 `RADRAY_BUILD_SHADER`、`RADRAY_ENABLE_DXC`、`RADRAY_ENABLE_SPIRV_CROSS`；`RADRAY_BUILD_RENDER` 不再依赖 shader module，新增独立 `RADRAY_BUILD_SHADER_COMPILER`，`RADRAY_ENABLE_SHADER_JIT` 依赖 compiler，未来 `RADRAY_BUILD_SHADER_TOOLS` 默认 OFF 且依赖 compiler。compiler 开启时公共 build output/install 自动带 compiler binary。
- 按 `RadRayDXC::Headers/Compiler/Validator/CLI` 做组件裁剪；纯 runtime preset 完全不 find package。
- 固定 RadRay 安装组件：`Runtime`（render/runtime/backend 消费路径）、`ShaderCompiler`、可选 `Validator`、可选 `CLI`；组件之间不得隐式带入 compiler。

**检查站**：

- [ ] **M4-C01**：最小 client consumer 的 link map 不含 `radrayrender`、`radrayruntime`、RHI backend 或 import-linked compiler；运行时加载只依赖平台搜索顺序；证据为 link map 和 loader probe。
- [ ] **M4-C02**：stock/旧 fork/错误 ABI binary 被 extension probe 拒绝；不存在“加载失败后调用 `IDxcCompiler3`”的路径；证据为 `RadRayDxcAbiProbe` 负向日志。
- [ ] **M4-C03**：`RADRAY_BUILD_SHADER_COMPILER=ON` 的 build tree 和 install tree 均在固定 preset 约定目录包含 compiler binary；`OFF` 时 configure/build/install 的 target graph、文件清单和 CMake cache 均不含 SDK/client；证据为 `RadRayShaderCMake` 文件清单。
- [ ] **M4-C04**：`RADRAY_ENABLE_SHADER_JIT=ON` 且 compiler off 时 configure 明确失败；`RADRAY_BUILD_SHADER_TOOLS=OFF` 时不注册 shader tools；`RADRAY_ENABLE_SPIRV_CROSS` 不再存在；证据为正/负 configure 日志。
- [ ] **M4-C05**：运行 `ninja -t commands` 或等价 CMake command inspection，证明 shader tools 只依赖 client+SDK，不依赖 render/runtime/backend；证据为命令图和 link map。

## M5：`radrayrender` target-native runtime representation

**前置**：M3 的 golden metadata；可与 M4 并行实现，但不得改变 M3 wire。

**实现项**：

- 将 bytecode view、metadata decoder、target-native binding/layout、vertex interface、完整 CPU type tree 和 backend pipeline-layout 构造归入 `radrayrender`。
- D3D12 直接消费 serialized RootSignature 与 DXIL facts；Vulkan 直接消费 SPIR-V facts 和 immutable sampler/push ranges；公共 RHI 不重新表达为统一字节布局。
- binding lookup 使用 HLSL declaration name -> artifact-local `BindingHandle`；handle 不跨 target/Variant/recompile 保证数值稳定。
- PSO builder 将 compiler vertex reflection 与外部 `PrimitiveVertexLayout` 组合；compiler 不拥有 stride/slot/step/format/offset。

**检查站**：

- [ ] **M5-C01**：`RadRayRenderShaderArtifact` decoder golden tests 覆盖两个 target 的所有 M3 fixture，并逐项构造 native layout；运行时不调用 DXIL/SPIR-V reflection API。
- [ ] **M5-C02**：DXIL register/space 与 SPIR-V set/binding 不一致的 fixture 能同时成功；任何公共代码试图互换两者时测试失败；证据为 target-native layout dump。
- [ ] **M5-C03**：static sampler、multiple RootConstants、single SPIR-V push block、nested CPU type tree 和 target-specific vertex interface 均有独立 tests；type tree 只做 wire safety 检查。
- [ ] **M5-C04**：fuzz/negative tests 对截断、越界 offset、重复 name、错误 GPU hash、未知必需 record fail closed，且不会创建半初始化 pipeline layout；type tree 不以 schema hash 验证。
- [ ] **M5-C05**：PSO builder 对 semantic/location 缺失、shape/format 不兼容、slot/offset/stride/step mode 不一致的 `PrimitiveVertexLayout` 在创建任何 native PSO 前失败；D3D12/Vulkan 各有正/负测试。
- [ ] **M5-C06**：`rg`/link inspection 证明 `radrayrender` 不依赖 `radrayshadercompiler`、DXC headers、SPIRV-Cross 或 runtime asset orchestration；证据为 target graph。

## M6：`radrayruntime` JIT orchestration 与双后端垂直切片

**前置**：M4、M5。

**实现项**：

- 将 assignment 选择、JIT 调度/缓存和 fail-closed policy 放入 `radrayruntime`；第一期不实现 artifact index、磁盘 artifact loader 或 AssetId/PassName publication mapping。
- 开发期 JIT 只在明确 capability 开启时调用 client；请求带 expected ContractHash 和一个 concrete assignment；不提供 legacy/new fallback。
- 使用 M0 golden HLSL fixture 与 M3 compiler result 形成临时内存 JIT artifact，验证“compiler result 原样交给 render decoder”。这不是 cook，也不引入生产 JSON 或 artifact index。

**检查站**：

- [ ] **M6-C01**：`RadRayRuntimeShaderJit` 的 D3D12 vertical slice 创建 PSO、执行 draw/dispatch、读取确定像素/缓冲结果；Vulkan vertical slice 完成同等 readback；证据为 backend smoke 输出。
- [ ] **M6-C02**：覆盖 no-resource graphics、texture+sampler、depth-only/static sampler、多个 DXIL RootConstants、SPIR-V push block、target-specific binding 集合六组场景；证据为 fixture case report。
- [ ] **M6-C03**：缺失 assignment、缺失 target lane、corrupt in-memory blob、ContractHash drift、toolchain mismatch 全部 fail closed；没有 JIT fallback 到另一 Variant 或另一 target；证据为负向 suite。
- [ ] **M6-C04**：binding handle 只能由当前 compiler result 的 HLSL declaration name 解析；inactive/unknown name 查找失败；证据为 lookup tests。
- [ ] **M6-C05**：JIT 测试进程的模块/导入列表不包含 SPIRV-Cross；compiler-enabled 模式只通过 `radrayshadercompiler` 动态加载 SDK，不存在 artifact index/loader 调用；证据为 module/import inspection。

## M7：现有 shaderlib 迁移与旧路径切换

**前置**：M6 的双后端垂直切片稳定。

**实现项**：

- 迁移 forward/depth/compute shader 到标准 stage attributes、HLSL keyword pragma、`__spirv__` 条件宏、标准 `vk::binding`/`vk::push_constant` 和可复用 RootSignature。
- 删除所有手写 `*.shader.json` binding/layout/variant metadata；PassName/AssetId 在资产调用方映射。
- 将实际 runtime representation 合并进 `radrayrender`，orchestration 合并进 `radrayruntime`，以 `radrayshadercompiler` 保留可选 client；移除旧 `radrayshader` 公共模块和 `shader_gen`。
- 更新 `docs/architecture/shader-pipeline.md`、`shaderlib.md`、`shader-authoring.md`、build guide 和依赖图；旧 C++ trace TODO 保持历史状态。

**检查站**：

- [ ] **M7-C01**：`RadRayRuntimeShaderJit`、D3D12/Vulkan vertical slices 和 migrated shader suites 使用 compiler metadata 通过；结果与版本控制的像素/资源行为基线一致；证据为 suite 输出与 baseline record。
- [ ] **M7-C02**：`rg -n --glob '!docs/adr/**' --glob '!docs/research/**' --glob '!docs/todo/**' "shader\\.json|shader_gen|SPIRV-Cross|reflection validation|C\\+\\+ trace|RADRAY_BUILD_SHADER(\\b|[^_])|RADRAY_ENABLE_DXC|RADRAY_ENABLE_SPIRV_CROSS" CMakeLists.txt cmake modules shaderlib tools docs/architecture docs/guide` 在生产源码、CMake 和 active docs 中无命中（历史 ADR/research/todo 允许命中并标明历史）；证据为命令日志。
- [ ] **M7-C03**：`cmake --build --preset win-x64-debug-shader-compiler`、适用 CTest suites 和 shader tool command inspection 全部通过；不出现 shader CLI 到 render/runtime/backend 的反向依赖；证据为 build/test/link logs。
- [ ] **M7-C04**：对任一 migrated Pass，删除/篡改 compiler metadata 后运行时明确失败，不会重新反射或从 JSON 补齐；证据为 negative runtime test。

## M8：第一期切换完成与 compiler-free package gate

**前置**：M7 全部通过。

**实现项**：

- 设置默认产品路径为新 HLSL + forked DXC contract；删除旧模块、旧 JSON codec、旧 manifest resolver、SPIRV-Cross、shader_gen 和无迁移价值的 probe/options。
- 增加纯 runtime preset：不发现/导入 RadRay DXC，不编译 client/tools，不复制/安装 compiler binary、headers、CLI、import libraries、`dxcompiler`/`dxil`，只保留 artifact decoder/layout/backend consumption。
- 用 M3 编译并版本控制的 raw golden bytecode/metadata fixture 作为 compiler-free decoder boundary test 输入；测试直接传入 target bytecode/metadata，不使用 artifact index/loader；正式 cook/publisher 不在本里程碑实现。
- 增加两个隔离 CMake preset：`win-x64-debug-shader-compiler`（compiler ON、JIT ON、tools OFF，binary dir `build_shader_compiler`）和 `win-x64-debug-runtime-only`（compiler/JIT/tools OFF，binary dir `build_runtime_only`）；install prefix 分别固定为 `_install/shader_compiler` 与 `_install/runtime_only`。
- 完成 docs index、CONTEXT 术语、ADR 状态和本 TODO 的收尾记录。

**检查站**：

- [ ] **M8-C01**：`cmake --preset win-x64-debug-runtime-only`、`cmake --build --preset win-x64-debug-runtime-only`、`cmake --install build_runtime_only --prefix _install/runtime_only --component Runtime` 全部成功；日志无 `find_package(RadRayDXC)`，安装文件清单无 DXC/compiler/client 文件；证据为 `RadRayShaderCMake` 日志和白名单。
- [ ] **M8-C02**：`RadRayRenderShaderArtifact` 在没有 compiler DLL、DXC headers、HLSL source、artifact index/loader 和 SDK 的隔离进程中直接消费 `modules/render/tests/data/shader_artifacts/` 下版本控制的 raw golden fixtures；损坏 metadata 时 fail closed；证据为 module/import inspection 和 test output。
- [ ] **M8-C03**：`cmake --preset win-x64-debug-shader-compiler`、`cmake --build --preset win-x64-debug-shader-compiler`、`cmake --install build_shader_compiler --prefix _install/shader_compiler --component ShaderCompiler` 全部成功，build/install 输出目录包含 compiler binary；证据为文件清单。
- [ ] **M8-C04**：`win-x64-debug-runtime-only` 与 `win-x64-debug-shader-compiler` 不共享 build cache；JIT suite 只在 compiler preset 注册，runtime-only 只运行 compiler-free decoder/render suites；证据为 CTest discovery output。
- [ ] **M8-C05**：D3D12 loadability/execution smoke、Vulkan validation-enabled smoke 和适用 CTest suites 全部通过；证据为 backend logs 和 suite reports。
- [ ] **M8-C06**：`git diff --check`、docs checker（相对于既有 baseline 不新增错误）、CMake target/link graph inspection 全部通过；旧路线只在历史记录中可见；证据为 CI artifacts。

## 后续但不阻塞第一期

以下内容保留为后续 TODO，不得作为 M0-M8 的完成条件：正式 cook/bake enumeration、artifact index/loader、artifact publisher、内容寻址目录发布、完整平台 coverage、生产 AOT artifact index 生成、纯 runtime release 包验收。后续实现必须直接消费本 ADR 的 v1 compiler wire，不得重新引入手写 JSON、runtime reflection、type-tree hash 或临时第二套 metadata。
