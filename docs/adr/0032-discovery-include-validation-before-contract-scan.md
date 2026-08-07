# ADR-0032 discovery include validation before contract scan

状态: 已被 ADR-0034 取代
日期: 2026-08
影响: RadRay DXC fork、source contract discovery、shader compiler client

## 背景

当前 fork 的 source contract discovery 直接扫描 root source 文本，没有进入 DXC frontend，因此
即使 concrete stage compile 使用 filesystem include handler，discovery 也不会读取或诊断嵌套
include。若把 root-only scanner 直接替换成处理预展开文本的实现，又会在本次 include 修正中
扩大 contract 语义和 source-origin 处理范围。

## 决策

discovery 增加一个由 DXC 执行的预处理验证步骤：使用 raw root `DxcBuffer`、逻辑 `SourceName`、
调用方提供的同一份 ordered `-I` path list 和每次 invocation 新建的 default filesystem include
handler，调用 `IDxcCompiler3::Compile` 的 `-P` 模式。预处理输出不进入 contract hash，也不作为
contract scanner 的替代输入；验证成功后仍由现有 root-only scanner 从 root source 提取
keyword domain 与 entry topology。

`CompileVariant` 的内部 discovery 先复用同一 include validation，再进行 assignment/contract
校验和 target stage compile。缺失或无法解析的 include 在 discovery 阶段以 compiler diagnostics
失败。`.hlsli` 继续不能声明 RadRay keyword group；允许 include 文件参与 contract 语义是独立的
后续设计，不随本次 filesystem-backed include 修正引入。

## 放弃的方案及代价

- **discovery 继续只扫描 root，只有 stage compile 读取 include**：两个阶段观察到的 source
  依赖不同，缺失 include 会延迟到 concrete compile，违背已确认的统一 include 边界。
- **本次直接把 contract scanner 迁移到预处理输出或 DXC AST**：能扩大语义覆盖，但会同时改变
  include source-origin、宏展开和 root-only pragma 约束，超出“先解决 include”的范围。

## 必须保持为真

- discovery 和 concrete compile 使用相同的 caller path list、DXC include handler 和文件读取规则。
- discovery 不保存预处理输出、不构造 caller-owned include closure，也不把 include bytes 纳入任何
  shader identity hash。
- root-only contract scanner 的现有 authoring 边界保持不变。
