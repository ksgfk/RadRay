> - 适用: 修正 RadRay DXC filesystem-backed include、source contract discovery、runtime JIT、raw shader tool 与相关测试
> - 权威: 本文是本次修正的实施计划；已确认的设计约束以 `CONTEXT.md` 与 `docs/adr/0018` 至 `docs/adr/0033` 为准
> - 状态: 已完成（2026-08-09；filesystem-backed include、ABI 断代、client/JIT/工具/样例/测试已落地并验证）
> - 锚点: `modules/shader/include/radray/shader/shader_compiler_contract.h`, `modules/shader/include/radray/shader/shader_artifact.h`, `modules/shader_compiler`, `modules/runtime/include/radray/runtime/shader_jit.h`, `tools/shader_compile`, `examples/example_lambert_sphere`, `docs/research/radray-includesource-dxc-boundary.md`

# Filesystem-backed shader include 修正计划

> 后续决策：本计划完成时保留的 `-P` + root-only scanner 已被 ADR-0034 取代；frontend/AST/Sema
> 迁移与 discovery input parity 统一由 `docs/todo/radray-dxc-frontend-semantic-migration.md` 跟踪。

## 目标

把 RadRay 当前的 caller-owned include closure 改成标准的 filesystem-backed 编译流程：caller
提交 root source bytes、逻辑 source name 和有序物理 include directory；RadRay DXC 在每次真实
invocation 中通过标准 `-I` 与 default filesystem include handler 按需读取 include 文件。

本次修正的验收重点是 include 边界和 shader compiler 使用情况，不改 render framework 的职责，
除非实现过程中发现明确的框架 bug。

## 已确认契约

1. `RootSource` 保留为内存中的原始 root `.hlsl` bytes；compiler 不根据 `SourceName` 从磁盘重新
   打开 root。`SourceName` 仅作为 DXC virtual main-file name，用于诊断和预处理上下文。
2. `CompileVariantRequest` 删除 `Includes`；caller 不递归读取、预展开或提交 transitive include
   内容。
3. include directories 是调用上下文，不是 shader request、canonical request、`ContractHash`
   或 output identity 的字段。顺序原样对应 DXC `-I` 顺序，compiler 不内置 `shaderlib`、工程根、
   CWD 或任何物理路径。
4. `ShaderJit` 构造时显式接收并按值持有 immutable path array；无默认路径、setter、全局路径或
   construction-time filesystem snapshot。相对路径在实际 invocation 时按当时进程 CWD 解析。
5. discovery 与 concrete stage compile 使用相同的 caller path list 和 DXC default include handler。
   两者都直接运行 frontend；`.hlsli` 不声明 RadRay contract，entry/pragma 语义由 Clang/DXC
   collector 负责。
6. 每次 DXC invocation 独立创建 handler；不跨 invocation 缓存 include bytes，也不由 RadRay 负责
   include 变更后的失效判断。caller 负责在一次 operation/batch 的读取窗口内稳定 include tree。
7. ABI path list 是独立的同步 borrowed view。每项是显式长度 UTF-8 bytes；空数组合法；非法
   view fail closed。路径 marshaling 属于 client/JIT，不进入 DXC 内部路径策略。
8. 删除 `CompileInputHash` 及其所有 wire/metadata/测试语义，保留 `ContractHash`、`BytecodeHash`、
   `PipelineLayoutHash`、`GpuArtifactHash` 等 output identity。ABI、wire、metadata、toolchain/package
   identity 统一断代，旧版本拒绝，不提供兼容 adapter。

## 非目标与延期项

- 不在 RadRay 重新实现 angle/quote、相对/绝对路径和 shadowing 规则；这些全部由 DXC 处理。
- 不把 include path/content 变成编译 identity，也不在 compiler 内实现依赖图、缓存失效、锁或
  hermetic source bundle。
- discovery 的 ordinary `Defines` 与完整 `CompilePolicy` 已纳入 typed request；keyword assignments
  仍只属于 concrete compile，不进入 discovery contract identity。
- 不在 RadRay 侧实现 DXC frontend/AST/Sema；如果未来允许 include 文件声明 entry 或 keyword domain，
  再单独设计 source-origin 和宏语义。
- 不修改 render framework、RHI 或 backend 的 include 搜索职责；runtime 只改变 ShaderJit 的
  compiler 调用配置。

