# ADR-0018 filesystem-backed shader compilation 与 ABI/schema 断代

状态: 部分被 ADR-0019 取代
日期: 2026-08
影响: RadRay DXC fork extension、`radrayshadercompiler`、shader request/metadata wire、shader JIT、shader tools、`shaderlib/**`

## 背景

当前 RadRay 扩展请求要求调用方递归收集并提交 `vector<IncludeSource>`，fork 内部再手工展开
include，并把收集到的 include 名称和内容编码进 `CompileInputHash`。这使调用方承担了本应由
编译器完成的文件解析与读取职责，也把共享库源码内容错误地提升为 shader 稳定 identity 的一部分。
它与标准 HLSL 编译流程的 filesystem-backed include 语义不一致，并使 runtime JIT、工具和测试各自
维护 include closure 的风险变大。

## 决策

### 1. 统一使用 filesystem-backed 编译

`IRadRayDxcCompiler::DiscoverSourceContract` 与 `CompileVariant` 都使用同一份不可变 compile
environment。调用方提交 root `.hlsl` 的原始 `RootSource` 和逻辑 `SourceName`；compiler 在每次
操作中通过 environment 的唯一 `ShaderlibRoot`，使用 DXC 的标准 filesystem include 机制读取
transitive `.hlsli`。调用方不再提交 `IncludeSource`，compiler 不再手工预展开 include。

当前 API 不同时支持 content bundle 与 filesystem 两种语义；若将来需要 hermetic/AOT source
bundle，另行设计独立 API。

### 2. compile environment 与 shader request 分离

`ShaderlibRoot` 作为独立 ABI 参数传给 discovery/compile，不编码进 canonical shader request，
也不存入可变的 compiler 全局状态。`ShaderJit`/compiler client 在其生命周期内持有不可变
environment，并保证 discovery 与 compile 使用同一环境。

### 3. include 内容不参与 shader input identity

删除 `CompileInputHash` 及其所有 wire、缓存和测试语义。`ContractHash` 只覆盖 canonical keyword
domain、entry topology 以及影响 discovery 的 structured defines/policy；不覆盖 shaderlib include
字节。`BytecodeHash`、`PipelineLayoutHash`、`GpuArtifactHash` 保留为 compiler output identity，
不承担外部源码变更的失效检测职责。何时重新编译或使外部产物失效由调用方的构建/资产系统负责。

### 4. 做一次干净的 ABI/schema breaking upgrade

提升 RadRay DXC extension ABI、compile request wire schema 与 metadata schema 的版本。新 client
只接受新 fork ABI 和新 wire；旧 fork、旧 request 与旧 artifact 直接 fail closed。不保留
`IncludeSource` legacy adapter、旧 input-hash 兼容字段或双语义 fallback。fork SDK 重新打包，
manifest/archive identity 与 golden artifacts 同步更新。

## 放弃的方案及代价

- **继续由调用方提交 include closure**：会复制编译器的文件解析职责，容易与 DXC 预处理语义分叉，
  并把 include 内容错误地固定进输入 identity。
- **在现有 request 上兼容两套语义**：短期减少 ABI 迁移工作，但会让同一个入口既可能使用调用方
  snapshot 又可能读取文件系统，缓存和诊断行为无法稳定解释。
- **把 `ShaderlibRoot` 编进 shader request/hash**：会把部署环境路径污染为 shader identity，
  也不能表达“同一逻辑 shader 在不同工作区编译”的复用关系。

## 必须保持为真

- root source 仍由调用方以原始字节提交；共享 include 由 compiler 在 compile environment 中从文件系统读取。
- discovery 与 concrete compile 使用相同的 `ShaderlibRoot`，且都不接受 caller-owned include closure。
- shaderlib include 内容不进入 `ContractHash` 或其他稳定 shader input identity。
- 新旧 ABI/schema 不混用；版本不匹配时 fail closed。
- include 根仍是唯一的 shaderlib 根，authoring 使用 root-relative angle includes。
