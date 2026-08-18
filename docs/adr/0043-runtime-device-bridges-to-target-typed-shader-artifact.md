# ADR-0043 运行时 Device 通过单一动态桥消费 target-typed shader artifact

状态: 生效
日期: 2026-08
影响: `radrayshader` artifact view、`radrayrender` pipeline layout 构造、runtime JIT smoke、
`example_lambert_sphere`

## 背景

DXIL artifact 只能交给 D3D12，SPIR-V artifact 只能交给 Vulkan。M5 因而把
`Device::CreatePipelineLayout` 从公共 RHI 删除，让两个后端的 concrete device 分别接收
`DxilShaderArtifactView` 与 `SpirvShaderArtifactView`，用类型系统守住不可互换性。

但 runtime 与样例先在运行时选择 `Device`，再只编译该 device 对应的 target。若每个调用方都直接
访问 concrete device，它们就必须重复 backend 宏、artifact decoder、下行转换和 layout 构造阶梯。
公共提交接口同时消费 `BindingHandle`，公共 `PipelineLayout` 却没有按 HLSL declaration name
取得 handle 的入口，调用方还要再次下行转换。

## 决策

`radrayrender` 提供一个面向运行时所选 `Device` 的单一动态 artifact 桥。调用方必须显式传入
`ShaderArtifactDecodeOptions`；桥先验证
`Device backend == requested target`，再调用该 target 的 typed decoder。decoder 继续验证
`requested target == artifact envelope target` 与独立可信的 `ExpectedGpuArtifact`。只有这些检查全部
成功后，桥才把 `DxilShaderArtifactView` 交给 `DeviceD3D12::CreatePipelineLayout`，或把
`SpirvShaderArtifactView` 交给 `DeviceVulkan::CreatePipelineLayout`。任一步失败都原路返回，不尝试
另一 target，也不做 backend fallback。

后端 concrete device 的两个 typed 入口和正反向 `static_assert` 编译边界保留。动态桥只把运行时
分派集中在 `radrayrender`，不新增公共 layout descriptor，不允许 caller 构造第二份 binding layout。
这是对 ADR-0011“只有 `Device::Create` 做后端分派”的窄化：后端仍只由 descriptor 创建且绝不自动
回退；额外允许的只有当前 device 与 compiler artifact 之间这一处 fail-closed 桥接。

公共 `PipelineLayout` 增加 `FindBinding(std::string_view)`，只把当前 layout 的 declaration name
解析为不透明 `BindingHandle`。它不公开 group、slot、Root Signature、descriptor set layout 或其他
layout 数据。

该桥属于 `Shader runtime representation`，不负责 AssetId/PassName、Variant assignment、artifact
加载缓存或 PSO 缓存；这些仍属于 `Shader artifact orchestration`。

## 放弃的方案及代价

- **每个运行时调用方保留 backend 宏与 concrete cast**：不削弱调用点的 typed 形态，但同一套
  fail-closed 阶梯会在样例和测试中继续复制，公共 `BindingHandle` 颁发路径仍不完整。
- **恢复公共 `Device::CreatePipelineLayout` 或公共 layout descriptor**：接口更短，但重新引入 M5
  已删除的 target-erased layout 构造面，caller 也可能建立第二份 layout schema。
- **只按 device 推断 target**：省去一个参数，但不能核对 JIT request/result 已明确携带的 target；
  显式 options 让 device、request 与 artifact envelope 三方必须一致。
- **为了消除分派先建立 ShaderAsset/MaterialAsset**：资产生命周期与 identity 不会消除 target-native
  RHI 分支，且会在 per-Variant artifact PSO cache 尚未裁决前锁定资产边界。

## 必须保持为真

- concrete backend layout 入口只接受匹配的 typed artifact view，反向组合继续编译失败。
- 动态桥只在 `radrayrender` 内访问 backend impl 头；runtime、example 与普通 render 消费方不下行转换。
- device、请求 target 或 artifact envelope 任意不一致都 fail closed，不尝试另一 lane。
- 公共 `PipelineLayout` 只颁发 opaque handle，不暴露或接受公共 layout 描述数据。
- 动态桥不依赖 shader compiler client、runtime 或资产系统。
