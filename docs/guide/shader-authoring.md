> - 适用: 写/改 HLSL；加 keyword；改 `*.shader.json`；跑 shader_gen / shader_cook
> - 权威: 本文是 shader 作者侧的操作流程。链路内部机制见 `architecture/shader-pipeline.md`；HLSL 库分层见 `architecture/shaderlib.md`
> - 锚点: `shaderlib/`, `tools/shader_gen/shader_gen.cpp`, `tools/shader_cook/shader_cook.cpp`

# shader 编写

## 硬规则速查

| 规则 | 违反后果 |
|---|---|
| include 一律根相对 + 尖括号：`#include <core/math.hlsli>` | DXC 能编，但源码身份扫描器认不出，cook/JIT 静默失效 |
| `.hlsl` = 入口（有 `VSMain`/`PSMain`/`CSMain`）；`.hlsli` = 库头（有 include guard，无入口） | — |
| include guard 名是 `RADRAY_<路径>_HLSLI`（`shadow/cascade.hlsli` → `RADRAY_SHADOW_CASCADE_HLSLI`） | — |
| 不写裸 `register(...)` / `[[vk::binding]]`，走 `core/platform.hlsli` 的 `VK_*` 与 `forward_pipeline/bindings.hlsli` 的 `RADRAY_FORWARD_*` | 编号与 group 分配的分歧编译器查不出 |
| `#pragma radray_keyword_group` 必须在任何 `#if` 之外 | 形成"要先知道 keyword 才能发现 keyword"的循环 |
| 命名：函数与局部变量 `snake_case`；类型与 GPU struct 字段 `PascalCase`；宏 `RADRAY_` 前缀大写 | — |
| 加 helper 前先在 `shaderlib/` 找现成的 | — |

`""` 形式的 include 保留给真正路径相对的场景，当前不存在。看到引号 include 就该复查。

## keyword 必须挣得它的变体维度

加 keyword 前逐条检查这三项，全过才加：

**1. 不能用固定功能状态表达。** `MaterialRenderState` 能表达的（blend、depth write、cull）
必须只住在那里。keyword 绝不能与表达同一个作者决策的固定功能状态**共存或互相暗示**——
那会造出两个真相且没有任何东西保持它们同步（manifest 校验与反射校验各只看到一边）。

**2. 真的改变字节码。** 一个在自己 keyword 关闭时就已经走不到的分支不算。反例：
背面法线翻转由 `SV_IsFrontFace` 守护，在 `CullMode::Back` 下本来就不可达，
再加 keyword 买不到任何东西。

**3. 作用域明确。** 要么是 pipeline 级（整帧一个值，如 `_POINT_SHADOWS` 守护阴影贴图绑定），
要么是 material 级（每 draw，如 `_BASECOLOR_MAP`）。一个 per-draw 决策不能做成 pipeline 级
keyword——pipeline 没有单一值可选。

合法的 keyword 通常守护**描述符绑定**或**无固定功能等价物的代码**：
`_ALPHATEST_ON` 需要 `clip()`；`_POINT_SHADOWS` 需要一个 `TextureCube` 绑定。

### 声明写在哪

写在**提供被守护绑定的那个文件**里，与 `#ifdef` 同处一地。这是唯一不会失同步的位置。

```hlsl
#pragma radray_keyword_group(BaseColorMap, _BASECOLOR_MAP) stages(Pixel)
#pragma radray_keyword_group(AlphaMode, _ALPHATEST_ON, _ALPHABLEND_ON) stages(Pixel)
#pragma radray_keyword_group(Lighting, _LIT, _UNLIT) stages(Vertex, Pixel) required
```

第一个标识符是组名，其后是组内**互斥**的 keyword。`stages(...)` 省略取 `Graphics`；
`required` 表示该组不允许全关。

入口 shader 自动继承它 include 的头文件里的声明。例如 `forward_pass.hlsl` 不重复声明
`_POINT_SHADOWS` / `_DIRECTIONAL_SHADOWS`——那两组由 `forward_pipeline/view.hlsli` 声明，
经 include 继承。

`stages(...)` 填准很重要：它决定字节码去重。一个只影响 pixel 的 keyword 若误标成
`Graphics`，其开与关会算出两个不同的 VS artifact key，白编一份完全相同的字节码。

