> - 适用: 历史方案快照；不再作为实施清单
> - 权威: 无。当前方案以 ADR-0016 与 `hlsl-radray-dxc-shader-pipeline.md` 为准
> - 状态: 已废弃（2026-08）。被 HLSL + forked RadRay DXC 路线取代
> - 锚点: `docs/adr/0014-cpp-trace-is-shader-source-of-truth.md`, `docs/adr/0015-variants-are-cpp-parameters.md`
> - 锚点: `modules/shader`, `shaderlib`, `tools/shader_gen`, `tools/shader_cook`
> - 锚点: `modules/runtime/tests/test_vertical_slice.cpp`, `modules/runtime/include/radray/runtime/gpu_resource.h`
> - 锚点: `modules/render/include/radray/render/rhi.h`

# C++ trace shader 前端：第一期

## 1. 第一期的终点

**trace → codegen → DXC → `ShaderDescriptor` → `PipelineStateCache` → 画出像素。**
双后端（D3D12 + Vulkan），进程内缓存按生成的 HLSL 文本哈希，**不落盘任何产物**。

不在第一期：离线编译、可分发产物、发布构建、"cook"/"bake" 命名（见 ADR-0015）。
第一期 DXC 是运行期必需品。

## 2. 引入方式

只引 LuisaCompute 的两个库，**不引 `dsl/`**：

| 引入 | 形态 |
|---|---|
| `luisa-compute-ast` | SHARED，必须全进程唯一（`TypeRegistry` 是单例，`Type` 身份即指针身份） |
| `luisa-compute-hlsl-codegen` | OBJECT，源码路径引用（无 `install()`，不可 `find_package`） |
| `dsl/` 的一个剪裁拷贝 | 进 RadRay 自己的源码树，剔除 runtime 耦合 |

需要改 LC 的两处：`src/backends/common/hlsl/CMakeLists.txt` 去掉 `luisa-compute-runtime`
（该边无符号依据：23 个 `LUISA_RUNTIME_API` 函数在 codegen 目录零命中），
`hlsl_codegen.h` 删掉未使用的 `#include <luisa/runtime/raster/raster_state.h>`
（唯一带非平凡传递依赖的 include）。

`ast` 只依赖 `core` + `ext`；AST 里 3 处 `#include <luisa/runtime/...>` 全是 header-only POD
（`argument.h` 仅 `<cstddef>/<cstdint>`；`curve_basis.h` 自带注释说明它刻意不拆分实现）。

## 3. 薄 DSL 层

从 LC 的 `dsl/` 剪裁拷贝（Apache-2.0 允许），保留 `var.h` / `expr.h` / `ref.h` /
`operators.h` / `stmt.h` / `sugar.h` / `builtin.h` 的数学与采样子集 / swizzle 表。

必须自己写的两块：

- **资源占位类型**，替代 `resource.h`（它 include `runtime/buffer.h`，而 `Buffer<T>` 持有
  `DeviceInterface*`）。`Var<Buffer<T>>` 本身从 7 行的空 tag `ArgumentCreation` 构造，
  只在 `FunctionBuilder` 上注册类型化参数槽，不需要 handle
- **带 attribute 的结构体反射**，替代 `LUISA_STRUCT`。这是整个方案的关键：`LUISA_STRUCT` 没有
  attribute 通道（`dsl/` 56 个文件里 `Attribute` 零命中），必须直接调
  `Type::structure(alignment, members, attributes)`

`func.h`（`Kernel1D`/`Callable`）不拷：它 include `runtime/device.h`。改为直接构造
`FunctionBuilder`（构造函数与 `push`/`pop`/`push_scope` 都是 public），即 `src/clangcxx/` 的做法。

## 4. 光栅 stage 的硬约束

来自 `src/backends/common/hlsl/codegen_utils/entry_points.cpp` 与 `resource.cpp`：

顶点输入结构的每个成员**必须**带 attribute，键取自这 11 个：

| key | HLSL semantic | 强制类型 |
|---|---|---|
| `position` / `normal` / `tangent` / `color` | `POSITION` / `NORMAL` / `TANGENT` / `COLOR` | 标量或向量 |
| `uv0`…`uv3` | `TEXCOORD0`…`TEXCOORD3` | 标量或向量 |
| `vertex_id` / `instance_id` | `SV_VertexID` / `SV_InstanceID` | `uint` |
| `is_front_face` | `SV_IsFrontFace` | `bool` |

- 成员只能是标量或向量。数组会被拒（这也是 LC 自己的 `AppData` 过不了这个循环的原因）
- `AppData` **不是**强制的：codegen 只读 `vert_args[0].type()` 的 `member_attributes()`，
  全仓库 `AppData` 的命中都在 `dsl/` 或注释里
- `attributes` 向量必须**全长**（未标注成员填空 `Attribute{}`），否则撞 `Type::structure` 的断言
- v2p（varying）结构：**`[position]` 必须是第 0 个成员**且类型 `float4`。codegen 只要求
  "有且仅有一个 position"，但 `TEXCOORD` 索引是无条件的 `memberIdx - 1`，放错位置会让索引回绕
- MRT：pixel 返回结构体时按成员索引发 `SV_TARGET0..N`，**返回结构不需要 attribute**
- 深度导出靠 `propagated_builtin_callables()` 测 `CallOp::RASTER_SET_Z_DEPTH*` 自动接管线

