> - 适用: 理解 RHI 绑定布局由 shader artifact 推导转为调用方显式定义的原因，以及实施本次重构
> - 权威: 2026-10-22 已确认设计、实施前快照与 Codex 实施计划；不是当前已实现的 RHI 契约
> - 锚点: `modules/render/include/radray/render/rhi.h`, `modules/render/include/radray/render/backend_shader_artifact.h`, `modules/render/include/radray/render/shader_layout.h`, `modules/render/src/rhi_validation.cpp`, `modules/render/CMakeLists.txt`, `cmake/RadRayConfig.cmake.in`

# 显式 RHI 绑定布局：转向原因与 Codex 实施计划

日期：2026-10-22。状态：设计已确认，代码未实施；本记录先于旧链路删除保存。
源码核查基线：`f3f92d13514389dc27cde03e93bd186727c657b3`，记录前工作树干净。
范围：shader 输入、参数绑定、Root Signature / PipelineLayout、模块依赖、构建安装及相关测试。
当前实现见 [RHI 与后端](../architecture/render-rhi.md)、[Shader pipeline](../architecture/shader-pipeline.md)；
后续实现只更新所属长期文档，本历史快照保持原样。

这里的“用户定义”指 **RHI 调用方或上层引擎定义绑定 ABI**。Shader 仍须声明与其一致的
register、space、set、binding；转移的是原生布局的定义权，而非取消 shader 中的资源声明。
输入来自本次 shader/RHI 调研、用户提供的薄 RHI 方案，以及用户随后确认的宽松校验与固定 sampler 归属。
本文保存完整决策和执行要求，使用它不需要本机 Downloads 或会话附件。

## 一、为什么改变定义权

目标是把 render 做成显式绑定布局的薄 RHI：上层定义稳定的资源组和后端绑定位置，render 创建原生布局并执行绑定，
shader 通过字节码进入 PSO。Shader 编译、反射及资产策略可以由上层选用，不成为使用 render 的前置条件。

旧设计选择 shader artifact 作为权威输入有合理目的：减少重复声明，通过 typed contract、名字、类型和布局解析尽早发现错误。
但它把一部分引擎策略放进了 render。面对独立组复用、稳定变体 ABI 和独立使用 RHI 的目标，这个边界需要调整。
本次决定来自源码结构与需求的冲突，并非一次已复现的驱动故障，也没有以性能测试证明新方案一定更快。

| 遇到的问题 | 基线中的具体表现 | 对新目标的影响与解决方向 |
|---|---|---|
| 布局权威绑定到编译产物 | artifact 解码、target layout、resolver/modifier 决定原生布局；D3D12 显式 carrier 与 Vulkan active declarations 的表达并不完全相同 | 变体所用资源变化与上层期望保留的稳定 ABI 混在一起。改由调用方声明完整布局，不随未使用资源自动裁剪 |
| 参数组依附完整布局 | `ShaderParameterSetDescriptor` 输入 `PipelineLayout* + GroupIndex`；`BindingHandle` 带 layout 对象身份和 metadata record index | 一个独立资源组的创建、更新和动态偏移需要完整布局上下文。引入独立 `BindingGroupLayout`，使用组内数字 ID |
| 语义、命名和原生位置耦合 | 通过 shader 名称查 binding，resolver 同时处理逻辑类型、target 位置、placement 和固定 sampler | 调用方难以仅凭 RHI 描述显式决定 ABI。拆开逻辑 ID、逻辑组槽、DX/VK 位置和内部 root parameter 编号 |
| render 的依赖超过原生执行需要 | 公共布局类型引用 shader enum、hash/wire sampler，artifact 桥暴露 shader diagnostic；target、测试和安装组件依赖 shader | 只删除一个 include 不足以独立使用 render。公共类型、实现、测试、CMake 和安装导出一起解耦 |
| 复用检查承担过多策略 | 组绑定的校验会比较布局结构及映射，handle 也限制跨 layout 包装使用 | 用户选择把合法的 native ABI 兼容性交给调用方；RHI validation 只做宽松的槽位、类型、数组和范围误用检查 |

“shader 驱动布局”与“独立组复用”并非理论上互斥，也可以继续增加反射合并、兼容签名和适配层。
本次没有选择继续扩展这条路径，因为项目目标已经明确为调用方控制 ABI、render 只做原生执行。
同样，不把 shader metadata 类型搬到 core 来维持原链路；这会保留职责耦合，只改变依赖所在目录。