## 加一个新 pass

1. 写 `.hlsl`，用 `RADRAY_FORWARD_*` 宏声明绑定，用 pragma 声明 keyword 组。
2. 生成 manifest 模板：

   ```powershell
   ./build_debug/_build/Debug/radray_shader_gen.exe `
       --shader-root build_debug/_build/Debug/shaderlib `
       --source forward_pipeline/my_pass.hlsl `
       --stage vertex=VSMain --stage pixel=PSMain `
       -o shaderlib/forward_pipeline/my_pass.shader.json
   ```

   `--shader-root` 指向**构建输出里的 shaderlib 副本**（`radrayshader` 的 POST_BUILD 拷过去的
   那份），因为 DXC 运行库也在那边。`-o` 可以直接写回源码树。

3. 补齐输出里的 `_TODO`。反射给不出的项已列在那里，逐条处理：

   | TODO | 怎么填 |
   |---|---|
   | `Residency` | 默认 `DescriptorTable`。要 root descriptor 就改，仅 CBuffer/Buffer/RWBuffer 且 `Count == 1` 合法 |
   | `VertexFormat` | 生成器按 32 位分量数直译。`UNORM8X4`、`FLOAT16X2` 这类必须手改 |
   | `ArrayStride` / `Offset` | 生成值是紧密打包的自洽起点，真实布局由 mesh 决定 |
   | unbounded 数组的 `Count` | 生成器填 1，改成真实上限（后端拒绝 0） |
   | `ImmutableSampler` | 生成器一律不生成，需要就手加 |
   | `BakeVariants` | 生成器不生成。决定哪些组合值得离线烘 |

   `_TODO` 键不属于 manifest schema，`ParseShaderAssetDesc` 忽略它。补完可以删，留着也能 cook。

4. 删掉 `_TODO` 之后重新生成会**拒绝覆盖**（需 `--force`）。这是刻意的：目标文件里已有你
   手工补的 Residency / BakeVariants，覆盖会静默丢掉。

## 改了 HLSL 之后

| 改动 | 要做什么 |
|---|---|
| 改了函数体、数学 | 什么都不用做。Debug 走 JIT，`Strict` 下改完立刻生效 |
| 加/删了绑定 | 同步改 manifest 的 `BindingGroups`。cook 期的反射核对会抓住漏改 |
| 加/删了 keyword pragma | 同步改 manifest 的 `KeywordGroups`，或重跑 `shader_gen --force` 到临时文件再手工合并 |
| 加了 include | 什么都不用做。源码身份自动含闭包 |
| 改了 entry point 名 | 同步改 manifest 的 `Stages[].EntryPoint` |
| 想改发布的变体集合 | 只改 manifest 的 `BakeVariants`。刻意不放在 HLSL 里，因为改它不该让所有产物 cache 失效 |

漏改 manifest 的绑定会在 cook 期的 `ValidateShaderReflection` 报错——那是最后一道防线，
关掉 `--no-validate-reflection` 就会把错误推迟到运行时建 PSO 失败。

## 给现有 pass 加一个 texture 绑定

端到端走一遍，四步。

**1. HLSL** — 用 `bindings.hlsli` 里的宏声明，**绝不写裸 `register(...)` / `[[vk::binding]]`**：

```hlsl
RADRAY_FORWARD_MATERIAL_TEXTURE2D(gMyTex, 5);
RADRAY_FORWARD_MATERIAL_SAMPLER(gMySampler, 1);
```

槽位号在 `shaderlib/forward_pipeline/bindings.hlsli` 里是唯一定义处。
若这个 texture 由 keyword 控制是否采样，**声明要无条件写在外面**，只把采样包进 `#if`——
描述符 ABI 不随 keyword 变，理由见 `architecture/shaderlib.md`。

**2. manifest** — 同步改 `BindingGroups`，加一条 `ShaderBindingDesc`（`Name` / `Binding` /
`Type` / `Count` / `Stages` / `Residency`）。也可以 `shader_gen --force` 输出到临时文件
再手工合并，别直接覆盖（会丢掉手补的 `Residency` / `BakeVariants`）。