**AST 与 codegen 全程不校验 pixel 参数 0 的类型 == vertex 返回类型** —— 这个不变量本来由
`dsl/` 的模板特化保证，不引 `dsl/` 之后归我们自己断言。错了不会有 LuisaCompute 诊断，
只会产出能编译但错误的 HLSL。

`Function` 是 `const FunctionBuilder*` 的轻量视图，不持有任何东西：
两个 `shared_ptr<const FunctionBuilder>` 必须活过 codegen 全程。

## 5. 删除清单

| 删除 | 规模 |
|---|---|
| `modules/shader` 全部 | 18,431 行（11,748 非测试 + 6,619 测试） |
| `shaderlib/**/*.hlsl(i)` | 1,472 行，18 文件 |
| `forward_pass.shader.json`, `error_pass.shader.json` | 2 份 |
| `tools/shader_gen`, `tools/shader_cook` | 648 行 |
| `tools/generate_imgui_shader.py` + 生成的 `radray_imgui_shader.cpp` | 275 + 707 行；4 个 getter 零调用点，确认死代码 |

删除代价低于看起来的样子：**这套系统零生产调用方**。
`PipelineStateCache::GetOrCreateGraphics` 只有测试调用；`LoadShaderAsset` 生产侧零调用点；
`RenderPipelinePass` 无任何子类；两个 example 因引用不存在的头文件被排除在构建外。
296 个 shader 层用例覆盖的这条管线，从未在测试之外画出一个像素。

## 6. 不动的东西

`modules/render` 的 16,829 行整体不动。它本来就不消费反射 —— D3D12 root signature
（`d3d12_impl.cpp:1663-2076`）与 Vulkan descriptor set layout（`vulkan_impl.cpp:1593-1831`）
全部从 `PipelineLayoutDescriptor` 构造。

作为契约保留、成为 trace 结果落点的类型：`ShaderDescriptor`、
`ShaderParameterSetLayoutEntryDescriptor`、`ShaderParameterSetLayoutDescriptor`、
`PushConstantDescriptor`、`PipelineLayoutDescriptor`、`VertexAttribute`、
`VertexBufferLayout`、`VertexInputState`、`PipelineStateCache`。

Binding group 编号 == D3D12 register space == Vulkan descriptor set index，全链路不重映射。

## 7. 实施风险

1. **`TypeRegistry` 是进程级单例，`Type` 身份即指针身份。** 两份 registry 会让跨边界的指针
   比较静默失败。`luisa-compute-ast` 必须是唯一一份 SHARED
2. **codegen 失败即 `LUISA_ERROR_WITH_LOCATION` 后终止，无错误返回路径。** 与 RadRay
   「避免异常、用 `std::error_code`」的硬规则冲突。需要在边界隔离，或接受进程终止 ——
   **动手前须就此单独裁决**
3. **`luisa::string` 默认不是 `std::string`**（是 `std::basic_string<char, char_traits,
   luisa::allocator<char>>`），与 `radray::string` 是两套类型。`LUISA_COMPUTE_USE_SYSTEM_STL`
   要一次定死
4. **`glslang` 在 `src/ext/CMakeLists.txt` 里无条件 `add_subdirectory` 且无 guard**，
   会随 `luisa-compute-ext` 的 PUBLIC 传染进来，即使只要 ast
5. **两处只读过代码未编译验证**：手工构造参数时的 cbuffer 打包与 register 分配；
   生成的 HLSL 能否端到端过 DXC。这两处是最小验证的首要目标
6. **`luisa-compute-core` 的 logger 是带动态初始化的全局单例**，加载时装 stdout color sink
   并读环境变量。需要用 `default_logger_set_sink` 接到 RadRay 的 logger

## 8. 测试

`test_vertical_slice.cpp` 现在是 D3D12/Vulkan × **Aot/Jit** 四参数。第一期无 AOT，
Aot 那一路随 manifest 一起消失，退化为按后端两参数。它仍是第一期的验收标准：
manifest → GPU → 像素读回改成 **C++ trace → GPU → 像素读回**。

## 9. 文档影响

| 文档 | 处置 |
|---|---|
| ADR-0003, ADR-0005 | 已标记被 ADR-0014/0015 取代 |
| ADR-0002（三层拆分） | 重审：格式层/对象层/资产层的边界在 trace 下是否还成立 |
| ADR-0004（内容寻址） | 收窄：key 的输入从源文件闭包改为 codegen 输出 |
| ADR-0006（`shader_types.h` 收录标准） | 重审：判据"是不是 manifest 数据"随 manifest 消失 |
| ADR-0013（vertex 接口投影） | 重审：其依据是 DXIL 输入签名与 SPIR-V `-fspv-reflect` 语义 |
| `architecture/shader-pipeline.md` | 重写 |
| `architecture/shaderlib.md` | 删除（HLSL 库不再存在） |
| `guide/shader-authoring.md` | 重写 |
| `todo/backend-specialized-shader-lanes.md` | 已标记作废；第 8.4 节的 cook 事务裁决留待第二期采纳 |
| `todo/vertex-interface-projection.md` | 标记为历史 |