## 二、重构前快照

### 职责、依赖与数据流

```text
shadercompiler → shader/core：编译、typed target artifact 与字节码
shader         → core：wire contract、artifact decoder
render         → shader/core：artifact 桥、layout resolver、native layout 与绑定

metadata blob
  → PrepareBackendShaderArtifact / typed decoder
  → ResolvedD3D12Layout 或 ResolvedVulkanLayout
  → CreateBackendShaderArtifact / 内部 layout 工厂
  → PipelineLayout
  → ShaderParameterSet(Layout, GroupIndex)

PipelineLayout::FindBinding(name) → BindingHandle
  → Set(handle, arrayIndex, value) / dynamic offsets / SetPushConstants
```

`ShaderDescriptor` 已支持字节码输入；entry 保存在 PSO 使用的 `ShaderEntry` 中。
问题主要集中在布局创建与绑定元数据，不需要为了新边界重做 shader 字节码入口。
基线 `modules/CMakeLists.txt` 无条件加入 shader；render target 和 render 测试链接 shader，安装组件也把 shader 作为 render 前置。
render 下的 fixture generator 写出的部分 `.dxil.bin` / `.spirv.bin` 实际是 metadata artifact，不能按文件扩展名当作裸字节码。

### 所有权、线程与帧生命周期

- Device 使用共享 owner；普通 shader、layout、parameter set、PSO 等工厂返回可空的独占 owner，依赖对象通过指针借用。
- 参数集借用完整 PipelineLayout，校验包装记录该依赖；调用方必须保证 layout、set 和实际资源覆盖全部使用期。
- RHI PipelineLayout 包装引用 device 内的 native layout cache 条目。Vulkan set layout、sampler 缓存和 D3D12 RS 缓存具有各自原生引用关系；缓存复用不延长上层业务资源的安全使用期。
- 同一对象和共享 command allocator 由调用方串行操作；layout cache 的操作及引用增减按既有单线程约束执行，不新增共享全局状态。
- 校验开启时，参数集首次绑定后封存，禁止原地 Set；调用方确认全部引用 epoch 已结束后才可 `ResetBindingsForReuse`。
- allocator Reset 必须发生在相关 GPU 工作真实完成以后；CPU End/Submit、校验层可见依赖消失或缓存命中均不是完成凭据。
- 当前 RHI 不自动上传参数、不维护业务帧版本，也不保证发现全部 GPU 在途误用；本次重构继续保持这个责任边界。

### 旧代码恢复入口

以下路径按上述基线理解，后续删除不会回写本记录。使用 `git show <基线提交>:<路径>` 查阅：

| 路径 | 核查内容 |
|---|---|
| `modules/render/include/radray/render/rhi.h` | 字节码入口、参数集描述、BindingHandle、命令接口 |
| `modules/render/include/radray/render/backend_shader_artifact.h` 与同名 `.cpp` | artifact prepare/create/hash 桥 |
| `modules/render/include/radray/render/shader_layout.h` 与同名 `.cpp` | target resolver、modifier、shader wire 类型依赖 |
| `modules/render/include/radray/render/pipeline_layout_types.h` 与同名 `.cpp` | 内部 layout 工厂、名字记录、artifact 辅助校验 |
| `modules/render/src/rhi_validation.cpp` | 包装对象、绑定检查、依赖和封存状态 |
| `modules/render/src/d3d12/d3d12_impl.cpp`、`modules/render/src/vk/vulkan_impl.cpp` | native 布局生成、资源写入与绑定 |
| `modules/render/tests/shader_fixture_generator.cpp` | fixture 生成方式及 metadata/bytecode 区别 |

其中公共头的同名实现位于 `modules/render/src/`，不是 include 目录。

## 三、已确认的解决方案

### 模块边界与对象关系

```text
radrayrender         → radraycore
radrayshader         → radraycore
radrayshadercompiler → radrayshader / radraycore

GroupDesc → BindingGroupLayout → ShaderParameterSet
                 ↓
PipelineLayoutDesc → PipelineLayout
                          ↓
原始 shader 字节码 + ShaderEntry + PipelineStateDesc → PipelineState
```

render 在自有模块中只依赖 core，原生后端和既有第三方依赖按需保留。
需要同时使用 compiler、artifact 和 render 的上层自行连接它们；本次不新增 Program、材质或 renderer 层。