## 文件责任与修改面

### 1. RadRay DXC fork

- `include/dxc/dxcapi_radrayext.h`
  - 增加 include path list ABI view。
  - 为 discovery/compile 增加独立 path-list 参数。
  - bump extension ABI、wire、metadata 和 toolchain identity，并使用新 interface IID，避免旧
    vtable 被误当成新接口。
  - 明确 borrowed lifetime、UTF-8、空列表和 invalid view 规则。

- `tools/clang/tools/dxcompiler/dxcradray.cpp`
  - 解析并校验 path-list view，转换为 DXC `-I` 参数；不保存 caller pointers。
  - 抽出统一的 invocation argument/handler 建立逻辑，保证 discovery、每个 lane 和每个 stage
    使用同一份 path list。
  - 删除 fork 内 `IncludeSource`、`ExpandSourceInternal`、`ExpandSource`、opened tracking、
    `EncodeCompileInput` 和相关 hash 输入。
  - discovery 以 raw root 直接调用 DXC syntax-only frontend；不再执行独立 `-P` 或 root-only scanner。
  - `CompileStage` 改为 raw root `DxcBuffer`，以 `BuildArguments` 加入 `SourceName`、entry/profile、
    defines/assignments、target flags 和 `-I`，传入 per-invocation default handler。
  - 保持现有 result status、diagnostic 和 requested-lane 原子发布规则；include 错误直接保留
    DXC diagnostics。

- fork tests/probes
  - 删除三个 RadRay 专用 fork probe/fixture；在现有 ClangHLSL/TAEF/gtest harness 中覆盖 ABI/schema
    handshake、typed discovery、空/多路径、非法 view、缺失嵌套 include 与路径顺序。
  - 重新生成并校验 RadRay DXC SDK archive、manifest identity 与 SHA-256；不修改 RadRay 的
    `third_party/` 或 `SDKs/` 源树。

### 2. RadRay shader wire 与 compiler client

- `modules/shader/include/radray/shader/shader_compiler_contract.h`
  - 删除 `IncludeSource`、`CompileInputHash`、request include 字段及旧 input-hash 版本语义。
  - 保留 root source、defines、assignments、targets、policy、expected contract。
  - 更新 wire constants/静态断言。

- `modules/shader/src/shader_compiler_contract.cpp`
  - canonical request encoder 不再序列化 include name/bytes 或 input hash。
  - 保持字段排序、重复名校验和其余 request canonicalization 不变。

- `modules/shader/include/radray/shader/shader_artifact.h` 与对应 source
  - 删除 metadata envelope 的 `CompileInputHash` 字段，更新 header size/schema/decoder bounds。
  - 重新生成 metadata goldens；不得在 decoder 侧补算 include/content hash。

- `modules/shader_compiler/include/radray/shader_compiler/client.h` 与 `src/client.cpp`
  - discovery/compile API 增加显式 include path 参数，client 保持无路径状态。
  - 将 high-level path 转为 ABI UTF-8 blob view，仅在同步调用期间保留 marshaling storage。
  - 完整转发 fork status/diagnostics；不新增跨调用 `LastDiagnostics`。
  - 旧 ABI、schema、toolchain identity 或 result 形状不匹配时 fail closed。

### 3. Runtime JIT、工具和样例

- `modules/runtime/include/radray/runtime/shader_jit.h` 与 `src/shader_jit.cpp`
  - 构造函数改为显式接收 `vector<std::filesystem::path>` 并按值拥有。
  - discovery/compile 都从同一 immutable list 调用 client。
  - `RADRAY_ENABLE_SHADER_JIT` 关闭分支同步新构造函数签名，继续直接返回 unavailable/nullopt。

- `tools/shader_compile/shader_compile.cpp`
  - 保留 `--shader-root` 读取 root source，并作为第一个 include path。
  - 添加可重复 `--include-path`，按命令行顺序追加。
  - 删除递归 `CollectIncludes`、`set` visited 和 `IncludeSource` request 初始化。
  - 工具只负责读取 root、传路径和写 raw lane outputs。

- `examples/example_lambert_sphere/example_lambert_sphere.cpp`
  - 删除 include closure collector；用工程根下的 `shaderlib` path 构造 JIT。
  - 保留样例自定义 pipeline 与 runtime shader JIT 流程，不引入框架级路径默认值。

