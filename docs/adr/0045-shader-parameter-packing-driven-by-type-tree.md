# ADR-0045 CPU 参数值由 compiler type tree 驱动打包，不写 CPU mirror struct

状态: 生效
日期: 2026-08
影响: 新增 runtime 参数打包组件、material 与 view/object 参数写入路径、
`examples/example_lambert_sphere`；消费 `modules/shader/include/radray/shader/shader_artifact.h`
的 `ShaderArtifactView::Types()`

## 背景

ADR-0016 要求 compiler 为每个 target result 输出运行时构造 CPU buffer 所需的完整 target-native
cbuffer/struct type tree，包含 nested members、names、offset/size、array/matrix stride 与
scalar/vector/matrix shape。这份 type tree 已经落地：`WireTypeRecord` 携带
`Name / ParentIndex / Kind / ElementCount / Offset / Size / Stride / TypeIndex`，decoder 已校验
range、record kind、offset/size/stride、type reference、父结构范围、同级名称与父链环路，
`RadRayRenderShaderArtifact` 的 golden 与 negative 用例都在跑。

但它**没有任何生产消费者**：`rg WireTypeRecord` 只命中 shader 层自身与
`test_radray_render_shader_artifact.cpp`。

代价直接体现在 `example_lambert_sphere.cpp`：它为 HLSL 的 `FrameData` 与
`lighting/lights.hlsli` 的 `DirectionalLight` 各写了一份 C++ mirror struct，并用五条
`static_assert(sizeof/offsetof)` 手工核对 ABI，然后 `memcpy` 进 upload slice。这套写法有三个问题：
mirror struct 是 HLSL 布局的第二份真相；`static_assert` 只能验证它自己声明的那几个 offset，
不能验证 HLSL 侧真实布局；shader 改一个字段顺序时 C++ 侧不会失败，只会画错。

## 决策

**CPU 侧参数值按名字写入，由 artifact 的 type tree 决定字节布局。禁止为 shader cbuffer/struct
编写 C++ mirror struct 或 `offsetof` 断言。**

机制：

1. 从 `ShaderProgram` 的 artifact type tree 预建一张**扁平参数名索引**：
   `参数名 → (binding, byteOffset, Kind, Size, Stride, ElementCount)`。
   作者写 material 时不需要知道参数落在哪个 cbuffer —— 与 Unity/UE 的心智模型一致。
2. 扁平名在同一 program 内**必须唯一**。两个 cbuffer 声明同名成员时索引构建失败，
   program 创建 fail closed。不做"最后一个赢"或按 binding 消歧。
3. 写入是 typed 的（`SetFloat` / `SetFloat4` / `SetMatrix4x4` / `SetTexture` / `SetSampler`）。
   **Kind 与 Size 不匹配时拒绝写入并报错**，不截断、不补零、不静默扩展。type tree 已经带了
   Kind 与 Size，这个检查是免费的。
4. 打包器输出连续 bytes，交给 `DynamicCBufferArena` 的 reservation；纹理与 sampler 不进 bytes，
   直接走 `ShaderParameterSet::Set`。
5. 未被写入的参数保持零值。缺失的必需参数不作为错误 —— HLSL 侧读到零是确定行为，而"哪些参数
   是必需的"不属于 compiler contract。

**打包器不挂在 `ShaderParameterSet` 上。** 公共 RHI 的 `ShaderParameterValue` 是
`variant<ShaderBufferBinding, ShaderTexelBufferBinding, TextureView*, Sampler*>` —— 全是绑定
描述，不是数据。cbuffer 内容必须由调用方写进 upload slice 再把 slice 绑上去。因此打包器是一个
独立的"命名值 → bytes"组件，不需要改动 RHI 接口。

**语义错配仍不属于运行时校验范围。** ADR-0016 已定：type tree 的语义错配是 compiler/ODR 系统
缺陷，runtime 只检查 wire bounds、record kind、offset/size、stride 与 CPU 构造安全性。本决策
新增的检查是**调用方写入值与 type tree 的匹配性**，不是对 type tree 自身正确性的二次核对。

## 放弃的方案及代价

- **维持 CPU mirror struct + `offsetof` 断言**。零新代码，且 memcpy 是最快的写入路径。代价是
  每条 pass 都要维护一份手写布局副本，而断言只能验证它自己写下的 offset —— HLSL 侧改字段顺序
  时 C++ 编译通过、渲染出错。type tree 也会继续零消费者，等于 compiler 白输出了这份数据。
- **调用方直接给 byte blob，引擎只做 memcpy**。接口最薄，但把布局知识完全推回调用方，等于
  mirror struct 方案换了个包装，且连 `offsetof` 断言那点保护都没有了。
- **按"binding 名 + 成员路径"寻址**（`Set("Frame", "Model", v)`）。消歧天然正确，不需要唯一性
  检查。代价是材质作者必须知道参数被放进哪个 cbuffer —— 而那是 shader 作者的布局决定，一旦
  重新分组 cbuffer，所有材质代码都要改。撞名是可检测的，把参数归属泄漏给材质作者不可回收。
- **`SetFloat4` 写到 `float3` 成员时截断**。少一类错误路径，写起来更宽松。但截断会静默丢掉
  一个分量，而这类错误的表现是"颜色略微不对"，是最难查的一类。
- **给 type tree 加独立 `CpuSchemaHash` 并缓存打包计划**。ADR-0016 明确禁止：type tree 不作为
  独立 hash 输入，不独立缓存、寻址或跨 Variant 复用，必须与所属 target result 原子交付和存活。
  扁平索引因此只在 program 生命周期内有效，不跨 program 共享。

## 必须保持为真

- `rg` 在 runtime 与 examples 的 shader 参数路径中找不到针对 HLSL cbuffer/struct 的
  `offsetof` 断言或 mirror struct。
- 扁平参数名撞名时 program 创建失败，不静默选择其中一个。
- Kind 或 Size 不匹配的写入返回失败，不修改任何字节。
- 打包器不调用 DXIL/SPIR-V reflection API，只读 `ShaderArtifactView::Types()`。
- 扁平索引不跨 `ShaderProgram` 共享，也不按 hash 缓存。
- `ShaderParameterValue` 的 variant 成员不因本决策增加数据载体类型。