采用独立组布局，保留现有 `PipelineLayout`、`ShaderParameterSet` 和绑定命令名称，避免无关重命名。
ShaderParameterSet 只借用创建时的 BindingGroupLayout；PipelineLayout 按逻辑 `GroupSlot` 组合组布局并借用它们。
组布局复制创建描述和所需映射，创建用的 span 不需要长期存活；组布局 owner 必须晚于参数集和引用它的 pipeline layout 释放。
PSO、命令、资源和 native cache 的既有完成与销毁契约继续适用，不增加隐式生命周期管理器。

### 公共接口与位置表达

| 接口或类型 | 实施契约 |
|---|---|
| `BindingGroupLayoutDescriptor` | 定义组内 BindingId、资源 Kind、Count、Stages，以及 D3D12 和 Vulkan 映射；固定 sampler 状态属于此描述 |
| `PipelineLayoutDescriptor` | 显式的 GroupSlot 到组布局的组合、push constant 描述和原生创建所需选项；不接受 artifact 或 shader 反射 |
| Device 创建入口 | 新增 `CreateBindingGroupLayout(desc)`、公开 `CreatePipelineLayout(desc)`；沿用可空独占 owner 和既有失败报告方式 |
| `ShaderParameterSetDescriptor` | 由 `PipelineLayout + GroupIndex` 改成 `BindingGroupLayout*` |
| `ShaderParameterSet::Set` | `Set(bindingId, arrayIndex, value)`；继续使用现有 buffer、texel buffer、texture view、sampler 值表示 |
| `CmdEncoder::BindShaderParameterSet` | `BindShaderParameterSet(groupSlot, set, dynamicOffsets = {})`；映射取自当前 PSO 的 PipelineLayout |
| `ShaderParameterDynamicOffset` | `{BindingId, ArrayIndex, Offset}`；ID 来自资源组，不带完整 PipelineLayout 的身份 |
| `CmdEncoder::SetPushConstants` | `SetPushConstants(pushId, byteOffset, data)`；ID 属于当前 PipelineLayout，允许块内按 4 字节对齐的部分更新 |

BindingId、GroupSlot、PushId 使用调用方提供的无符号数字，分别属于组内、pipeline 组槽和 pipeline push 块的命名空间，
不编码对象地址、字符串名字或 native slot。资源 Kind 使用 render 自有枚举，覆盖现有常量、structured/raw/texel
只读和可写 buffer、sampled/storage texture 及 sampler，不引用 shader enum。

D3D12 映射明确 register class、index、space 和 table/root descriptor placement；Vulkan 映射明确 set、binding 和 regular/dynamic placement。
两套映射可以同时提供，当前 device 只消费自己的字段；公共结构不包含 SDK 类型。活动后端所需映射由调用方提供，禁止自动重编号。
第一版一个组对应一个 Vulkan set，组内 set 相同；GroupSlot 不必等于 set。PipelineLayout 中不同组的 native set 不重叠，set 编号空洞使用空 set layout 补齐。
D3D12 root parameter 编号由后端产生，组内 descriptor 存储位置与完整 RS 的 root 编号分离。

固定 sampler 归 BindingGroupLayout，PipelineLayout 通过组合继承；固定项不可通过 Set 修改。
使用 render 自有 sampler 描述，补齐替代当前固定 sampler wire state 所需的 bias、border、reduction 和 unnormalized 语义，
同步普通 sampler 的创建和 cache key；既有默认值不变。目标后端不支持的组合明确报告失败，不静默改变含义。

### 动态偏移与校验边界

动态 buffer 偏移保留为可选能力，普通绑定为默认。Vulkan 按布局预计算的顺序打包 offsets，D3D12 用声明为 root descriptor 的 buffer 地址加偏移。
保留当前动态/root buffer `Count == 1` 的边界，因此其 ArrayIndex 为 0；普通 descriptor 数组继续支持。
每个动态项显式提供一个 offset，包括 0，不把缺省当作 0。校验层检查遗漏、重复、对齐和有效 buffer 范围。
Push constants 保持 Vulkan 一个逻辑块、D3D12 可多个 root constant 块的当前能力范围；固定 sampler 保持单元素，不顺带扩大数组能力。