- runtime/shader compiler/render tests 与 fixture generator
  - 所有 `IncludeSource` helper 改为物理 fixture directory path。
  - 共享 shaderlib 测试显式传 `shaderlib` 路径；测试 CWD 约定只用于相对 path 行为，不成为隐式
    compiler 配置。

## 实施顺序与检查站

### M0：冻结新契约并同步 SDK（已完成）

先改 fork ABI/header 与 RadRay wire/schema 定义，删除 input-hash 字段，更新静态断言和 package
identity；在 client 尚未迁移前允许构建暂时失败，但不保留旧/新双格式。

检查：client handshake 能识别新 fork；stock DXC、旧 fork、旧 metadata 和旧 request 均 fail closed；
`rg` 在 active code 中不再找到 `IncludeSource` 或 `CompileInputHash`（历史 ADR/research 除外）。

### M1：实现 fork filesystem invocation（已完成）

实现 path-list validation、统一 argument builder、default handler 和 frontend discovery；随后删除
手工展开、input hash 与独立 `-P` validation。先验证 compiler harness，再接 RadRay client。

检查：include-free root、nested include、缺失 include、ordered shadowing、相对 path/CWD、空 path
list、discovery diagnostics 和 DXIL/SPIR-V 两 lane 均有独立断言。

### M2：迁移 shader client 与 JIT（已完成）

更新 canonical request、client marshaling、result decode、JIT constructor/const calls。所有旧调用
点必须在本阶段结束前迁移，不能通过默认空路径兼容旧构造函数。

检查：compiler client suite、metadata suite、JIT negative paths、JIT 并发读取同一 immutable list；
JIT 关闭时的 stub 构建也通过。

### M3：迁移工具、样例和 fixture（已完成）

改 raw CLI、Lambert sample、shaderlib pass tests、runtime tests、render fixture generator 和 CMake
工作目录约定。不得添加 shaderlib copy step；只保留 DXC runtime deployment。

检查：CLI 单路径/多路径命令、样例启动、shaderlib pass discovery/compile、所有 fixture 生成结果。

### M4：更新制品、文档并做全量验收（已完成）

重新打包 fork SDK、回写 manifest/hash、更新 metadata goldens、架构文档、build guide、research
report、样例 ADR 与本计划状态。完成静态依赖、ABI map/import、build、CTest 和 `check_docs.py`
验证。

## 验证命令

按 `docs/guide/build-test.md` 顺序执行，禁止 build/test 并行：

```powershell
python tools/fetch_sdks.py restore
cmake --preset win-x64-debug
cmake --build build_debug --parallel 24
ctest --test-dir build_debug -C Debug -R RadRayShaderCompilerClient --output-on-failure
ctest --test-dir build_debug -C Debug -R RadRayDxcMetadata --output-on-failure
ctest --test-dir build_debug -C Debug -R RadRayShaderLibPass --output-on-failure
ctest --test-dir build_debug -C Debug -R RadRayRuntimeShaderJit --output-on-failure
python tools/check_docs.py
git diff --check
```

另建 runtime-only preset 验证 compiler-free 分支，使用 `ninja -C build_debug -t commands` 或
`link /MAP` 检查 target dependency，不用 `dumpbin /DEPENDENTS` 判断 Vulkan 动态加载边界。

## 完成定义

- active code 不再有 caller-owned include closure、手工 include resolver 或 `CompileInputHash`。
- discovery 与 concrete compile 对同一稳定 include tree 使用同一份 caller path list 和 DXC handler。
- JIT、tool、样例和测试均显式配置 include path；不存在隐藏 shaderlib/CWD 路径。
- 新旧 ABI/schema/toolchain/package 不能混用，失败包含可定位 diagnostics。
- 现有 shader compiler、runtime JIT、D3D12/Vulkan 和 compiler-free 验证全部通过，且文档与实现一致。

## 后续独立议题

- discovery 的 Defines/CompilePolicy parity、include 中 entry declaration 的语义与 scanner 退役已纳入
  `docs/todo/radray-dxc-frontend-semantic-migration.md`。
- include dependency graph、外部缓存失效协议和 hermetic/AOT bundle API。
