# ADR-0006 shader_types.h 的收录标准是"是不是 manifest 数据"

状态: 生效
日期: 2026-07
影响: `modules/shader/include/radray/shader/shader_types.h`、`modules/render/include/radray/render/rhi.h` 的类型归属

## 背景

拆出 `radrayshader`（见 ADR-0002）时必须决定：哪些类型跟着格式层走，哪些留在 `rhi.h`。

第一次用的判据是"**它是否持有 device 指针**"。结果把 8 个纯 RHI 参数类型
（`PipelineLayoutDescriptor`、`VertexInputState`、`ShaderDescriptor` 等）搬进了 shader 库，
而 shader 库从未使用它们。

## 决策

判据改成"**它是不是 manifest 的内容**"——即：它是否出现在 `*.shader.json` 里，
或是否有 JSON codec？

按此标准收录的类型，每一个都能在 `shader_manifest` 里找到对应字段或 codec：

| 类型 | 对应 |
|---|---|
| `ShaderStage` / `ShaderBlobCategory` | 编译阶段与字节码类型，贯穿工具链 |
| `ShaderParameterBindingType` | `ShaderBindingDesc::Type` |
| `VertexFormat` / `VertexStepMode` | `ShaderVertex{Attribute,Buffer}Desc` 的字段 |
| `ShaderBindingLocation` | `ShaderPushConstantDesc::Location`（有 codec） |
| `SamplerDescriptor` | `ShaderBindingDesc::ImmutableSampler`（有 codec） |
| `AddressMode` / `FilterMode` / `CompareFunction` | `SamplerDescriptor` 的成员，随其被序列化 |

只为喂给 RHI 而存在的类型（`PipelineLayoutDescriptor`、`VertexInputState`、
`ShaderDescriptor`…）留在 `rhi.h`，由 `modules/runtime/shader_program.h` 负责把 manifest
数据打包成它们。

`RenderBackend` 留在 `rhi.h`：shader 侧 API 一律直接收 `ShaderBlobCategory`，
由调用方决定要哪种字节码。**刻意没有任何 `RenderBackend → ShaderBlobCategory` 的映射函数。**

**命名空间刻意保持 `radray::render`**，与 `rhi.h` 一致。两个库共同实现 render 这一概念层，
拆库是物理构建边界而非概念重命名；改名会让 `ShaderStage` 这类跨库类型出现割裂，
且要改动全仓库每一处 `render::` 限定。

## 放弃的方案及代价

- **判据 = "是否持有 device 指针"**。必要但不充分。用它把 8 个纯 RHI 参数类型拽进了
  shader 库。教训可推广为一个问法：**"如果我删掉这一个便利函数，这一层还需要它吗？"**
  `RenderBackend` 当初看起来属于 shader 层，唯一理由就是有人在那里写了
  `GetShaderBlobCategoryForBackend`——一个便利函数把一个类型拽低了一层。
- **`shader_types.h` 改用新命名空间**（如 `radray::shader`）。要改全仓库每处
  `render::ShaderStage`，且让同一概念层出现两个命名空间。
- **不拆，`shader_types.h` 留在 render**。CLI 就得链进整个后端，正是 ADR-0002 要解决的问题。

## 一个计算依赖闭包时的陷阱

判断"某个类型能否搬走"要算依赖闭包，而**枚举成员必须排除**。

`ShaderParameterBindingType` 的成员名叫 `Buffer` / `Texture` / `Sampler`，与 device class
同名。若把枚举成员当成类型引用，闭包会经
`ShaderParameterBindingType → Buffer → Device` 污染到几乎整个 `rhi.h`（实测 110/133 个类型），
从而误判拆库不可行。枚举成员是值，不是类型依赖。

## 必须保持为真

- `shader_types.h` 里的每个类型都出现在 `*.shader.json` 中或有 JSON codec。
- 不存在任何 `RenderBackend → ShaderBlobCategory` 的映射函数（任何一层都没有）。
- `shader_types.h` 的命名空间是 `radray::render`。
- 判断类型归属时用"是不是 manifest 数据"，不用"是否持有 device 指针"。