用户明确选择：**兼容检查只在可选 RHI validation 内进行，且保持宽松**。
检查对应槽位可接受的资源类型、ID/数组边界、资源范围和必要调用状态；沿用 device 归属与已封存对象的误用检查。
不要求整个 PipelineLayout 或 BindingGroupLayout 是同一对象，不以完整布局结构、native 坐标、stage mask 或布局 hash 作为绑定放行条件。
正常后端路径不增加兼容性扫描、反射校验或逐次适配；它按调用方提供合法数据的契约执行。

Shader 与布局 ABI、独立组对象与当前 native pipeline layout 的真实兼容性由调用方保证。
宽松校验通过不证明 native 兼容，原生创建和调试层只能报告其能够发现的问题。
能力探测、布局构建必需的错误处理和原生 API 失败处理保留；放宽组兼容检查不等于取消这些机制。
**native cache 仍以完整创建语义判等**，确保缓存正确；这与每次绑定是否检查组兼容性是两件事。

### 取舍与非目标

收益是布局可以由上层稳定定义，资源组可以独立创建和复用，render 可以脱离 shader/compiler 使用。
代价是 shader 与 CPU 显式描述需要调用方同步，部分错误不再由 RHI 提前检出；buffer 内的数据布局、上传位置和使用期也继续由上层负责。
这里不承诺零配置绑定、任意布局互换或测得的性能提升。

不在本次增加成员级 shader schema 校验、shader 自动重绑定、任意序列化 RS 导入、bindless 体系、
自动上传分配、帧版本或 GPU 在途资源管理。编译器已有 wire contract 和 RootSignature 编译策略保持独立，
render 停止把它们作为布局输入；HLSL 仍遵守唯一 platform gate 和直接数字 `VK_BINDING(binding, set)` 约定。

## 四、Codex 实施计划

按以下顺序实施，最终一次性迁移仓库调用方，不保留旧 artifact/layout API 的兼容入口。
本记录已经满足删除前历史留档要求，实施时不重复新建同一主题历史，也不修改本记录的状态和内容。

### 1. 建立 render 自有接口

- 增加独立 BindingGroupLayout、两类 layout descriptor、数字 ID 与 render 自有资源/后端位置类型；接口按上表落地。
- 原样复用 ShaderDescriptor 的字节码入口与 ShaderEntry 的 entry 分工；PSO 显式组合 shader、PipelineLayout 和固定功能状态。
- 固定 sampler 归组布局，补齐 render sampler 描述与缓存语义。新增类型按同名 `.h/.cpp` 组织，遵守现有容器、Nullable 和结果类型约定。
- 新增 validation 包装和对象 tag，记录组布局依赖；不引入全局/函数内 static 共享状态，不新增 try/catch/throw。

### 2. 实现两后端布局及绑定

- D3D12 从显式描述生成 tables、root descriptors、root constants、static samplers；每个组形成稳定的 descriptor 存储计划，PipelineLayout 保存它到当前 RS 的 root 参数映射。
- Vulkan 从显式描述创建 descriptor set layouts、pipeline layout、push ranges 和 immutable samplers；参数集按独立组布局分配，绑定时使用当前 pipeline 的 GroupSlot 映射。
- 保留原生缓存、descriptor 分配和提交路径；native cache key 覆盖全部创建语义，不再使用 shader artifact identity。逻辑 ID 不改变原生对象判等语义。
- 将参数更新、动态偏移、push constants 和校验迁到新 ID；native 映射在布局创建时建立，绑定时直接消费，不增加逐次完整布局比较或 descriptor 重排适配。
- 迁移既有封存、ResetBindingsForReuse、资源借用和 native 保活行为，不实现自动数据版本或延迟回收。

### 3. 删除旧链路并完成依赖拆分

- 移除 render 的 backend shader artifact 桥、artifact resolver/modifier、FindBinding 名字查询、BindingHandle，以及依赖 artifact 的 vertex-input 校验；保留与 artifact 无关的输入范围和原生必要约束。
- 替换旧 resolved-layout 工厂和 shader wire 类型；render 公共头及实现不再包含 shader 头、引用 `shader::` 或链接 radrayshader，不将旧 metadata 整体迁入 core。
- 新增默认开启的 `RADRAY_BUILD_SHADER`；compiler 要求 shader，tools 要求 compiler，显式不合法组合在配置阶段清楚报错。最小 render 配置关闭 shader/compiler/tools。
- 按实际构建 target 生成安装组件；render 组件只依赖 core，compiler 组件依赖 shader，保留各组件实际需要的第三方包发现。
- 让外部消费方通过 `find_package(RadRay CONFIG REQUIRED COMPONENTS render)` 使用 `RadRay::render`，无需 shader target 或 DXC。配置和构建不自动下载、准备依赖。