**3. 确认** — 跑一次 cook。`ValidateShaderReflection` 会拿反射结果核对 manifest，
漏改或写错在这里就报出来。

**4. C++** — group 与 binding 索引**从 manifest 读，不硬编码**：

```cpp
const ShaderPassDesc& pass = program->GetDesc();
for (const ShaderBindingGroupDesc& group : pass.BindingGroups) {
    render::ShaderParameterSetDescriptor setDesc{
        .Layout = program->GetPipelineLayout().Get(),
        .GroupIndex = group.Group};          // 来自 manifest
    auto set = device.CreateShaderParameterSet(setDesc).Release();

    for (const ShaderBindingDesc& binding : group.Bindings) {
        if (binding.Name == "gMyTex") {
            // Set(binding, arrayElement, value)。arrayElement 是绑定数组的下标，
            // 非数组绑定（Count == 1）传 0。
            set->Set(binding.Binding, 0, render::ShaderParameterValue{myTextureView});
        }
        // ShaderParameterValue 是 variant：
        //   ShaderBufferBinding / ShaderTexelBufferBinding / TextureView* / Sampler*
    }
    set->FlushWrites();                      // 【必须】Set 只记脏值，这里才真写描述符
}
```

**按 `binding.Name` 匹配、用 `binding.Binding` 当索引**是这里的惯例——名字是你在 HLSL 里
写的那个，索引由 manifest 给。`ShaderPassProgram` / `PipelineLayout` 的其余部分不用动：
layout 由 `PipelineLayoutCache` 按内容去重，加了绑定就自然是一个新条目
（见 `architecture/asset-system.md`）。

完整的可运行例子是 `modules/runtime/tests/test_vertical_slice.cpp`。

## 烘焙

```powershell
cmake --build build_debug --target radray_cook_shaders
```

它对**构建输出目录里的 shaderlib 副本**跑 `radray_shader_cook --discover`，不动仓库源目录。

直接调 CLI：

```powershell
./build_debug/_build/Debug/radray_shader_cook.exe `
    --shader-root build_debug/_build/Debug/shaderlib `
    --manifest forward_pipeline/forward_pass.shader.json `
    --category dxil --category spirv
```

常用选项：

| 选项 | 用途 |
|---|---|
| `--discover` | 烘 shader root 下所有 `*.shader.json` |
| `--clean` | 先删整个产物目录。删过 bake 规则后用——增量从不删东西，旧 blob 会一直占着发布包 |
| `--no-incremental` | 重编所有 blob，不复用 |
| `--no-validate-reflection` | 跳过反射核对。不推荐 |

烘焙**不在 ALL 里**，需要显式触发。理由：开发构建走 JIT，把 cook 挂进每次构建会给每个人
加一笔与其工作无关的开销，且 AOT 是发布/打包关注点。

## 排查

**"改了 shader 但没生效"**
`Strict` 下应立刻生效。检查：跑的是构建输出目录里的那份 shaderlib 吗？源码树里的改动要
重新构建才会被 POST_BUILD 拷过去。

**"AOT 产物不命中，一直在 JIT"**
`Strict` 下源码哈希与 index.json 里记的不符就算未命中。改过 HLSL 后本就该重新 cook。
用 `ShaderBytecode::Source` 字段（`Artifact` / `Jit`）确认实际走了哪条路。

**"发布包里报 shader 缺失"**
`AllowJit == false` 时未烘焙的组合是显式错误。检查 manifest 的 `BakeVariants` 是否覆盖了
运行时实际请求的 keyword 组合。域大而烘焙集小是正常的，但运行时请求必须落在烘焙集内。

**"两个变体编出了完全相同的字节码"**
keyword 组的 `stages(...)` 标错了。见上文。

**"PipelineLayout 建不出来 / 绑定错位"**
manifest 的 `Group` 就是 D3D12 的 `RegisterSpace` 与 Vulkan 的 set index，`Binding` 就是
HLSL register 号，且 b/t/s/u **共用同一编号空间**（Vulkan 的要求）。检查 HLSL 里的
`register(t1, space2)` 与 manifest 里的 `Group: 2, Binding: 1` 是否一致。