### 4. 迁移 fixture 与测试

- 将纯 artifact 解码和 wire contract 测试迁回 shader 模块；废弃旧 resolver/handle 专用断言，替换为显式布局行为测试。
- render 测试仅链接 render、必要测试依赖，读取预编译的每 stage 原始 DXIL/SPIR-V 和手写布局；不能沿用 metadata artifact 假装裸字节码。
- fixture generator 迁到 shader compiler 侧，作为显式手动、`EXCLUDE_FROM_ALL` 的生成目标；从编译结果的 stage bytecode 范围输出原始字节，记录 HLSL 来源与生成方式。常规 render 构建和测试不得调用生成器。
- 同模块 GTest 仍汇入一个测试可执行文件；不在生产对象添加测试计数或轨迹。构建完成后再执行对应 gtest suite，不并发构建与测试。

### 5. 同步当前文档与完成交付

- 实现完成后更新根 AGENTS 的模块依赖规则，以及架构地图、RHI、shader pipeline、shader authoring、构建安装与测试说明。
- 当前文档解释显式 ABI、逻辑 ID 与 native 坐标、宽松 validation、固定 sampler、所有权和 dynamic offset 契约；删除 RHI 页面临时“已确认、未实现”段落。
- compiler metadata 作为上层可选输入单独说明，不再把 artifact resolver 描述为 render 的唯一合法布局入口。
- 交付实际测试结果、失败或环境限制；不以旧测试计数或 GPU skip 充当本次验收，也不回写本历史快照。

## 五、验收标准

| 场景 | 必须观察到的结果 |
|---|---|
| 逻辑 ID 与 native 位置分离 | 同一组逻辑 ID 在 D3D12/Vulkan 使用不同 register/set/binding，实际 dispatch 和读回正确；GroupSlot 与 Vulkan set 不同也正确 |
| 独立组复用 | 两个完整 PipelineLayout 复用一个参数集；D3D12 因其他组变化而 root 参数编号变化时仍正确；native 兼容的独立组布局对象不被身份校验拒绝 |
| 稳定变体 ABI | shader 变体少用若干资源时仍使用调用方声明的同一组布局，不自动缩减或重新编号 |
| 资源覆盖 | 常量、structured/raw/texel buffer、texture、sampler、普通数组、固定 sampler 和 push constants 完成真实绑定与读回 |
| 动态偏移 | 同一 buffer 的不同偏移得到不同且正确的结果；Vulkan 接受无序的逻辑 offset 记录并按 native 顺序提交；静态 buffer 无需 offsets |
| Push constants | 完整块及非零 byteOffset 的部分更新正确；校验拒绝越界和非 4 字节对齐 |
| 宽松校验 | 开启时诊断未知槽位、不可设置的资源类型、数组越界、固定 sampler 写入、无效动态范围等；不添加完整布局结构必须相等的测试要求 |
| 校验关闭 | 合法输入在双后端结果一致，后端不依赖 validation 对象或隐式补全操作才能正确执行 |
| 缓存与生命周期 | native 相同创建语义仍复用，不同 sampler/push/layout 语义不误命中；组布局、参数集、PSO、cache 引用和安全 Reset/销毁路径正确 |
| 无 shader 的 render | 关闭 shader/compiler/tools 后可构建 render 与 render 测试、安装并由独立外部 consumer 编译链接；包不要求 shader/DXC |
| 原模块回归 | shader/compiler 开启的配置仍可构建，迁移后的 decoder/contract 与编译测试通过；非法开关组合有明确配置错误 |
| 文档 | `python tools/check_docs.py` 和 `git diff --check` 通过；当前行为与历史计划明确分开 |

GPU 验收要求 D3D12 和 Vulkan 实际可用，按仓库规则设置 `RADRAY_TEST_REQUIRED_BACKENDS=d3d12,vulkan`，
需要 GPU validation 的验收设置 `RADRAY_TEST_GPU_VALIDATION=1`。等待真实完成并收集原生诊断后判断结果。
缺少设备、SDK 或驱动验证能力时报告未覆盖项，不算通过；不为补环境擅自修改 third_party/ 或 SDKs/。

本次记录只进行了源码与文档核查，未实施上述重构，也未运行其 C++ 或 GPU 验收。
