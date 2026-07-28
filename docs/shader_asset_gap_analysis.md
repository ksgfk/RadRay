# ShaderAsset 系统调研与缺口清单

调研日期: 2026-07-27
调研对象: `modules/runtime/include/radray/runtime/shader_manifest.h` 及其实现
调研目的: 评估该系统是否已足够坚固, 可以正式作为引擎资产引入

相关提交背景:
- `46acfd2` 「准备重写 Shader 与 Material 系统」删除了旧的 material / PSO / variant-library /
  mesh-pass 全链路 (约 12000 行), 包括 `forward_pipeline.cpp`(1488)、`mesh_pass_executor.cpp`(803)、
  `shader_variant_library.cpp`(1002)、`pipeline_state_cache.cpp`(301)。
- `888f847` 加入了当前这套 shader_asset 基础设施。
- **中间的装配层尚未重建**, examples 停留在旧 API 且已从构建中摘除。

---

## 1. 现状事实

### 1.1 代码规模

> 本节数字随 8.16 拆库 (`core ← shader ← render ← runtime`) 与 8.19 已更新至
> 2026-07-28。文件已从 `modules/runtime/` 迁至 `modules/shader/`。

| 路径 | 行数 | 性质 |
|---|---|---|
| `modules/shader/include/radray/shader/shader_manifest.h` | 983 | 公开接口 |
| `modules/shader/include/radray/shader/shader_types.h` | 196 | manifest 数据词汇 (10 项, 见 8.17/8.18) |
| `modules/shader/src/shader_manifest.cpp` | 3447 | 全部实现 (未拆分) |
| `modules/shader/src/shader_asset_json.h` | 20 | private, JSON 字段表唯一实现点 |
| `modules/shader/src/shader_reflection_map.h` / `.cpp` | 59 / 140 | private, 反射→RHI 映射, validator 与 generator 共用 |
| `modules/shader/include/radray/shader/shader_asset_template.h` | 249 | manifest 模板生成器 |
| `modules/shader/src/shader_asset_template.cpp` | 1155 | 同上 |
| `modules/shader/tests/test_shader_asset.cpp` | 4460 | |
| `modules/shader/tests/test_shader_asset_template.cpp` | 911 | |
| `modules/runtime/include/radray/runtime/shader_asset.h` / `src/` | 127 / 190 | `ShaderAsset`, 持 GPU 对象 (见 8.9) |
| `modules/runtime/include/radray/runtime/shader_program.h` / `src/` | 235 / 293 | manifest 数据 → RHI 参数打包 |

shader 相关 gtest suite: `ShaderAssetTest`(73) / `ShaderArtifactTest`(60) /
`ShaderResolverTest`(43) / `ShaderBakeSetTest`(26) / `ShaderVariantTest`(22) /
`ShaderKeywordPragmaTest`(21) / `ShaderAssetTemplateTest`(19) / `ShaderAssetLoadTest`(11) /
`ShaderAssetSampleTest`(6) / `ShaderLayoutBindingTest`(6) / `ShaderAssetIdTest`(1)。
全仓 **373** 个用例 / 30 个 suite / 22 个 exe (数字的取得方式见 8.8 与 8.20: 必须
双向集合比较 + 干净构建)。

### 1.2 声明与实现完整性

头文件声明的**每个符号都有实现**, 实现文件里**零 TODO / FIXME / XXX**。

已逐项核对 (行号对应 2026-07-28 的 `modules/shader/src/shader_manifest.cpp`):
`ShaderVariantDomain` 全部方法 (`:1594-1808`)、`GetEffectiveBakeSet`(`:1811`)、
`ExpandShaderBakeSet`(`:1831`)、`ShaderResolveContext` 全部成员 (`:1951-2130`, 见 8.19)、
`ShaderResolver` 全部成员 (`:2131-2412`)、`ValidateShaderReflection` DXIL(`:2547`) 与
SPIRV(`:2637`)、`ComputeShaderSourceIdentity`(`:2739`)、`CookShaderAsset`(`:3304`)、
`CookShaderAssetFile`(`:3418`)。
`BuildPipelineLayoutStorage` / `BuildVertexInputStorage` 已按 8.9 迁至
`modules/runtime/src/shader_program.cpp:42` / `:77`。
18 个 JSON codec 宏声明各有 `Write` + `Read` 定义 (共 36 个)。

### 1.3 已硬化的设计决策

这些是系统坚固性的实际来源:

- **manifest 是唯一 ABI 来源, 反射只做核对**。由此 `BuildPipelineLayoutStorage`
  (`:2445`) 对 target 与 variant 都不变, PipelineLayout 可在编译任何字节码前建好。
  这一条支撑了后面所有分层。
- **内容寻址 blob + stage 投影去重**。`ProjectToStage`(`:1778`) 把无关组一律归
  `kShaderKeywordOff`, 于是「两个变体投影结果相同 ⟺ 共用同一份字节码」成立,
  pixel-only keyword 不污染 VS。测试 `VertexBlobSharedAcrossPixelOnlyVariants`
  (`test_shader_asset.cpp:3535`) 断言 `Deduplicated == 1`。
- **`ShaderVariantKey` 按组编码而非按 keyword 位图**, keyword 总数无上限;
  头文件明确标注「不要放进每帧路径」。
- **Strict / Lenient 过期策略**。`SourceIdentityCache`(`shader_manifest.h:700-704`)
  记录整个 include 闭包的时间戳而非只缓存哈希, 使 Strict 的「改 shader 立刻生效」
  真能兑现。
- **自扫 `#include` 求源码身份** (`ComputeShaderSourceIdentity`), 不求解 `#if`,
  过度失效优于漏失效。

### 1.4 已存在的文档失真

`shader_manifest.h:848-850` 注释称「本轮只烘焙全部 keyword 关闭的默认组合, bake set
属未来工作」——**已过期**。`CookShaderAsset` 在 `shader_manifest.cpp:3325-3343` 实际调用了
`ShaderVariantDomain::Build` + `ExpandShaderBakeSet`, 随后 `category × variant × stage`
三重循环。`shaderlib/forward_pipeline/forward_pass.shader.json:99-120` 已在使用
`BakeVariants`。测试 `ShaderResolverTest.CookBakesEveryDeclaredVariant`
(`test_shader_asset.cpp:3501`) 印证。

---

## 2. 参照系: 现有资产系统

`modules/runtime/include/radray/runtime/asset.h` + `asset_manager.h`。

- `AssetId = Guid`(`asset.h:11`) 持久标识; `AssetHandle = SparseSetHandle`(`:15`) 运行时 slot;
  `AssetTypeId = RuntimeTypeId`(`:20`) 手填固定 Guid, 不依赖 RTTI。
- `class Asset`(`asset.h:32-55`) 两个纯虚: `OnUnload(IRenderResourceRecycler&)`、`GetTypeId()`。
- **契约: 构造即完整** (`asset.h:27-28`) —— Asset 进入 AssetManager 时 CPU 数据 + GPU 资源
  都已就绪, 加载发生在构造之前由协程完成。
- `AssetManager`(`asset_manager.h:213-333`): `Load(AssetLoadRequest{Id, Task, DebugName})`,
  按 id 去重 (`_idIndex`), 引用计数 (`AssetRefControl`), `CollectUnreferenced()`,
  `Unload(id)` 强制回收。**单线程泵模型** (`asset_manager.h:208`: 协程推进、表操作、run 钩子
  全在主/泵线程)。无专门热重载机制。
- 不存在 `AssetRegistry` / `AssetLoader` / `ResourceManager` / `AssetDatabase`。

**三个现有资产类型形状一致, 是明确样板**:

| | ImageAsset | TextureAsset | StaticMesh |
|---|---|---|---|
| 声明 | `image_asset.h:11` | `texture_asset.h:63` | `static_mesh.h:30` |
| 基类 | `: public Asset` | 同 | 同 |
| `RuntimeTypeTrait` | `:61-65` | `:122-126` | `:78-82` |
| 工厂 | `LoadImageAsset`(`:43/48/54`) | `LoadTextureAssetFrom*`(`:105/114`) | `LoadStaticMesh`(`:76`) |
| 返回 | `StreamingAssetRef<T>` | 同 | `AssetLoadTask` |

工厂范式 (`image_asset.cpp:187-196`) + AssetId 生成惯例 (`image_asset.cpp:54-65`
`MakeImageAssetId` 从路径哈希出 Guid)。异步上传参照 `texture_asset.cpp:107-113`
的 `FrameUploadScheduler` / `co_await frame.WaitGpu()`。

---

## 3. 缺口清单 (按严重程度)

### G1. 不是 `Asset`, 且资产粒度未定 (阻塞)

`shader_manifest.h` 中没有任何类型继承 `Asset`, 无 `RuntimeTypeTrait` 特化,
无 `LoadShaderAsset(AssetManager&, ...)` 工厂。

真正的困难不是「加个基类」, 而是 `Asset` 的**构造即完整**契约与 shader 字节码的
**按 variant 惰性**本质冲突 —— 加载时不可能 resolve 所有变体。

自洽的切法 (且是现有设计已支持的):
`ShaderAsset` = `ShaderAssetDesc` + `ShaderPipelineLayoutStorage` + 已创建的
`render::PipelineLayout`。这三者都是 variant / target 无关的, 正好对应 1.3 的第一条
不变量。字节码与 PSO 归另一层惰性缓存。

~~**未决问题**: 资产粒度是 manifest 还是 (asset, pass)?~~ **已裁决 (2026-07-28)**:
`ShaderAsset` = 一份 manifest。见第 8 节。

~~**未决问题**: `ShaderResolver` 归谁?~~ **已裁决 (2026-07-28)**: 归 `ShaderAsset`,
一资产一 resolver。见 8.1。

`PipelineLayout` 的 per-pass 性质 (`shader_manifest.h:248-249`) 落在资产**内部**分层
(`ShaderPassProgram`), 不上升为资产边界。

### G2. `AssetId` 约定未定 (阻塞 G1)

`shader_manifest.h:292` 自承「不包含 AssetId —— 落盘身份约定尚未确定」。
已确认实现里零 AssetId / Guid 相关内容; `ShaderArtifactIndex` 也没有
(只有 `FormatVersion` / `AssetName` / `Sources` / `ToolchainHash` / `Entries`),
产物身份完全靠内容寻址。

依赖 G1 的粒度决定。可照抄 `image_asset.cpp:54-65`。**已定 (2026-07-28)**: 见 8.2。

### G3. 系统完全未接线 (阻塞验证)

- `ShaderResolver` 在非测试代码里**零构造点**。
- `BuildPipelineLayoutStorage` 调用点仅测试 (`test_shader_asset.cpp:345/366/387`)。
- `CreateShader` / `CreatePipelineLayout` / `CreateGraphicsPipelineState` 在
  `modules/runtime` 下**零调用**。RHI 侧实现存在
  (`rhi.h:1228`、`d3d12_impl.cpp:1539`、`vulkan_impl.cpp:1350`)。
- `RenderSystem::OnInitialize`(`render_system.cpp:33-53`) 建了 `RenderPassRegistry`、
  `_shaderIncludeRoot`、`_dxc` 后什么都不做; `GetDxc()` / `GetShaderIncludeRoot()` 无调用方。
- `render_framework/render_pipeline.h` 175 行**不 include shader_manifest.h**,
  文件内无 Shader / Pso / PipelineState 标识符。6 个 `On*` 钩子全空实现
  (`render_pipeline.cpp:149-152` 等), 只有 target 状态转换与 clear 有内容。
- `MaterialRenderState`(`render_pipeline.h:60-67`) 在全仓库**零使用点**,
  只被 `shader_manifest.h:291` 的注释提到。
- `examples/CMakeLists.txt:2-3` 两个 demo 均已注释掉, 依赖的
  `material_asset.h` / `forward_pipeline_shader.h` / `standard_material_factory.h` /
  `static_mesh_component.h` / `gltf_asset.h` 全部不存在。
- ImGui 是唯一真正在用 shader 的地方, 走完全独立的内嵌字节码路
  (`src/imgui/radray_imgui_shader.cpp`, Python 生成的 `constexpr std::byte[]`,
  707 行, 5 个 getter 在 `:687-705`), 且这些 getter 目前也无调用方
  (`imgui_system.cpp` 在 `46acfd2` 被删)。

### G4. 缺 pass / program 级 API

从 keyword 走到字节码需调用方自行完成四步:

```
ShaderVariantDomain::Build(asset, pass, diag)
  -> domain.Resolve(keywords, diag)             // ShaderVariantKey
  -> domain.CollectDefines(key, stage)          // 每 stage 一次, 内含投影
  -> resolver.Resolve(pass, stage, category, defines, diag)  // 每 stage 一次
  -> 自行聚成一组 render::ShaderDescriptor
```

`ShaderResolver` 公开面只有构造 / `Resolve` / `GetSourceIdentity` /
`GetToolchainHash` / `CanJit`(`shader_manifest.h:671-692`), **无任何**
`ShaderVariantKey` / `ShaderVariantDomain` / keyword 参数。
全仓库没有 `ShaderProgram` 之类类型。这条链**只在测试里出现过**
(`test_shader_asset.cpp:3558-3573`)。每个未来调用方都要重走一遍, 是会被抄错的地方。

**形状已定 (2026-07-28)**: `ShaderPassProgram`, 归 `ShaderAsset` 所有, 见 8.3。
G4 与 G1 实现上不可分 —— program 的生命周期出口就是 `ShaderAsset::OnUnload`。
注意 5.6 结论 2 说的"持有 N 个 stage 的 `Shader` 对象"**已被 8.4 推翻**:
`render::Shader` 是瞬态的, program 不持有它。

### G5. 线程模型冲突

`ShaderResolver` 头文件明说非线程安全 (惰性 index 缓存 + 源码身份缓存)。
JIT 编译是阻塞调用 (`shader_manifest.cpp:2161` `_dxc->CompileFile`)。
`AssetManager` 是单线程泵模型。若 shader 加载走 `AssetLoadTask`, JIT 会卡住泵线程。
`texture_asset.cpp` 有 `FrameUploadScheduler` 处理异步上传, shader 侧无等价物。

### G6. 无 cook 驱动入口, 发布路径从未真实运行 —— 已修复 (2026-07-28)

原始问题:
- `CookShaderAsset` / `CookShaderAssetFile` 唯一调用方是单元测试
  (`test_shader_asset.cpp` 约 20 处 + `test_shader_asset_template.cpp:772/815`)。
- `tools/shader_gen` 是 manifest **生成器** 不是 cooker: 读反射 + 扫
  `#pragma radray_keyword_group` → `GenerateShaderAssetTemplate`(`shader_gen.cpp:290`)
  → 写 `*.shader.json`, 输出带 `"_TODO"` 数组列出需人工确认字段。不调用 cook。
- CMake 无 cook 步骤。`modules/runtime/CMakeLists.txt:68-81` 的 POST_BUILD 只是把
  `shaderlib/` 整棵树与 DXC DLL 拷到输出目录供 JIT 使用。
- 仓库内**零 `index.json`**, `shaderlib/` 下无 `forward_pass/` 产物目录。
- 结论: `Lenient` + `AllowJit == false` 这条发布路径只在临时目录的测试里跑过,
  从未在真实构建中运行。

**修复**: 见第 7 节。

### G7. 资产覆盖率 2/4

已有 manifest 两份:
- `shaderlib/forward_pipeline/forward_pass.shader.json` (FormatVersion 1, Name
  `ForwardPrincipled`, 8 个 KeywordGroups 全为 `Stages:[Pixel]`, 1 个 pass `Forward` =
  VSMain + PSMain, 1 条 `Combination` bake 规则 → 2 变体)。
- `shaderlib/forward_pipeline/error_pass.shader.json` (Name `ErrorPass`, 2 个从
  `view.hlsli` 继承的 keyword 组, 无 bake 规则 → 只烘默认变体)。见 5.2。

无 manifest 的入口 shader: `shadow_pass.hlsl`、`imgui/imgui_pass.hlsl`。

### G8. vertex input 反射校验只查存在性

- DXIL 侧 (`shader_manifest.cpp:2565-2596`): 仅 VS 且声明了 VertexInput 时遍历
  `InputParameters`, 跳过系统语义, 按 semantic 基名 (大小写无关) + 有效索引匹配。
- SPIRV 侧 (`:2665-2689`): 遍历 `StageInputs`, 跳过 BuiltIn, **只按 location 数值**匹配。
- **`Format` / `BufferBinding` / `Offset` / `ArrayStride` 一概不核对。**

这几项恰好是 `shader_manifest.h:32-33` 列为「反射推不出、必须作者声明」的内容。
正因反射推不出, 写错了无人拦截, 直到 PSO 创建失败或顶点数据错位。

已校验的部分 (`MatchReflectedBindings`, `:699-776`, 两个重载共用):
stage 是否声明、反射类型可否映射、push constant 位置认领 + 名字 + 是 CBuffer + stage、
绑定存在性 (反射有而 manifest 无 → 失败)、绑定名、Type、**Count** (反射 0 视为
unbounded 以 manifest 为权威)、Stages 包含当前 stage。DXIL 另加 push constant
size 上界 (`:2547-2563`, 声明值先 16 字节对齐再比); SPIRV 另加 range 数量 ≤ 1、
`Offset + Size <= pc.Size`(`:2636-2663`)。

`Residency` 与 `ImmutableSampler` 未被反射校验是**设计使然** (反射原理上给不出),
其合法性在 manifest 解析期检查 (`ValidateBinding :146-170`)。

### G9. `RADRAY_ENABLE_SPIRV_CROSS` 关闭时 SPIRV cook 无法完成 —— 已绕开 (2026-07-28)

`shader_manifest.cpp:3132-3156`: 未启用 spirv-cross 时 SPIRV 反射校验直接失败并报
`"SPIR-V reflection validation requires spirv-cross"`。使 `ValidateReflection == true`
的 SPIRV cook 在该配置下不可用。

**cook 驱动入口的处理**: `radray_shader_cook` 的默认 category 集按本次构建编入的后端
决定 (`DefaultCategories()`) —— 没编 Vulkan 就不烘 SPIRV。而根 `CMakeLists.txt:75-77`
已钉住"Vulkan + JIT ⇒ 必须有 spirv-cross", 故"编了 Vulkan"即"能校验 SPIRV",
这个组合在构建期不会出现。

底层限制本身未变: 显式 `--category spirv` 且构建关掉了 spirv-cross 仍会失败。这是正确
行为 (不能声称校验过一份没校验的 ABI), 但如果将来需要"不校验只烘"的配置, 应当走
`--no-validate-reflection` 而不是放宽这条。

### G10. asset→pass `Source` 继承规则重复实现 —— 已修复 (2026-07-28)

原始问题:
- cook 路径自己做: `shader_manifest.cpp:3295` 与 `:3314`
  (`pass.Source.empty() ? asset.Source : pass.Source`)。
- resolve 路径要求调用方先补好: `:2205-2210` 报
  `"pass source path is empty; inherit ShaderAssetDesc::Source first"`。
- 无公开辅助函数, `ShaderPassDesc` 上也没有。两条路径各自实现同一规则, 将来会分叉。

**这是垂直切片撞上的第一个真实缺口** —— 切片直接把 manifest 里的 pass 交给
`ShaderResolver::Resolve`, 立刻拿到上面那条诊断。若照着 cook 路径再抄一遍
三元表达式, 同一规则就有第三份实现了。

**修复**: 新增两个公开函数 (命名对齐既有的 `GetEffectiveBakeSet`):
- `GetEffectiveSource(asset, pass)` → `std::string_view`, 单一真相。
- `MakeResolvablePass(asset, pass)` → `ShaderPassDesc` 副本, Source 已展开,
  可直接喂 `Resolve`。返回副本而非就地修改, 是为了让 `ShaderAssetDesc` 保持与
  manifest 逐字对应 (pass.Source 空就是空), 往返序列化不失真。

cook 里那两处三元表达式已改为调用 `GetEffectiveSource`。

### G12. keyword 与 MaterialRenderState 的双重归属 —— 已裁决并修复 (2026-07-27)

**裁决**: keyword 三条准则已写入 `AGENTS.md` 的 Shader Conventions
(不可用固定功能表达 / 真实改变 bytecode / 作用域明确为管线级或材质级)。

**已执行的修改**:
- 删除 `_ALPHABLEND_ON`。`standard_material.hlsli` 改为无条件
  `surface.Alpha = saturate(base_color.a)`; 混合关闭时该值本就被 blend state 丢弃,
  keyword 守护它并不改变任何一条路径的结果。顺带合并了恒等的 `Alpha` / `Coverage`
  两个字段 (现只留 `Alpha`)。
- 删除 `_DOUBLESIDED_ON`。两处使用都是死代码或自然失效:
  法线翻转 (`standard_material.hlsli`) 在 `CullMode::Back` 下 `is_front_face` 恒 true;
  early-out (`forward_pass.hlsl`) 在双面时法线已翻向相机故 `wi.z > 0` 本就成立。
  `SV_IsFrontFace` 一直是无条件传入的, 所以翻转改为无条件后单面路径逐位不变。
- 顺带修掉一个独立 bug: `wi.z <= 0` 的 early-out 返回 `float4(0,0,0,Alpha)`,
  吃掉了 emissive。`principled.hlsli:103-106` 的 `front_side` 检查已使这些像素的每个瓣
  返回 0, 故 early-out 是纯性能捷径, 直接删除, 自发光材质掠射角的黑边随之消失。
- keyword 组 9 → 8, bake 变体 3 → 2 (半透明不再是独立变体, 与 opaque 共用字节码)。
- 同步 `forward_pass.shader.json`、`test_shader_asset.cpp`、`test_shader_asset_template.cpp`。
  后者新增一条断言, 防止 `DoubleSided` / `AlphaBlend` 重新变成 keyword。
- 验证: 242/242 shader 相关测试通过, 其中 `ManifestCooksRealForwardShader` 真实调用 DXC
  编译并做反射校验。

**保留为管线级 keyword 的例子**: `_POINT_SHADOWS` / `_DIRECTIONAL_SHADOWS`
(`view.hlsli:33-34`) 守护的是 descriptor 绑定 (`view.hlsli:36-49` 的 `gShadowCube` /
`gShadowArray` / `gShadowSampler`), 固定功能状态表达不了, 是合法的变体维度。

以下为裁决前的原始分析, 保留作为判据来源。

---

### G12 (原始分析). keyword 与 MaterialRenderState 的双重归属

`MaterialRenderState`(`render_pipeline.h:48-67`) 的论证是: blend / zwrite / cull 属 PSO
固定功能段, 不影响 bytecode, 故不该烘进变体, 「同一份 shader + 同一 keyword 表只需一个
ShaderAsset, opaque / transparent / 双面 等差异全部落在材质侧」。

但 `forward_pass` 的实际 keyword 表与此冲突 —— 有两个 keyword **同时**是 bytecode 分支
和固定功能状态:

- `_ALPHABLEND_ON`: 在 `standard_material.hlsli:71` 有 `#ifdef` 真的改 bytecode
  (决定 `SurfaceSample::Alpha` 是否非 1), 同时必然要求 `MaterialRenderState::Blend`
  开启混合。
- `_DOUBLESIDED_ON`: 在 `forward_pass.hlsl:81-87` 有 `#ifndef` 真的改 bytecode
  (单面时 `wi.z <= 0` early-out), 同时必然要求 `MaterialRenderState::Cull = None`。

即同一个作者决策要在两处各写一遍, 且两处一致性无任何机制保证。写错组合
(如开 `_ALPHABLEND_ON` 但没覆盖 Blend) 不会被 manifest 校验拦住, 也不会被反射校验
拦住 —— 反射看不到固定功能状态。

这不是实现 bug, 是**分层边界的一个未裁决问题**。可选方向 (需决策, 尚未讨论):
(a) 接受冗余, 由材质层在构建 PSO 时断言一致性;
(b) 让 manifest 的 keyword 组可选地声明它蕴含的固定功能约束, 材质覆盖时核对;
(c) 把这类 keyword 从材质决策收归 shader 侧, MaterialRenderState 只管纯粹的
    非 bytecode 状态。

必须在设计 material / PSO 层之前裁决, 否则会硬化成两套真相。

### G13. `MaterialRenderState` 的"pass 基线"不存在 (2026-07-28 发现)

`MaterialRenderState`(`render_pipeline.h:60-67`) 的三态语义 (`:55-59`) 说各字段为
`nullopt` / `OverrideBlend=false` 时"沿用 `ShaderPassDesc` 的 pass 基线"。**该基线不存在。**

- 旧 `ShaderPassDesc` (`46acfd2b^` 的 `shader_asset.h:75-78`) 确实有
  `Primitive` / `DepthStencil` / `MultiSample` / `ColorTargets` 四个字段, 注释写着
  "本 pass 的固定渲染状态 (blend / depth / raster 等)", 还有一个
  `AllowMaterialRenderStateOverrides` 开关。
- `46acfd2b`「准备重写 Shader 与 Material 系统」删掉了整个文件。新
  `ShaderAssetDesc` (`shader_manifest.h:300-302`) **刻意**不含这些, 并把责任指回
  `MaterialRenderState`。
- 于是两处注释互相引用, 形成空环: manifest 说"由材质覆盖", 材质说"沿用 manifest 基线"。

`MaterialRenderState` 全仓库零使用点 (只有定义, 没有读写), 所以这个空环从未在运行时暴露。
两份真实 manifest (`error_pass` / `forward_pass`) 也没有任何 `Cull` / `Blend` /
`DepthWrite` 字段, 与 8.1 一致。

更要紧的是覆盖面: `MaterialRenderState` 只能表达 `Cull` / `DepthWrite` / `Blend` 三项,
而 `GraphicsPipelineStateDescriptor` (`rhi.h:1125-1135`) 需要 `Topology` / `FrontFace` /
`PolygonMode` / `DepthCompare` / `DepthTestEnable` / 各 target 的 `Format` /
`MultiSampleState` / `ColorTargetState[]`。这些**目前无人负责**。

**裁决 (A 方案, 2026-07-28)**: 见 8.6。已实施 —— `render_pipeline.h` 与
`shader_manifest.h` 的两处空环注释均已改为指向本节, `PipelineStateCache` 要求调用方给出
完整固定功能状态。基线合成本身仍未实现, 留给 material 层。

### G11. 文档失真 —— 已修复 (2026-07-27)

`shader_manifest.h:848-850` 的过期注释 (见 1.4) 已改为描述真实行为
(烘焙范围由 `GetEffectiveBakeSet` / `ExpandShaderBakeSet` 决定, 按
category × variant × stage 编译并去重)。

---

## 4. 结论与建议顺序

**底层坚固, 但「正式引入 ShaderAsset」目前不是正式化动作, 而是新写一层。**

建议顺序反过来: 不要先写 `class ShaderAsset : public Asset`, 那样是在没有任何
消费方的情况下猜 API 形状。

优先级见第 7 节。

---

## 5. 垂直切片 (G3, 已完成 2026-07-28)

### 5.1 落点决策: headless gtest, 不是 example app

**否决 example app 的理由**:
- `examples/CMakeLists.txt` 全文只有两行被注释的 `add_subdirectory`。两个 demo 共缺 8 个
  不存在的头文件: `gltf_asset.h`、`material_asset.h`、`camera_control_component.h`、
  `static_mesh_component.h`、`standard_material_factory.h`、`static_mesh_scene_proxy.h`、
  `forward_pipeline_shader.h`、`render/common.h`。这些正是 `46acfd2` 删掉的 material 层
  —— 而 material 层的 API 形状是切片本该用来**推导**的东西, 先补再验证等于用猜出来的
  形状验证自己。
- 硬阻塞: `RenderSystem::_pipeline` 全仓库**零赋值点** (只有 `render_system.cpp:27` 的
  `reset`), 无 setter, `OnInitialize` 也不创建。`Render` 在 `_pipeline == nullptr` 时
  (`render_system.cpp:82`) 什么都不画。走 example 要先改 `RenderSystem` 生产代码。
- `Scene → proxy → draw` 链路目前**不可能产出任何 proxy**:
  `PrimitiveComponent::CreateSceneProxy()` 基类返回 nullptr
  (`src/components/primitive_component.cpp:42-44`), `Scene::AddPrimitive` 拿到 nullptr
  直接返回 (`src/render_framework/scene.cpp:19-22`)。要打通需补 6 层
  (component 子类 / proxy 子类 / pipeline 子类 / RenderSystem 注入口 /
  mesh pass executor 原 803 行 / PSO 缓存原 301 行), 每层都在无消费方时猜形状。
  **切片阶段刻意不碰这条链。**

**选 headless gtest 的理由**: 要验证的链路完全不需要窗口; swapchain / present / flight
时序是另一个已在运行的子系统。readback 断言给出 example 给不了的自动回归。
`radray_add_radray_gtest_case` (`cmake/Utility.cmake:256-293`) 已注入
`RADRAY_TEST_ARTIFACTS_DIR` / `RADRAY_TEST_UPDATE_BASELINE`, 这套 golden image 地基
写好但全仓库零调用, 切片是它第一个用户。

**风险**: 仓库内**没有任何测试创建过 render device** (`Device::Create` / `CreateDevice`
在所有 tests / benchmarks / tools 下零命中), 这条路从零搭。device 创建失败必须
`GTEST_SKIP` 兜底, 抄 `test_shader_asset.cpp:3977` 的模式。

### 5.2 选 error_pass.hlsl 作为切片 shader

`shaderlib/forward_pipeline/error_pass.hlsl` 仅 28 行: 顶点只有 `float3 Position : POSITION0`,
`PSMain` 返回洋红常量 `float4(1,0,1,1)` —— 断言"中心像素是洋红"最干净。
对比 `forward_pass` 需填 `gView` 大结构 + 3 个 binding group + 48 字节 stride 顶点,
绝大部分工作与 G3 无关。

**要为它新写 `error_pass.shader.json`**, 顺手补 G7 一格。

**关键事实 (已实测确认, 修正了先前的推测)**:
`error_pass.hlsl:9` include 了 `<forward_pipeline/view.hlsli>`, 而 `view.hlsli:33-34`
声明了 `_POINT_SHADOWS` / `_DIRECTIONAL_SHADOWS` 两个 keyword 组并在 `:36-49` 用它们
守护 shadow 绑定 (t1 `gShadowCube` / t2 `gShadowArray` / s3 `gShadowSampler`)。

- **keyword 组会继承** —— manifest 必须声明这两组。`radray_shader_gen` 从预处理输出
  自动派生, 无需手写。
- **但那三个 shadow 绑定不会出现在反射里, 故不该声明**。原先推测"必须声明 t1/t2/s3"
  是错的。原因: 反射只报告**被使用**的绑定, 而 `error_pass` 的 `PSMain` 返回常量、
  从不采样阴影, 声明后未使用的资源被 DXC 剥掉。已用
  `radray_shader_gen --probe "_POINT_SHADOWS,_DIRECTIONAL_SHADOWS"` 显式探测验证:
  输出里依然只有 `gPerObject` 与 `gView`。声明它们会让 manifest 描述一份不存在的 ABI
  (虽然因 `AcceptsReflectionMissingDeclaredBindings` 的"声明 ⊇ 反射"规则不会导致
  cook 失败, 正因如此这类错误不会被自动发现, 更需要人守住)。
- 同理 **`gView` 的 Stages 只有 `Vertex`** —— `PSMain` 压根不读它。

另一个实测发现: `radray_shader_gen` 拒绝把 keyword 当作无条件 `--define`
(报错 `"an unconditional define would pin that variant dimension"`), 探测 `#ifdef`
后的绑定必须用 `--probe`。

manifest 所需的绑定事实:
- `gPerObject`: group 0, binding 1 (`bindings.hlsli:49-50` `register(b1, space0)`),
  `ObjectConstants { float4x4 ObjectToWorld; }` (`bindings.hlsli:46`), 仅 Vertex 使用。
- `gView`: group 1, binding 0 (`view.hlsli:28`), `ViewConstants` 见 `view.hlsli:13-26`
  (ViewProj + CameraPosition + LightCounts + 光源数组 + CubeShadow + CascadeShadow)。
  error_pass 的 VS 只用 `ViewProj`, 但 cbuffer 大小按完整结构算。
- shadow t1/t2/s3: **不声明** (理由见上)。
- 顶点: 1 个 buffer, `ArrayStride = 12`, 一个 `POSITION` 属性 `FLOAT32X3` offset 0。
- 无 group 2 (材质组) —— 兜底 pass 不能依赖正在失败的那一环。

**已完成**: `shaderlib/forward_pipeline/error_pass.shader.json` +
`ShaderAssetSampleTest.ManifestMatchesErrorPassContract` (契约锁定, 含"不得声明
shadow/材质绑定"的反向断言) +
`ShaderAssetSampleTest.ManifestCooksRealErrorPassShader` (真实 DXC 编译 +
`ValidateReflection`, DXIL 与 SPIRV 各 1 VS + 1 PS)。
`ScopedForwardSampleCookDirectory` 改为接受 manifest 文件名参数, 使两个 cook 用例
各用独立临时目录 (cook 产物落在 manifest 旁边)。

### 5.3 切片实施顺序 (7 阶段, 见 todo)

1. `Device::Create` + `GTEST_SKIP` 兜底, D3D12 / Vulkan 双后端参数化
2. offscreen `CreateTexture` + `CreateTextureView` + `CreateRenderPass` + `CreateFramebuffer`
3. shader 链路: `LoadShaderAssetDesc` → `BuildPipelineLayoutStorage` → `CreatePipelineLayout`
   → `ShaderVariantDomain::Build` + `DefaultVariant` → 每 stage `CollectDefines`
   + `ShaderResolver::Resolve` → `CreateShader` ×2
4. `BuildVertexInputStorage` + `CreateGraphicsPipelineState`
5. VB/IB/CB + `CreateShaderParameterSet` + `Set` + `FlushWrites`
6. `BeginRenderPass` → `SetViewport`/`Scissor` + `Bind*` + `DrawIndexed` → `EndRenderPass`
7. `CopyTextureToBuffer` + `Map` + 断言中心像素洋红

### 5.4 实施所需的 API 事实

**`CreateGraphicsPipelineState`** (`render/rhi.h:1234`), descriptor 在 `rhi.h:1125-1135`。
必填项 (按后端实现核实):
- `PipelineLayout` — `vulkan_impl.cpp:2470` 无判空直接 cast
- `VS` / `PS` 的 `EntryPoint` — `vulkan_impl.cpp:2304-2307` 空则报错
- **`CompatibleRenderPass` — Vulkan 硬性要求** (`vulkan_impl.cpp:2465-2468`
  `"vk graphics pipeline requires an explicit render pass"`)。**故必须先建 RenderPass 再建 PSO**,
  且其 attachment format / sampleCount 必须与 `ColorTargets` / `DepthStencil` 一致。
- `Primitive` / `MultiSample` / `ColorTargetState` / `DepthStencilState` 各有
  `Default()` (`rhi.h:980-989` / `:1070-1075` / `:1110-1115` / `:1048-1060`)

**encoder 获取范例**: `render_pipeline.cpp:215-254` 的 `ClearTarget` 是仓库内唯一一段完整的
"RenderPassRegistry → GetOrCreateRenderPass → GetOrCreateFramebuffer →
`GetCommandBuffer()->BeginRenderPass` → (此处插绘制) → `EndRenderPass`"。切片直接照抄骨架。
注意它 Begin 后立刻 End —— 空 pass 只为触发 `LoadAction::Clear`。

**绘制 API** (`rhi.h:1321-1344`, 全部从未被调用过): `SetViewport(Viewport)` /
`SetScissor(Rect)` (两个类型在 `core/basic_math.h:40` / `:49`) / `BindVertexBuffers` /
`BindIndexBuffer` / `BindGraphicsPipelineState` / `Draw` / `DrawIndexed`。
后端实现完整: D3D12 `d3d12_impl.cpp:3911`/`:3914`, Vulkan `vulkan_impl.cpp:4407`。

**参数集**: `CreateShaderParameterSet` (`rhi.h:1232`) + `Set` / `FlushWrites`
(`rhi.h:1496-1498`) + `encoder->BindShaderParameterSet(groupIndex, set, dynamicOffsets)`
(`rhi.h:1316`)。注意 `gPerObject` / `gView` 在 forward manifest 里是 `RootDescriptor`,
error_pass 的 manifest 可自行决定 (RootDescriptor 需 Count==1 且为 CBuffer/Buffer/RWBuffer)。

**测试注册**: `modules/runtime/tests/CMakeLists.txt` 现有 2 个 target 都用 `radray_add_test`
+ `ENVIRONMENT "RADRAY_PROJECT_DIR=..."` + `RADRAY_PROJECT_DIR_DEFAULT` 编译期兜底
(`:13-16`, 理由见 `:6-12`: 直接跑 exe 时 ctest 的环境变量不存在)。切片若用
`radray_add_radray_gtest_case` 则环境变量由它注入。

### 5.5 切片跑通后应浮现的三个结论

这是做切片的真正目的 —— 让三个悬着的决定从猜测变成推导:
1. **`ShaderResolver` 归属**: 大概率 `RenderSystem` (已持有 `_dxc` 与 `_shaderIncludeRoot`,
   `render_system.h:50-52`)。切片会暴露它是否需要跨资产共享 index 缓存。
2. **program 级 API 形状 (G4)**: 切片里"遍历 stage、各自 CollectDefines、各自 Resolve、
   聚成 ShaderDescriptor 数组"那段代码写出来后, 该抽成什么就明显了。
3. **PSO 缓存 key**: 确认用 `ComputeShaderArtifactKey` 的 `ShaderHash` (POD) 而非
   `ShaderVariantKey` —— `shader_manifest.h:335-336` 明确警告后者别进每帧路径, 切片是第一次
   真正检验这条警告。

### 5.6 切片已完成 (2026-07-28)

`modules/runtime/tests/test_vertical_slice.cpp`,
`VerticalSliceTest.ManifestToPixels` 按后端参数化, **D3D12 与 Vulkan 双双通过**。
链路: `LoadShaderAssetDesc` → `BuildPipelineLayoutStorage` → `CreatePipelineLayout`
→ `ShaderVariantDomain::Build` + `DefaultVariant` → 每 stage `CollectDefines` +
`ShaderResolver::Resolve` (JIT) → `CreateShader` ×2 → `BuildVertexInputStorage` →
`CreateGraphicsPipelineState` → `CreateShaderParameterSet` + `Set` + `FlushWrites`
→ barrier + `BeginRenderPass` + `DrawIndexed` + `EndRenderPass` → `Submit` + `Wait`
→ `CopyTextureToBuffer` + `Map` → 断言中心像素洋红、角落黑。

断言角落为 clear 黑是刻意的: 只断言中心洋红的话, 一张被整体填成洋红的图也会过。

**实测的后端差异 (写切片时踩到的)**:
- **绕序**: `PrimitiveState::Default()` 是 `FrontFace::CW` + `CullMode::Back`。同一份
  NDC 顶点在 D3D12 与 Vulkan 下绕序**相反** (NDC y 轴方向不同), 实测 Vulkan 通过而
  D3D12 整个三角形被剔除。切片改用 `CullMode::None` 绕开 —— 绕序是"CPU 侧投影矩阵与
  网格数据的契约", 属材质/网格层职责, 不是 shader 链路的一部分。
  **这是未来 material 层必须显式裁决的一件事**, 目前无人负责。
- **队列**: Vulkan 的 `GetCommandQueue` 不会惰性创建, 必须在
  `VulkanDeviceDescriptor::Queues` 预声明; D3D12 按需创建。这是设备创建唯一需要
  分支的地方。
- **`CompatibleRenderPass`**: Vulkan 建 PSO 时硬性要求, 故 RenderPass 必须先于 PSO 创建。
- Vulkan instance 是进程级全局 (`InstanceVulkan::InitEnv` / `ShutdownEnv`), 必须晚于
  device 释放。

**顺带确认的实现细节**: Upload 堆 buffer 可直接当 VB/IB/CB 用, 无需 staging copy,
两个后端都不需要为此加 barrier。

**结论 1 (ShaderResolver 归属)**: 切片里 resolver 是局部变量, 因为只有一份 manifest。
真实系统里谁持有它, 取决于 G1 (`ShaderAsset` 粒度), 本切片不足以单独裁决。

先前在本文档里写过"应把 `ManifestPath` 从构造参数移到 `Resolve` 入参", **该结论已撤销**,
理由不成立:

- 当时的论据是"多个 manifest 共享 include 闭包 (如 `view.hlsli`), 跨资产共享 resolver
  才能让 `SourceIdentityCache` 生效"。但复核代码后确认: 该缓存按 **entry 源文件路径**
  做 key (`shader_manifest.cpp:2024` 比对 `SourcePath`, `:2040` 写入), 存的是整份 include
  闭包算出的单个哈希。两份 manifest 的 entry 不同 (`forward_pass.hlsl` vs
  `error_pass.hlsl`), 各占一条独立条目, 共享 resolver 不会让它们复用彼此的缓存。
  被共享的头文件只体现在各自的 `Stamps` 列表里 (用于时间戳复核), 不是缓存 key。
- 且"一个 resolver 绑一份 manifest"与现有设计是**一致**的: `index.json` 每份 manifest
  一个, artifact 目录由 manifest 路径推导 (`GetShaderArtifactDirectory(_config.ManifestPath)`,
  `shader_manifest.cpp:2067` 与 `:2098`), `_index` 也只缓存那一份。把 `ManifestPath` 挪到
  `Resolve` 入参反而要把 `_index` 改成按路径索引的 map, 凭空多一层缓存与失效逻辑。

更自然的方向是**每个 `ShaderAsset` 各持一个 resolver**, `RenderSystem` 只提供
`ShaderRoot` + `dxc` + staleness 策略 (它已持有前两者, `render_system.h:50-52`)。
但这依赖 G1 先定, 故本项归入 G1, 不单独立项。

**结论 2 (program 级 API 形状)**: 切片中"遍历 stage → CollectDefines → Resolve →
CreateShader → 攒 ShaderEntry"这段约 25 行, 且必须让 `unique_ptr<Shader>` 活到建完
PSO。这是明确的 program 级抽象需求: 一个持有 N 个 stage 的 Shader 对象 + 对应
`ShaderEntry`, 生命周期绑在一起。

**结论 3 (PSO 缓存 key)**: 切片没有缓存, 故尚未真正检验。留待 material 层。

---

## 6. cook 驱动入口 (G6, 已完成 2026-07-28)

### 6.1 落点: 独立 CLI 工具 `tools/shader_cook`

`tools/shader_cook/shader_cook.cpp` → 目标 `radray_shader_cook`, 只在
`RADRAY_ENABLE_SHADER_JIT` 下构建 (守卫在 `tools/CMakeLists.txt`, 与 `shader_gen` 同一条)。

**为何不并入 `radray_shader_gen`**: 两者方向相反。`shader_gen` 从反射**生成** manifest
模板, 输出还带 `"_TODO"` 待人工收敛; cook **消费**已收敛的 manifest。共用一个 exe 会让
"这个工具做什么"取决于参数。

**为何是独立 exe 而不是塞进某个既有程序**: cook 要链 `radrayruntime` 与 DXC。塞进某个 app
的启动路径会让"产出发布包"依赖"跑起某个 app"。

**刻意不提供 `--output`**: 产物目录由 `GetShaderArtifactDirectory(manifestPath)` 推导,
而运行时 `ShaderResolver` 用的是同一个函数从同一个 manifest 路径反推。一旦可以自定义
输出位置, 布局约定就有了第二个真相, 且运行时那一侧看不到构建时传的参数。要换位置就换
manifest 的位置。

参数面:

| 选项 | 作用 |
|---|---|
| `--shader-root <dir>` | 必需。include 根, 也是相对 manifest 路径的基准 |
| `--manifest <path>`… | 显式指定, 可重复; 相对路径按 shader root 解析 |
| `--discover` | 递归收集 root 下全部 `*.shader.json`, 结果排序使输出可复现 |
| `--category <dxil\|spirv>`… | 默认按本次构建编入的后端推导 (见 G9) |
| `--no-validate-reflection` | 关掉反射核对。默认开 |
| `--no-incremental` | 关掉"已存在且自验通过的 blob 跳过编译" |
| `--clean` | 先删整个产物目录再烘 |
| `--quiet` | 只报错误 |

两条刻意的行为决定:
- **一份失败继续烘剩下的**, 退出码仍反映有失败。一次构建把所有 manifest 的问题报全,
  比每次只暴露第一个省往返。
- **`--discover` 一个都没找到是错误**, 不是静默成功。那几乎总是 `--shader-root` 指错或
  部署步骤没跑, 而静默返回 0 会让构建"成功"却不产出任何 `index.json` —— 那正是这个工具
  存在的唯一目的。

`--clean` 存在的理由: 增量只跳过已存在的 blob, **从不删除**任何东西。删掉一条 bake 规则后
上一轮的 blob 会留在目录里, 它不在新 `index.json` 内, 运行时查不到, 不影响正确性, 但会
一直占着发布包。

### 6.2 CMake 集成: 显式目标, **不进构建**

`tools/shader_cook/CMakeLists.txt` 定义 `radray_cook_shaders` —— 一个不挂在 ALL 上的
`add_custom_target`:

```
cmake --build build_debug --target radray_cook_shaders
```

**为何不做成构建期步骤** (POST_BUILD 或进 ALL):
- 开发构建靠 JIT。产物存在只会多一层失效面 —— "改了 shader 却读到旧 blob" 这类问题在
  Strict 下靠时间戳复核挡住, 但那是白付的复杂度, 因为开发根本不需要产物。
- cook 要用 DXC 编译每个 (category × variant × stage)。挂进每次构建等于给所有人加一笔
  与其当前工作无关的开销 (实测 forward_pass 全量约 0.5s, 会随变体数增长)。
- AOT 是**发布/打包**的需求。该由打包流程按需触发, 而不是由"编了一次代码"触发。

烘的是输出目录里那份 shaderlib 而非源码树: 运行时 resolver 拿到的 manifest 在
`<exe>/shaderlib` 下 (`render_system.cpp:45` 的 `_shaderIncludeRoot`)。烘源码树等于把产物
放到运行时不会去看的地方, 还会污染 git 工作区。定序靠
`add_dependencies(radray_shader_cook radrayruntime)` —— `radrayruntime` 的 POST_BUILD 先把
`shaderlib/` 部署到输出目录。

实测输出 (Debug, D3D12 + Vulkan 都编入):

```
radray_shader_cook: cooking 2 manifest(s) for [DXIL, SPIRV] from '.../_build/Debug/shaderlib'
radray_shader_cook: .../error_pass.shader.json   -> 4 entries (compiled 4, reused 0, deduplicated 0)
radray_shader_cook: .../forward_pass.shader.json -> 6 entries (compiled 6, reused 0, deduplicated 2)
```

再跑一次全部转为 `reused`。产物落在
`build_debug/_build/Debug/shaderlib/forward_pipeline/{error_pass,forward_pass}/`。

**测试不依赖这个目标**: 两个新用例与切片的 AOT 分支都把 manifest 拷进各自的临时目录后
自行调 `CookShaderAssetFile`, 故删掉输出目录里的产物后 ctest 依然全过 (已验证)。这是刻意的
—— 测试不该依赖"某人先跑过某个 target"这种前置状态。

### 6.3 测试

**切片加 AOT 参数** (`test_vertical_slice.cpp`): 参数从 `RenderBackend` 改为
`SliceParams{Backend, Mode}`, `Mode ∈ {Jit, Aot}`, 共 4 个用例, **全部通过**。

AOT 分支先把 manifest 拷进临时目录 (`ScopedCookedManifest`, 产物落在副本旁边, 不碰源码
树), 调 `CookShaderAssetFile`, 然后用 `Lenient` + `AllowJit = false` + **`dxc == nullptr`**
解析。传 nullptr 而不是 `dxc.get()` 是关键: 给了指针再关 `AllowJit` 只测到"我们没去用它",
传 nullptr 才测到"发布包里 DXC 根本不存在时也能起来"。每个 stage 都断言
`bytecode->Source == Artifact` —— 少了这条, AOT 用例若悄悄退回 JIT 照样画出洋红,
整个参数化就白跑了。

**两个新的设备无关用例** (`test_shader_asset.cpp`, `ShaderAssetSampleTest`):
- `CookedErrorPassResolvesWithoutJit`: 真实 manifest 烘出的产物在发布包配置下逐 stage
  逐 category 命中, 且 `cook.Index.Find(bytecode->Key)` 必须命中 —— 这是"运行时纯函数
  重算 key"这条设计唯一的真实检验点。附带两条反向断言: `ShaderRoot` 指向不存在的目录时
  `Lenient` 仍命中 (发布包不部署源码), 同配置换 `Strict` 必须失败 (算不出身份且无 JIT
  时不能猜)。
- `UnbakedErrorPassVariantFailsWithoutJit`: 请求合法但未预编的组合 (`_POINT_SHADOWS`)
  在 `AllowJit = false` 下必须显式失败, 而同一份产物在开发配置下由 JIT 兜底 —— 后半条
  确保前半条的失败来自"没有 JIT", 不是"这个变体非法"。查 PS 而非 VS 是必要的:
  该 keyword 组只作用于 Pixel, VS 的投影会把它归零从而命中默认变体的 blob。

全量: `test_shader_asset` 227/227, `test_shader_asset_template` 40/40,
`test_vertical_slice` 4/4。CLI 本身另手工验过 `--discover` / `--clean` / `--category` /
错误路径的退出码。

### 6.4 写这一层时撞到的两件事

- **`INSTANTIATE_TEST_SUITE_P` 的参数列表里既不能写裸逗号也不能塞 `#if`**。
  `SliceParams{a, b}` 的逗号被预处理器当成宏参数分隔符; 而把 `#if defined(...)` 放进
  `testing::Values(...)` 在 MSVC 的 `/Zc:preprocessor` 下直接是语法错误。解法是把参数集
  在宏外面用一个函数算好, 再 `testing::ValuesIn(...)`。
- **gtest 的 `<<` 不认 `format_as`**。`EXPECT_*() << category` 编不过 (`ShaderBlobCategory`
  只有 `format_as`, 没有 `operator<<`), 要先 `fmt::format` 成字符串。

---

## 7. 后续优先级

已完成: G3 垂直切片 (第 5 节)、G6 (第 6 节)、G9 (构建期已绕开)、G10、G11、G12。
G1 / G2 / G4 的设计已裁决 (第 8 节), 实施中。

接下来:

1. ~~**G1 + G2 + G4 实施**~~ **已完成 (2026-07-28)**: `MakeShaderAssetId` +
   `ShaderAsset` + `ShaderPassProgram` + `LoadShaderAsset`, 头文件按 8.4b 分三层,
   新增 `test_shader_program.cpp` 10 个用例。**切片尚未改用 `ShaderAsset`** —— 它仍自己
   临时建 `Shader` 与 PSO, 留待第 2 步一并处理。
2. ~~**PSO 库**~~ **已完成 (2026-07-28)**: `pipeline_state_cache.h/.cpp` 的
   `PipelineStateCache` + `GraphicsPipelineStateKey`, 接进 `RenderSystem`, G13 按 8.6
   裁决。切片已切到 `LoadShaderAsset` + `PipelineStateCache` (四个用例逐字通过),
   5.6 结论 3 的 PSO 缓存 key 在真实 device 上检验完毕。新增
   `test_pipeline_state_cache.cpp` 6 个用例, 全量 362/362 通过。
3. ~~**重构为 `core ← shader ← render ← runtime`**~~ **已完成 (2026-07-28)**:
   按 **8.15** 执行完毕 (8.13 的三库版已被其取代), 实施结果见 **8.16**。此步吸收并
   **关闭了 G14 与 G15**, 以及原计划的"抽出 `radrayshader`" (8.4c/8.7/8.9)。
   验收: 全量构建通过, ctest 360/360 且 exe 自报 ↔ ctest 双向集合完全一致,
   `radray_shader_cook.exe` 零后端 obj、无 `d3d12.dll` 导入、体积 33 MB → 9.84 MB。
4. **绕序契约归属**。切片用 `CullMode::None` 绕开了 D3D12/Vulkan 绕序相反的问题
   (见 5.6), material/mesh 层必须显式裁决 —— 目前无人负责。
5. G8 vertex input 校验补强、G7 补齐其余 manifest (`shadow_pass`、`imgui_pass`)。
   G5 线程模型已由 8.3 绕开 (加载路径不碰 DXC), 待 material 层触发大批量 JIT 时再看。

**PSO 库之后新浮现的**: 固定功能状态的"基线 + 覆盖"合成仍无归属 (G13)。本轮把 PSO 层定为
执行层, 要求调用方给全状态, 于是缺口从"两处注释互相指"变成"一处明确未实现" —— 更好, 但
仍是缺口。`MaterialRenderState` 至今零使用点, 且只覆盖 `Cull` / `DepthWrite` / `Blend`
三项。这条要等 material 层。

~~**G14**~~ **已关闭 (2026-07-28)**: `rhi.cpp` 曾把无状态纯函数 (格式尺寸查表等) 与
设备工厂放在同一个 .cpp, 而 **obj 是链接粒度** —— 只想调
`GetVertexFormatSizeInBytes` 的工具会被迫链入整个图形后端 (Windows 上表现为
`radray_shader_cook.exe` 硬依赖 `d3d12.dll`, macOS 上会以 Metal + Cocoa 复发)。
最终修法不是原计划的"拆出 `rhi_format.cpp`", 而是随 8.15 第 1 步把这些纯函数连同
19 个类型一起迁入 `radrayshader/src/shader_types.cpp` —— 同样让它们脱离含设备工厂的
obj, 且顺带完成了库边界。

~~**G15**~~ **已关闭 (2026-07-28)**: `radrayrender` 内部"编译器 / 设备后端"两半缺少
库边界 (G14 那条链的根因), 现已由 8.15 的 `radrayshader` 划出。最终布局不是本条原先
设想的 `rhi_types` + `shadercompiler` + `radrayrender` 三个平级库, 而是用户裁决的
四层直链 `radraycore ← radrayshader ← radrayrender ← radrayruntime` —— 改动更小,
且依赖方向更贴合实测事实 (device 侧重度使用那批类型, 说明它们是 render 的下层)。
原顾虑"需要跨平台构建才能验证"仍然成立: Windows 侧已实测闭合, macOS/Linux 的收益
只能靠推理, 见 8.16 的遗留风险。

---

## 8. G1 / G2 / G4 裁决 (2026-07-28)

### 8.1 粒度: `ShaderAsset` = 一份 manifest

G1 原先把"`PipelineLayout` 是 per-pass"当作粒度应为 (asset, pass) 的论据。该论据只说明
**内部需要分层**, 不说明**资产边界**该切在哪。边界其实已被产物布局定死:

- `index.json` 每份 manifest 一个;
- artifact 目录由 manifest 路径推导 (`GetShaderArtifactDirectory`);
- `ShaderArtifactIndex::Entries` 混装该 manifest 下所有 pass 的所有 stage,
  `PassName` 只是条目里的一个字段, 不是分文件依据。

若粒度取 (asset, pass), 则同一 manifest 的两个 `ShaderAsset` 会共享一份 `index.json`
和一份 resolver 缓存, 立刻退回 5.6 撤销过的那个问题 (谁持有 resolver), 而 5.6 的结论
"每个 `ShaderAsset` 各持一个 resolver"**仅在 asset = manifest 时成立**。

【所以】asset = manifest, pass 落为资产内部的 `ShaderPassProgram`。resolver 归
`ShaderAsset`, 一资产一份; `RenderSystem` 只提供 `ShaderRoot` + `dxc` + staleness 策略
(前两者它已持有, `render_system.h:50-52`)。

副作用一则: manifest 在源码树与输出目录 (`<exe>/shaderlib`) 各有一份, 绝对路径不同 →
AssetId 不同。这是**正确的** (确实是两份文件, 产物目录也各自独立), 但意味着 AssetId 是
"这份文件"而非"逻辑资产名"。按逻辑名寻址 (材质里写 `"ForwardPrincipled"`) 需要另一层
名字→路径映射, 属 material 层, G1 不做。

### 8.2 AssetId (G2)

照 `image_asset.cpp:45-66`: FNV-1a 双次哈希 (第二次加 `:salt`) 填满 16 字节, 再打
UUID v4 的版本 / 变体位。key 为 `fmt::format("shader:{}", absolute(path).generic_string())`。
前缀是命名空间隔离 —— 同一路径在不同资产类型下必须得到不同 id。

### 8.3 分层与职责

| 对象 | 归属 | 生命周期 |
|---|---|---|
| `ShaderAssetDesc` / `ShaderResolver` (字节码缓存) | `ShaderAsset` (= 一份 manifest) | 资产存活期 |
| `PipelineLayout` + vertex input storage (per pass) | `ShaderAsset` 内的 `ShaderPassProgram` | 资产存活期, `OnUnload` 交 recycler |
| `render::Shader` | **无归属** | 单次 `CreatePipelineState` 调用内的局部量 |
| PSO + `StreamingAssetRef<ShaderAsset>` | `RenderSystem` 的 PSO 库 | device 存活期 / 显式清理 |

`Asset` 的**构造即完整**契约与"字节码按 variant 惰性"的冲突 (G1 原文 `:114-115`) 解法:
加载期只做 variant / target **无关**的部分 —— `LoadShaderAssetDesc` →
`BuildPipelineLayoutStorage` → `CreatePipelineLayout` → `BuildVertexInputStorage`。
"完整"指 ABI 与 layout 就绪, 不指所有变体已编译。字节码归 `ResolveVariant` 惰性。

【顺带解掉 G5 的阻塞】加载协程里只有同步文件 IO 与同步 GPU 调用, 两者都短, **不碰 DXC**,
所以 `AssetManager` 的单线程泵不会被 JIT 卡住。JIT 发生在后续 `ResolveVariant`, 那是
调用方主动触发的, 届时阻塞是它自己的选择。G1 因此不需要先解 G5。

### 8.4 `render::Shader` 是瞬态的 (推翻 5.6 结论 2)

复核两个后端, `Shader` 只在 PSO 创建时被消费, PSO 建成后无任何回指:

- **D3D12**: `Dxil` 就是一个 byte vector, `ToByteCode()` (`d3d12_impl.cpp:4482`) 返回指向它的
  `D3D12_SHADER_BYTECODE`, `CreateGraphicsPipelineState` 内部把字节码拷进 PSO
  (`d3d12_impl.cpp:2723-2724`)。`GraphicsPsoD3D12` 成员只有 `_device` / `_layout` /
  `_pso` / `_vertexStrides` / `_topo` (`d3d12_impl.h:893-897`) —— **无 `Shader` 引用**。
- **Vulkan**: `ShaderModuleVulkan` 持 `VkShaderModule`, 只作为 `stageInfo.module` 喂给
  `vkCreateGraphicsPipelines` (`vulkan_impl.cpp:2492`)。`GraphicsPipelineVulkan` 成员是
  `_device` / `_layout` / `_pipeline` (`vulkan_impl.h:1015-1017`)。Vulkan 规范也明确允许
  pipeline 建成后立即销毁 shader module。
- `ShaderEntry::EntryPoint` 那个 `string_view` 同理只需活到调用返回:
  `entryPointsOwned` 是 `CreateGraphicsPipelineState` 里的局部 `vector<string>`。

【所以】`Shader` 不是资源, 是参数。它的生命周期只需覆盖一次 `CreatePipelineState`,
应在建 PSO 的函数里当局部量创建, 出作用域即销毁。

**不做 `ShaderHash → unique_ptr<Shader>` 常驻缓存**的理由: 代价是永久驻留全部
`VkShaderModule` / `ID3D12` 侧字节码副本, 收益只有"PSO cache miss 时省一次
`vkCreateShaderModule`"。而字节码由 `ShaderPassProgram` 缓存 (见下), 重建 `Shader`
只是从内存里的字节码再走一次 `CreateShader`, 不触发 JIT、不读盘。收益近零, 占用实打实。

【注意 `ShaderResolver` 本身不缓存字节码】: `Resolve` 每次调用都会重新读 blob 或重新
JIT (`shader_manifest.cpp:2206` 起, 缓存的只有 `_index` 与 `_sourceIdentities`)。所以
字节码缓存**必须**落在 `ShaderPassProgram` 这一层, 否则每次 PSO miss 都要重新读盘 /
重编。这是 program 层存在的实质理由之一, 不只是"少写 25 行样板"。

真正需要常驻的是 **PSO 库**: PSO 的 key 比 `ShaderHash` 宽得多 (还含 `MaterialRenderState`、
vertex layout、RT 格式), 同一份字节码会喂给多个 PSO, 去重必须发生在 PSO 层, `Shader`
只是该层内部的中间产物。

因此 `ShaderPassProgram` 的缓存形状是两级, 但第二级从 `Shader` 换成了字节码:

- `ShaderHash → ShaderBytecode` (拥有字节码, 按 artifact key 去重);
- `(ShaderVariantKey, category) → 各 stage 的 ShaderHash`。

两级是必要的: `ProjectToStage` 保证"两个变体投影相同 ⇔ 该 stage 共用同一份字节码"
(`shader_manifest.h:389`), 实测 `forward_pass` 的 `Deduplicated == 1`。若只按变体缓存,
共用的 stage 会存多份副本。

### 8.4b 头文件分层 (2026-07-28)

`shader_manifest.h` 里没有 `ShaderAsset` 是不可接受的: 仓库惯例是 `<x>_asset.h` 装
`XAsset` (`image_asset.h` → `ImageAsset`、`texture_asset.h` → `TextureAsset`、
`static_mesh.h` → `StaticMesh`), 一个叫 `shader_asset.h` 却不含 `ShaderAsset` 的文件
会骗人。故按三层重排:

| 文件 | 内容 | 不得依赖 |
|---|---|---|
| `shader_manifest.h` (原 `shader_asset.h`) | manifest desc + 变体域 + 产物索引 + `ShaderResolver` + cook + layout 构建 | `asset.h` / `asset_manager.h` |
| `shader_program.h` | `ShaderPassProgram` / `ShaderProgramVariant` | 同上 |
| `shader_asset.h` (新) | `ShaderAsset` + `MakeShaderAssetId` + `LoadShaderAsset` | — |

实现文件同步改名 (`shader_asset.cpp` → `shader_manifest.cpp`,
`src/shader_asset_json.h` → `src/shader_manifest_json.h`), 保持头/实现同名。

【格式层不得含 Asset 的理由】: `tools/shader_cook` 只需要格式层, 让它传递性依赖
`asset_manager.h` 就等于为一个 CLI 吃下 stdexec 的编译开销 (`asset.h` 本身很轻,
只含 `sparse_set.h` + `runtime_type.h`; 重的是 `asset_manager.h` → `coroutine.h`)。
这条分界照 `image_data.h` (数据格式) 与 `image_asset.h` (Asset) 的既有先例。

【program 层也停在 Asset 之下】: program 的形状与资产系统无关, material 层若只想拿
一个 pass 的 layout + 字节码, 不该被迫拖进 `AssetManager`。

### 8.4c 已裁决: 抽出 `radrayshader` 库, 但排在 PSO 库之后 (2026-07-28)

8.4b 的三层分界靠注释与 review 维持。**库级分界才是编译器强制的** —— 这是"彻底"与
"约定"的差别, 故最终形态应把格式层与对象层抽成独立库:

| 库 | 内容 |
|---|---|
| `radrayshader` (新) | `shader_manifest.*`、~~`shader_program.*`~~ (**已修正, 见 8.9: 留在 runtime**)、`shader_asset_template.*`、`shader_reflection_map.*`、`shader_manifest_json.h` |
| `radrayruntime` | `shader_asset.*` (`ShaderAsset` + `LoadShaderAsset`)、`shader_program.*`、PSO 库 |

**依据一, 依赖方向天然成立**: `shader_manifest.cpp` 的 include 只有 core
(`json.h` / `file.h` / `binary_io.h` / `basic_math.h` / `logger.h` / `enum_flags.h`)
与 render (`dxc.h` / `spvc.h` / `hlsl.h` / `spirv.h`), **零个 `runtime/` 头**。插在
render 与 runtime 之间不产生环: `core ← render ← shader ← runtime`。

**依据二, 体量已经不成比例**: shader 相关 .cpp 共 5122 行, 占 `radrayruntime` 全部
10084 行的 51%。单个 `shader_manifest.cpp` (3433 行) 比 `application.cpp` (926) +
`gpu_resource.cpp` (960) + `gpu_system.cpp` (689) 三者之和还多。

**依据三, 现有消费者被迫过度链接**: `tools/shader_cook`、`tools/shader_gen`、
`test_shader_asset` (227 用例)、`test_shader_asset_template` (40 用例) 都只需要格式层,
却各自链接整个 `radrayruntime` —— 连带 imgui、freetype、cgltf、window、AssetManager。
一个 codegen CLI 没有理由依赖 imgui。

**资产层必须留在 runtime**: `ShaderAsset` 依赖 `Asset` / `AssetManager` /
`IRenderResourceRecycler`, 挪进 shader 库会形成循环。分界线正是 8.4b 那条, 只是从
文件级升到库级 —— `shader_program.h` 当初刻意不依赖 Asset, 就是为这一步留的口子。

**【为何排在 PSO 库之后】**: 抽库要再动一遍全部 include 路径 (应改为
`radray/shader/...`, 否则库名与路径对不上), 与 PSO 库的改动落在同一批文件上。先把
PSO 库做完, `ShaderPassProgram` 的对外形状与 8.5 的钉住机制都稳定后, 抽库就是纯搬运
\+ CMake, 风险最低。

抽库时不可漏的三件事:
1. `RADRAY_ENABLE_SHADER_JIT` 与 `RADRAY_DXC_VERSION` 两个 compile definition 要跟着
   走 (含那个 `FATAL_ERROR` 检查) —— 漏了会让 toolchain hash 静默变化, AOT 产物全部失效。
2. shaderlib 部署的 POST_BUILD 要跟着走, 且 `tools/shader_cook/CMakeLists.txt` 里
   `add_dependencies` 那段注释明确依赖"DXC 已被放进同一输出目录", 需重新确认。
3. 测试跟着挪到 `modules/shader/tests/`; `test_shader_program.cpp` 里涉及 `ShaderAsset`
   的用例留在 runtime 侧。

### 8.5 新浮现: PSO 持 `PipelineLayout` 裸指针会悬垂

两个后端的 PSO 都存了 `_layout` 裸指针 (`d3d12_impl.h:894`、`vulkan_impl.h:1016`), 而
`PipelineLayout` 按 pass 归 `ShaderAsset` 所有。于是 `ShaderAsset` 卸载时, 若 PSO 库里
还有引用其 layout 的 PSO, 即悬垂。

解法: PSO 库条目持一个 `StreamingAssetRef<ShaderAsset>`。引用计数机制已存在, PSO 存活
即钉住 asset。比在 `RenderSystem` 里做失效通知简单, 也避免了 asset → RenderSystem 的
反向依赖。

**修正 (2026-07-28)**: 上一段原写"`OnUnload` 不可能在 PSO 之前跑", 这只对**一条**回收
路径成立。`asset_manager.h:212` 说明资产回收有两条路径, 而 `Unload(id)`
(`asset_manager.h:272-273`) 明确是"确需强制清空"的场景, **不看引用计数**。所以
`StreamingAssetRef` 只挡住 `CollectUnreferenced`, 挡不住显式 `Unload`。

因此 PSO 库仍需提供 `RemovePipelineStatesUsing(const ShaderAsset*)`, 由显式 `Unload`
的调用方负责先调它。这一层的责任划分本轮不动 (`Unload` 目前无调用方), 记为已知缺口。

**本轮 (G1) 不做 PSO 库** —— 只做到 `LoadShaderAsset` 工厂。切片仍自己临时建 `Shader`
与 PSO (它现在就是这么干的), 改动面更小。PSO 库单列一步, 形状参照 `RenderPassRegistry`
(`gpu_resource.h:306`)。

### 8.6 PSO 库 (G13 裁决 + key 组成, 2026-07-28)

**G13 裁决: A 方案 —— PSO 层收完整固定功能状态, 不替 material 层定基线。**

`GraphicsPipelineStateKey` 与 `GraphicsPipelineStateDescriptor` (`rhi.h:1125-1135`) 的
固定功能段一一对应, 调用方必须把每一项填满。`MaterialRenderState` 继续保持零使用,
"基线 + 覆盖"的合成留给未来的 material 层。

【为什么不在 PSO 层顺手补一个基线】G12 的教训正是"同一个作者决策在两处各写一遍, 且
一致性无机制保证"。若 PSO 层自己定义一套 pass 默认状态, 它就成了第二套真相 —— manifest
验证与反射验证都看不到固定功能状态, 谁都拦不住两边写歪。PSO 层是**执行**层, 它应当要求
一个已经完整的状态, 而不是替上层猜缺失的部分。

**key 组成**: `ShaderPassProgram*` (代表 `PipelineLayout` + vertex input, 二者都是 pass
级、与 variant 无关) + 各 stage 的 `ShaderHash` + `render::RenderPass*` +
`PrimitiveState` + `optional<DepthStencilState>` + `MultiSampleState` +
`ColorTargetState[]`。

- `RenderPass*` 直接做身份: `RenderPassRegistry` 已按 attachment 描述去重, 指针相同
  即兼容类相同。
- 【刻意不用 `ShaderVariantKey` 做 key】`shader_manifest.h:335-336` 已指出它是变长结构、
  属作者期概念, 不该进每帧路径。运行时身份用解析后的 `ShaderHash` —— 两个不同 variant
  若投影到同一份字节码 (`ShaderProgramVariant` 的共享机制), 本就该命中同一个 PSO。
- 比较方式先用线性扫 + 值比较 (`operator==` 已在 `rhi.h:991/1062/1077/1117`), 照
  `RenderPassRegistry` (`gpu_resource.cpp:853/882`) 的先例。成为热点再换哈希。

**只做 graphics**: compute PSO 目前零消费者, 两份 manifest 里也没有 compute pass。

**析构顺序**: PSO 存 `RenderPass*` 与 `PipelineLayout*` 裸指针, 故 PSO 库必须先于
`_renderPassRegistry` 与所有 `ShaderAsset` 销毁。已在 `render_system.h` 里靠成员声明顺序
(`_pipelineStateCache` 在 `_renderPassRegistry` 之前) 与 `render_system.cpp` 的显式
`reset()` 顺序两处落实。

**实施结果 (2026-07-28)**:
- 条目除 `StreamingAssetRef` 外还存一个 `const ShaderAsset* Owner` 裸指针。【必须如此】
  `RemovePipelineStatesUsing` 不能用 `Ref.Get()` 匹配 —— 资产被 `Unload` 后 ref 立刻失效
  返回 `nullptr`, 而那正是最需要逐出的时刻。`Owner` 只作身份比较, 从不解引用。
- 非法 key (program / render pass 为空) 在解析变体【之前】就被拒, 故连 miss 都不计。
- 切片改造后净减约 25 行: 原先的 `LoadShaderAssetDesc` → `BuildPipelineLayoutStorage` →
  `CreatePipelineLayout` → `ShaderVariantDomain::Build` → `ShaderResolver` → 逐 stage
  `Resolve` + `CreateShader` → `BuildVertexInputStorage` → `CreateGraphicsPipelineState`
  共八步, 压成 `LoadShaderAsset` + `GetOrCreateVariant` + `GetOrCreateGraphics` 三步。
  这是分层形状的检验: 若比手写还长, 说明抽象切错了位置。
- 8.4 的"`Shader` 是瞬态局部量"结论首次兑现: 它现在只出现在
  `PipelineStateCache::GetOrCreateGraphics` 的函数体内, 全仓库无第二处 `CreateShader`
  调用。

### 8.7 抽出 `radrayshader` 库的实施计划 (2026-07-28)

8.4c 已裁决要抽库, 本节把它落成可执行步骤。核查过依赖与消费者后, 8.4c 的三条依据全部
成立, 但有四处它没预见的细节。

**依据复核 (数字已更新)**

shader 相关 .cpp 现为 4715 行 (`shader_manifest.cpp` 3184 + `shader_asset_template.cpp`
1065 + `shader_program.cpp` 164 + `shader_reflection_map.cpp` 129), 占 `radrayruntime`
全部 8378 行的 56% —— 比 8.4c 记录的 51% 更高, 因为其间 `shader_manifest.cpp` 有过精简
而其余部分在增长。单个 `shader_manifest.cpp` 仍比 `application.cpp` (798) +
`gpu_resource.cpp` (874) + `gpu_system.cpp` (607) 之和多。

**8.4c 未预见的四处细节**

1. ~~`render_resource_recycler.h` 一并挪进 shader 库~~ **作废, 见 8.9。**
   本条原打算"把 `render_resource_recycler.h` 跟着搬进 shader 库", 那是拿 include 关系
   倒推库归属 —— 用错误的分界线去迁就一个 include, 而不是先问这个 include 为何存在。
   正确结论: `shader_program.*` 本身就该留在 runtime, 于是这个 include 根本不需要动。

2. **`RADRAY_ENABLE_SHADER_JIT` 的 option 条件必须改。**
   `CMakeLists.txt:60` 现在写 `cmake_dependent_option(... "RADRAY_BUILD_RUNTIME AND
   RADRAY_ENABLE_DXC")`。抽库后 JIT 是 shader 库的能力, 与 runtime 无关, 条件应改为
   `RADRAY_BUILD_SHADER AND RADRAY_ENABLE_DXC`。**漏改的后果不是编译错误而是静默降级**:
   关掉 `RADRAY_BUILD_RUNTIME` 只想构建 shader 库 + cook CLI 时, JIT 会被静默关成 OFF,
   cook 出来的产物 toolchain hash 与开 JIT 的机器不同 —— 正是 8.4c 第 1 条要防的那类
   静默失效, 只是换了触发路径。

3. **`tools/shader_cook` 与 `tools/shader_gen` 的 `add_dependencies` 必须重指。**
   两者现在都 `add_dependencies(... radrayruntime)`, 注释明确写"radrayruntime 的
   POST_BUILD 已把它们放进同一输出目录"。POST_BUILD 跟着挪到 `radrayshader` 后,
   这两处必须改成 `radrayshader`, 且 `target_link_libraries` 也从 `radrayruntime` 换成
   `radrayshader` —— 这正是 8.4c 依据三要拿到的收益 (两个 CLI 不再链 imgui / freetype /
   cgltf / window)。

4. **shaderlib 部署的 POST_BUILD 挂在静态库上, 语义要重新确认。**
   `$<TARGET_FILE_DIR:radrayruntime>` 对静态库指向的是 lib 输出目录。当前之所以能用,
   是 `radray_set_build_path` 把所有产物 (含静态库与 exe) 收进同一个
   `_build/<Config>/`。挪到 `radrayshader` 后这个前提不变, 故可直接搬。但**搬完必须实测
   `$<TARGET_FILE_DIR:radrayshader>/shaderlib` 确实等于 exe 所在目录**, 而不是只看编译
   通过 —— 若它落错目录, 症状是运行期找不到 shaderlib, 编译期毫无提示。

**目录与命名**

**注意**: 下面这张表的 `shader_program.*` 归属已被 8.9 推翻, 以 8.9 的表为准。

| 位置 | 内容 |
|---|---|
| `modules/shader/include/radray/shader/` | `shader_manifest.h`、~~`shader_program.h`~~、`shader_asset_template.h`、~~`render_resource_recycler.h`~~ |
| `modules/shader/src/` | 对应 .cpp + 私有 `shader_manifest_json.h`、`shader_reflection_map.h/.cpp` |
| `modules/shader/tests/` | `test_shader_asset.cpp`、`test_shader_asset_template.cpp` |
| `modules/runtime/` 保留 | `shader_asset.*`、`pipeline_state_cache.*`、**`shader_program.*`** (见 8.9), 测试保留 `test_shader_program.cpp`、`test_pipeline_state_cache.cpp`、`test_vertical_slice.cpp` |

include 路径 `radray/runtime/...` → `radray/shader/...`, 库名与路径对齐 (8.4c 要求)。
新增 `RADRAY_BUILD_SHADER` option, 依赖 `RADRAY_BUILD_RENDER`;
`modules/CMakeLists.txt` 里 `add_subdirectory(shader)` 插在 render 与 runtime 之间。

**`test_shader_program.cpp` 的归属**: 8.4c 说"涉及 `ShaderAsset` 的用例留在 runtime
侧", 核查后该文件 10 个用例**全部**经 `shader_asset.h` 加载, 无一个只用 program 层。
故整个文件留在 runtime, 不拆分。

**执行顺序** (每步都要能独立编过, 便于二分定位):
1. 建 `modules/shader/` 骨架 + CMakeLists + `RADRAY_BUILD_SHADER` option, 空库先编过。
2. `git mv` 搬 2 组文件 + 2 个私有头 (8.9 修正: `shader_program.*` 不搬),
   改 `#include` 与 include guard 路径。
3. 搬 compile definitions (含 `FATAL_ERROR` 检查) 与 POST_BUILD; 改 `CMakeLists.txt:60`
   的 option 条件。
4. 改两个 CLI 的 link + `add_dependencies`; 改 `examples/sphere_demo` 的 include。
5. 搬两个测试文件到 `modules/shader/tests/`, 连带 `RADRAY_PROJECT_DIR_DEFAULT` 那段。
6. 全量构建 + 全量 ctest, 逐项核对第 (4) 条的运行期目录假设。

**验收标准**: 用例总数不变 (当前 360, 且须做 8.8 要求的双向集合比较), 且两个 CLI 的
链接闭包不含 imgui / freetype / cgltf / radraywindow。第二条要显式查, 因为它是抽库的
**目的**, 而编译通过并不能证明它达成 —— 具体查法与 `d3d12.dll` 的例外见 8.10 第四点。

### 8.8 抽库前置: ctest 注册污染 (2026-07-28, 已修)

规划抽库时按 8.4c 的验收标准 (用例总数不变) 去建立基线, 发现基线本身是坏的 —— 这必须
先修, 否则"总数不变"无从判断。

把 21 个 exe 的 `--gtest_list_tests` 自报用例与 `ctest -N` 逐项做集合比较, 发现三处不一致:

1. **`test_json` 的 ctest 注册指向了 `test_binary_io` 的用例。**
   `build_debug/.../test_json_e3b0c442_tests.cmake` 里三个 `add_test` 全是
   `BinaryIoTest.*`, 但 exe 路径是 `test_json.exe`。于是这三个 ctest 条目实际执行
   `test_json.exe --gtest_filter=BinaryIoTest.*`, **匹配到 0 个用例, gtest 返回 0,
   ctest 记为 Passed** —— 空跑伪装成通过。同时
   `JsonTest.SupportsNonNullTerminatedStringViews` 从未被注册, 从来没跑过。
   `BinaryIoTest` 的三个用例因 `test_binary_io` 自己的注册仍在真实运行, 故总数虚高 3。
   成因是 `gtest_discover_tests` 的 POST_BUILD 发现结果串到了另一个目标的输出文件
   (两者共用 `e3b0c442` 后缀, 且 `test_json.cpp` 在 `7fe92f1a` 重构中改过)。
   删掉 exe 强制重新发现后自愈; 连做 6 轮并行重建未能复现, 故判定为一次性陈旧产物,
   不是稳定竞态。
2. **`test_shader_asset_sample.exe` 是无源僵尸。** 仓库里已无
   `test_shader_asset_sample.cpp`, 任何 CMakeLists 也不再提它, 但产物与
   `test_shader_asset_e3b0c442_*.cmake` 仍留在 build 树里, 又贡献 2 个用例
   (`ShaderAssetSampleTest` 的 6 个用例现由 `test_shader_asset.cpp` 提供)。
3. 修正后 ctest 与 exe 自报**双向完全一致**, 无重复项, 总数 **360**。

**教训**: 上一轮把 "362 全绿" 当作回归通过的依据, 但其中 3 条是空跑, 1 个真实用例从未
被执行。**`ctest -N` 的条数不足以作为覆盖面证据** —— 它只说明有多少条注册, 不说明注册
指向的用例存在。此后凡以"用例总数"作为验收标准 (含 8.7), 都应同时做一次
exe 自报 ↔ ctest 的集合比较, 而不是只比数字。

**遗留**: `gtest_discover_tests` 的 hash 后缀在不同目标间可以相同 (`e3b0c442` 被
`test_json` / `test_binary_io` / `test_shader_asset` 等共用), 这是串台的必要条件。
本轮未深究其生成规则, 记为已知隐患: 若再次出现, 应考虑给 `radray_add_test` 传
`DISCOVERY_EXTRA_ARGS` 或改用唯一的 `TEST_PREFIX`。

### 8.9 修正 8.7: `shader_program.*` 留在 runtime, 分界线是 GPU 对象所有权 (2026-07-28)

8.7 第 1 条的处理方式错了。它发现 `shader_program.cpp` include 了
`render_resource_recycler.h`, 便决定把后者一起搬进 shader 库 —— 这是**拿 include 关系
倒推库归属**: 为了保住"`shader_program.h` 属于 shader 库"这个预设, 去搬动一个碍事的
依赖, 而没有先问这个 include 为什么存在。

**它存在的原因恰恰说明 `shader_program.*` 不属于 shader 库。**

`ShaderPassProgram` 持有 `unique_ptr<render::PipelineLayout>` (`shader_program.h:169`),
并且有 `ReleaseRenderResources(IRenderResourceRecycler&)` (`:143`) 把它交出去延迟释放。
它是**活的 GPU 对象所有者**, 有设备生命周期, 要参与帧回收 —— 这是运行时职责, 不是格式
职责。那个 include 不是意外, 是这一事实的直接后果。

**真正的分界线: 是否拥有活的 GPU 对象。**

按此标准实测 (grep `render::Device` / `CreateShader` / `CreatePipelineLayout` /
`RenderBase` / `Recycler` / `unique_ptr<render::`):

| 文件 | 命中 | 归属 |
|---|---|---|
| `shader_manifest.h/.cpp` | 仅 3 处**注释**提及 `CreateShader` | shader 库 |
| `shader_asset_template.h/.cpp` | 0 | shader 库 |
| `shader_reflection_map.h/.cpp` | 0 | shader 库 |
| `shader_program.h/.cpp` | 8 处**真实**使用 | **runtime** |

格式层三组文件里 `render::Device` 一次都没出现过 —— 它们只产出**描述与字节**
(`ShaderBytecode` 是纯数据, 注释说"可直接喂给 `CreateShader`", 但自己不调)。
`shader_manifest.h` 里连 `Device` 这个名字都没有 (实测 grep `class Device|Device\*|
Device&` 零命中)。这才是"格式层"名副其实的样子, 也是 cook / codegen CLI 真正需要的
全部。

**旁证一, 消费者从不越界**: `tools/shader_cook`、`tools/shader_gen`、`examples` 里
`ShaderPassProgram` / `ShaderProgramVariant` / `IRenderResourceRecycler` **零引用**。
8.4c 依据三说"两个 CLI 只需要格式层", 现在可以更精确: 它们需要的正是上表前三行, 一行
不多。

**旁证二, 待搬的测试也从不越界**: `test_shader_asset.cpp` (227 用例) 与
`test_shader_asset_template.cpp` (40 用例) 对 `ShaderPassProgram` 零引用。program 层
只被 `test_shader_program.cpp` / `test_pipeline_state_cache.cpp` /
`test_vertical_slice.cpp` 用, 而这三个都因需要 device 而留在 runtime。**测试的分布本身
就描出了这条分界线**, 与上表完全吻合。

**旁证三, layout 的创建方本就在 runtime**: `CreatePipelineLayout` 的唯一调用点是
`shader_asset.cpp:140`, 而 `BuildPipelineLayoutStorage` (`shader_manifest.cpp:2459`)
只产出描述。"描述在 shader 库、创建在 runtime" 这条线早就存在, 8.7 差点把它跨过去。

**修正后的归属**

| 库 | 内容 |
|---|---|
| `radrayshader` | `shader_manifest.*`、`shader_asset_template.*` + 私有 `shader_manifest_json.h`、`shader_reflection_map.h/.cpp` |
| `radrayruntime` | `shader_program.*` (活 GPU 对象)、`shader_asset.*`、`pipeline_state_cache.*` |

`render_resource_recycler.h` **原地不动**, 留在 `radray/runtime/`。

**连带收益**: 8.7 的执行步骤 2 从"搬 4 组文件"减为 3 组;
`radray/runtime/shader_program.h` 的 include 路径无需改动, 而它是 `shader_asset.h` /
`pipeline_state_cache.h` 的直接依赖 —— 少动一层就少一次全仓库 include 重写。
被搬走的 .cpp 行数从 4715 降为 4378 (占 `radrayruntime` 的 52%), 依据二的体量论证不受
影响。

**8.4b/8.4c 的三层分界仍然成立**, 只是库边界落在**格式层与对象层之间**, 而不是 8.4c
表格暗示的"格式层 + 对象层 vs 资产层"。8.4c 那张表把 `shader_program.*` 划进新库是错的,
以本节为准。8.4b 说"program 层也停在 Asset 之下"依然对 —— 停在 Asset 之下不等于要
离开 runtime, 这两件事被 8.4c 混为一谈了。

### 8.10 两个 CLI 能脱离 runtime, 但脱不了 render (2026-07-28)

抽库后 `shader_cook` / `shader_gen` 是否能彻底脱离 `radrayruntime`, 甚至 `radrayrender`?
**结论: 脱 runtime 可以且是白拿的收益; 脱 render 不行, 且不该试。**

**一, 脱 runtime: 成立。**

两个 CLI 的 include 已经很干净 (各 4 个 radray 头):
`enum_flags.h` / `file.h` / `types.h` / `render/dxc.h` + 一个 shader 头
(`shader_manifest.h` 与 `shader_asset_template.h`)。**零个其他 runtime 头** —— 没有
`asset.h`、没有 `asset_manager.h`、没有 `application.h`。所以 8.7 步骤 4 把
`target_link_libraries` 从 `radrayruntime` 换成 `radrayshader` 后, 两个 CLI 与 runtime
再无关系。这正是 8.4c 依据三预期的收益 (不再链 imgui / freetype / cgltf / window)。

**二, 脱 render: 不成立, 因为格式层真的在用 render 的东西。**

不是"include 了但没用"。实测格式层调用了三个 render 函数, 且都定义在 `rhi.cpp`:
- `render::GetVertexFormatSizeInBytes` (`rhi.cpp:213`), 被 `shader_manifest.cpp:392` 与
  `shader_asset_template.cpp:879` 调用;
- `render::IsDynamicShaderParameterBindingType` (`rhi.cpp:319`), 被
  `shader_manifest.cpp:128` 调用;
- `render::Dxc` 整套 (`DxcCompileOptions` / `DxcOutput`), 这是 JIT 与 cook 的核心。

更根本的是**类型**: `shader_manifest.h` 里出现 22 个 `render::` 名字
(`ShaderDescriptor`、`VertexInputState`、`PipelineLayoutDescriptor`、`ShaderStage`、
`ShaderBlobCategory`、`VertexFormat` ...)。manifest 的产出物就是"能直接喂给 RHI 的
描述", 这些类型是它的**值域**, 不是实现细节。剥掉 render 就等于把这些类型在 shader 库
里再定义一遍 + 写转换层 —— 那是 G12 式的第二套真相, 代价远大于收益。

【唯一的例外确认过了】`render::Device` 在两个格式层头里**只出现在一句注释**里
(`shader_manifest.h:608`), 实际代码零引用。也就是说格式层用的是 render 的**描述类型与
纯函数**, 从不碰设备对象 —— 这与 8.9 的分界线完全一致, 也说明 render 这层依赖是"用
数据契约", 而非"用运行时"。

**三, 但当前有一个真实的、可修的代价: CLI 硬依赖 `d3d12.dll`。**

实测 `radray_shader_cook.exe` 的导入表 (dumpbin /DEPENDENTS) 含 **`d3d12.dll` 与
`dxgi.dll`**, 且 `/IMPORTS:d3d12.dll` 显示唯一被引用的符号是
`D3D12SerializeVersionedRootSignature` —— 来自 `d3d12_impl.cpp:2023`, 一个 cook 永远
走不到的代码路径。

传导链 (已用 dumpbin /SYMBOLS 验证): 格式层调 `GetVertexFormatSizeInBytes` →
该符号在 `rhi.cpp.obj` → 同一个 obj 里 `RhiCreateDevice` / `CreateDXGIFactory` 的
`#ifdef RADRAY_ENABLE_D3D12` 分支对 `d3d12::CreateDevice` /
`d3d12::CreateDXGIFactory` 有 **UNDEF 外部引用** → 链接器为解析它们必须拉入整个 d3d12
后端 → 后端引入 `d3d12.dll` 导入项。

**obj 是链接粒度**: 一个纯枚举尺寸查表函数与设备工厂放在同一个 .cpp 里, 于是"想知道
`VertexFormat` 有几个字节"就得背上整个 D3D12 后端。这是 12.7MB exe 的主要来源。

**裁决: 记为已知缺口, 本轮不修, 不阻塞抽库。** 修法是把 `rhi.cpp` 里的纯函数
(格式尺寸查表、`IsXxxFormat`、`IsDynamicShaderParameterBindingType` 等, 均无状态、
无后端分支) 拆到独立的 `rhi_format.cpp`, 让链接器能只取这一个 obj。这是 render 模块
内部的整理, 与 shader 抽库正交 —— 混在一起做会让"抽库是纯搬运"这个前提失效
(8.4c 刻意把抽库排在 PSO 库之后, 就是为了保住这个前提)。

**四, 修正 8.7 的验收标准。**

8.7 原写"查 `radrayshader` 链接闭包不含 imgui / freetype / cgltf / radraywindow"。
按本节结论, 验收应改为可观测的二进制事实, 而非 CMake 声明:
- 两个 CLI 的 `dumpbin /DEPENDENTS` **不应**出现 imgui / freetype 相关依赖;
- `d3d12.dll` / `dxgi.dll` **仍会**出现 (本节第三点, 已知缺口), 不作为失败;
- exe 体积应下降 (当前各 12.7 / 12.8MB), 作为参考指标而非硬门槛。

【为何要看二进制而不是看 CMake】`target_link_libraries` 只说明声明的依赖, 说明不了
链接器实际拉进了什么 —— 本节第三点正是"CMake 上只写了 radrayrender, 二进制里却多了
一个 d3d12.dll"的例子。

### 8.11 CLI 的跨平台性: render 依赖不是障碍, 但当前配置矩阵有一处真空 (2026-07-28)

`radrayrender` 高度平台相关, 而 cook / gen 本身是平台无关的工具, 依赖它是否会让两个 exe
难以跨平台? **结论: 不会 —— 平台相关的部分全部是 option 化的, 且在非 Windows 上自动
关闭。但 8.10 第三点那条链在 macOS 上会换个后端重演, 且存在一个尚未被任何 preset 覆盖
的配置。**

**一, 8.10 里那个 `d3d12.lib` 并非 render 的固有属性。**

追到了具体来源: 它不是任何 `#pragma comment(lib)` 也不是 render 自己写的, 而是
`RADRAY_ENABLE_D3D12` 拉起的 `D3D12MemoryAllocator` 通过 PUBLIC 传递上来的
(`build.ninja` 的 `LINK_LIBRARIES` 里 `D3D12MAd.lib` 紧跟着 `d3d12.lib dxgi.lib
dxguid.lib`)。

而 `RADRAY_ENABLE_D3D12` 是 `cmake_dependent_option(... "RADRAY_BUILD_RENDER AND
WIN32")` (`CMakeLists.txt:55`) —— **在 Linux / macOS 上它必然为 OFF**, 于是
`src/d3d12/*.cpp` 根本不参与编译 (`modules/render/CMakeLists.txt:20-22` 的 glob 在
`if (RADRAY_ENABLE_D3D12)` 内), D3D12MA 也不会被 `add_subdirectory`
(`CMakeLists.txt:369`)。同理 Metal 由 `APPLE` 门控, Vulkan 用 volk 的 headers-only
形式 (`volk::volk_headers`), 实测两个 exe 的导入表里**没有 `vulkan-1.dll`** —— 它是
运行期动态加载, 不产生链接期平台依赖。

所以 render 的平台相关性是**编译期可裁剪**的, 不是"链接它就绑死一个平台"。cook / gen
在 Linux 上会得到一个不含任何图形 API 导入的 exe。

**二, 真正会跨平台复发的是 8.10 的 obj 粒度问题, 不是 render 本身。**

在 macOS 上同一条链会换成 Metal / Cocoa 重演: 格式层调 `GetVertexFormatSizeInBytes` →
`rhi.cpp.obj` → 其中 `RhiCreateDevice` 的 `#ifdef RADRAY_ENABLE_METAL` 分支引用
`metal::CreateDevice` → 拉入 Metal 后端 → 而 `APPLE AND RADRAY_ENABLE_VULKAN` 那段
(`modules/render/CMakeLists.txt:43-49`) 是 **PUBLIC** 链接 `Cocoa` 与 `QuartzCore`
framework。于是一个 shader codegen CLI 会链上 Cocoa。

这加强了 8.10 的结论: 该缺口不是"Windows 上多一个 dll"这种局部瑕疵, 而是**每个平台都
会以本地形式复发**。修法不变 (把 `rhi.cpp` 的纯函数拆到 `rhi_format.cpp`), 但优先级
应上调 —— 记为 G14。

**三, 发现一处配置真空: 没有任何 preset 构建 `RADRAY_ENABLE_D3D12=OFF`。**

`d3d12.lib` 混进 CLI 这件事在 Windows 上永远存在, 在非 Windows 上永远不存在, 于是
"cook 不该依赖图形后端"这个性质**在本仓库的 CI 矩阵里从未被检验过** ——
`CMakePresets.json` 全是 win-x64。这与 8.8 的教训同源: 一个性质若没有任何配置会让它
失败, 它就不算被验证。

建议 (不阻塞抽库): 加一个 `RADRAY_BUILD_RENDER=ON` + `RADRAY_ENABLE_D3D12=OFF` +
`RADRAY_ENABLE_VULKAN=OFF` 的配置只构建 `radrayshader` + 两个 CLI, 作为"格式层不依赖
任何后端"的守门配置。它同时能守住 8.7 第 2 条那个 `RADRAY_ENABLE_SHADER_JIT` 的
option 条件 —— 那条改动的失败模式 (JIT 被静默关掉) 恰好只在"不构建 runtime"的配置下
才暴露, 而目前没有这样的配置。

**四, 对抽库计划的影响: 无。**

抽库后 CLI 的依赖是 `radraycore + radrayrender + radrayshader`, 三者都是编译期可裁剪
的。跨平台性不因抽库变好也不变坏 —— 变好的是"能构造出一个不含后端的配置"这件事从
不可能 (CLI 必须链 runtime, runtime 必须链 window) 变成可能。这算抽库的一项额外收益,
但要真正兑现需要 G14 与第三点的守门配置。

### 8.12 更激进的拆库: 不应把 render 的内容拆进 runtime, 但确实该拆 render 自身 (2026-07-28)

问题: 既然 8.11 暴露了"CLI 被迫链入图形后端", 是否该更激进 —— 把 `radrayrender` 的一部分
内容拆进 `radrayruntime`?

**结论: 方向反了, 不该这么拆。但问题问对了一半 —— render 确实该拆, 只是应沿"编译器 /
设备"这条缝拆成两个平级库, 而不是把内容往 runtime 里搬。**

**一, 为什么"拆进 runtime"不成立。**

依赖方向是 `core ← render ← (shader) ← runtime`。把 render 的内容搬进 runtime 意味着让
**下游**吸收上游的职责, 这会产生两个后果:

- **加重而非减轻 8.10/8.11 的病症。** 那两节的根因是"工具为了拿一个纯函数, 被迫链入
  它不需要的东西"。cook / gen 抽库后恰恰是**不依赖 runtime** 的 (8.10 第一点已核实:
  两个 CLI 零个非 shader 的 runtime 头)。把 `rhi.h` 的类型往 runtime 搬, 等于把它们挪到
  CLI 够不着的地方 —— 而 `shader_manifest.h` 用了 22 个这类 `render::` 名字, CLI 会被迫
  重新依赖 runtime, 即 8.4c 依据三想消掉的那条依赖原地复活。
- **runtime 会成为第二个巨物。** `radrayruntime.lib` 已是 231MB (render 37.8MB,
  core 17MB), 是仓库里最大的静态库。它现在的问题是承担太多, 不是太少。

**二, 但 render 内部确实有一条干净的缝, 且实测无环。**

render 的 8378 行源码可清晰二分:

| 组 | 文件 | 行数 | 依赖 device 层? |
|---|---|---|---|
| 编译器/反射 | `dxc.cpp`、`hlsl.cpp`、`spirv.cpp`、`spvc.cpp`、`msl.cpp` | 2646 | **零** |
| 设备/后端 | `rhi.cpp` + `d3d12/*` + `vk/*` | 11588 | — |

实测: 编译器那五个 .cpp 对 `Device` / `SwapChain` / `CommandQueue` / `backend/`
**零引用** (`spvc.cpp` 的 14 处命中经查全是名为 `markDeviceStorage` 的局部 lambda,
非 `render::Device`)。头文件侧, `dxc.h` 从 `rhi.h` 只取 **`ShaderStage` 与
`ShaderBlobCategory`** 两个枚举, `spvc.h` 只取 `ShaderStage`。

也就是说 shader 工具链需要的是 render 的**枚举与描述类型**, 加上一个不碰设备的编译器 —— 
这与 8.9 的结论完全一致 (格式层用"数据契约", 不用"运行时"), 也解释了 8.10 那条链为何
显得荒谬: 它跨过了一条本该存在的库边界。

**三, 若要激进, 正确的形态是把 render 拆成两个平级库。**

```
core ← rhi_types (纯枚举/描述/纯函数)
         ├── shadercompiler (dxc/hlsl/spirv/spvc/msl)  ← radrayshader ← runtime
         └── radrayrender  (Device/后端, 平台相关)      ←──────────────── runtime
```

`rhi.h` 的结构已经支持这条线: 前 476 行是纯枚举与描述 struct, `class Device;` 等对象
声明从 **477 行**才开始。拆点是现成的, 不需要重新设计类型。

这样 cook / gen 的依赖变成 `core + rhi_types + shadercompiler + radrayshader`, **完全
不含任何图形后端**, 8.11 第三点那个守门配置也就自然成立 (不需要靠关 option 来模拟)。

**四, 裁决: 记为 G15, 不在本轮做, 且 G14 仍是更优的第一步。**

【为何不现在做】三条理由:
- **G14 用 1 个文件的代价拿到 80% 的收益。** 把 `rhi.cpp` 的纯函数拆到
  `rhi_format.cpp` 就能断开 8.10 那条链 (格式层只需要 `GetVertexFormatSizeInBytes` /
  `IsDynamicShaderParameterBindingType` 两个函数)。G15 要动 4 个库的边界与全部
  `#include`, 收益是"依赖图更诚实", 而非"新解决了什么"。
- **本轮的前提是"抽库是纯搬运"** (8.4c 刻意把抽库排在 PSO 库之后就为保住这个前提)。
  同时重划 render 的边界会让这个前提失效, 一旦出错就无法二分定位是哪一半的问题。
- **G15 的收益要等真实的跨平台构建才能验证。** 8.11 第三点已指出仓库目前只有 win-x64
  preset; 在没有非 Windows CI 的情况下做 G15, 等于凭推理重划边界而无法证伪 ——
  和 8.8 那个空跑测试同类的错误。

**顺序**: 抽 `radrayshader` (第 7 节第 3 步) → G14 (拆 `rhi_format.cpp`) →
补 8.11 第三点的守门配置 → 视需要再评估 G15。前三步都是局部且可独立验证的, G15 是唯一
需要整体重构的一步, 应当最后并在有跨平台验证手段之后再做。

**本节的排期建议已被用户裁决覆盖 (2026-07-28)**: 用户要求一次性完成整个结构重构, 并定名
`radraygal` / `radrayshader` / `radraygpu`。执行方案见 **8.13**, 其中 G14 被并入第 1 步
顺带完成, G15 即该方案本身。本节的技术分析 (依赖方向无环、缝在哪) 仍然有效, 只有"排到
最后做"这条排期结论作废。

### 8.13 ~~一次性重构为 gal / shader / gpu 三库~~ 已被 8.15 取代 (2026-07-28)

**本节方案作废**, 以 **8.15** 为准 (用户提出改动更小的 `core ← shader ← render ←
runtime` 布局, 实测更优)。本节保留作为决策记录: 其中"边界标准是是否持有 device 对象
指针"的分析、命名空间裁决、以及第七节的风险清单在 8.15 中仍然沿用。以下为原文。

---


用户要求一次性完成整个工程结构重构, 并指定了库名与职责:

| 库 | 职责 | 对应本文档此前的称法 |
|---|---|---|
| `radraygal` | GPU Abstract Layer: 纯枚举 / 描述 struct / 纯函数 | 8.12 的 `rhi_types` |
| `radrayshader` | shader 与编译器相关 (manifest + 格式层 + dxc/hlsl/spirv/spvc/msl) | 8.12 的 `shadercompiler` + 8.9 的 `radrayshader` **合并** |
| `radraygpu` | 设备 / 后端, 平台相关 | 8.12 的 `radrayrender` (原 `radrayrender` 更名) |
| `radrayruntime` | 引用上述三库 | 不变 |

依赖: `core ← gal ← {shader, gpu} ← runtime` (shader 与 gpu 平级, 互不依赖)。

这**覆盖了 8.12 第四点"G15 排到最后"的建议**。8.12 的顾虑 (无跨平台 CI 无法证伪) 依然
成立, 记录在下面的风险一节; 但用户已明确要一次做完, 故按此执行。合并 8.12 的
`shadercompiler` 与 8.9 的 `radrayshader` 是用户的简化, 且是合理的 —— 二者的消费者完全
重合 (cook / gen / 格式层), 分成两个库会多一层边界而无额外收益。

**一, 可行性已实测: 三个方向都无环。**

- **gpu 不依赖 shader**: `rhi.cpp` / `d3d12/*` / `vk/*` / `backend/*.h` 对
  `dxc.h` / `hlsl.h` / `spirv.h` / `spvc.h` / `msl.h` **零 include**; 对
  `HlslShaderDesc` / `SpirvShaderDesc` / `MslShaderDesc` / `HlslReflection` **零引用**。
  后端消费的是**编译后的字节**, 不是反射结构。
- **shader 不依赖 gpu**: 编译器五个 .cpp 对 `Device` / `SwapChain` / `CommandQueue` /
  `backend/` 零引用 (8.12 已核实); 对 `rhi.cpp` 里的纯函数也几乎不用 —— 唯一命中是
  `msl.cpp` 的 5 处 `format_as`, 而 `format_as` 全部作用于纯枚举, 归 gal。
- **gal 谁都不依赖**: 只需 `core` (`types.h` / `nullable.h` / `enum_flags.h` /
  `basic_math.h`, 即 `rhi.h` 现有的全部 include)。

**二, 关键实测: gal 的边界不是一条"行号切割线"。**

8.12 曾说"`rhi.h` 前 476 行是纯类型, 477 行起是 device 对象, 拆点现成"。**这句是错的,
需要更正** —— 实际布局是描述 struct 与 device class 交错的:

- `RenderBase` / `IDebugName` 在 **516/532**, 早于大量描述 struct;
- shader 层需要的描述分布在 **677 (`SamplerDescriptor`)** 到
  **969 (`VertexInputState`)**;
- device class 主体在 1196 之后, 但 `SwapChainFrame` (703)、`Sampler` (1522)、
  `SamplerCache` (1596) 散落其间。

所以拆分标准必须是**语义**而非位置: **一个类型进 gal 的充要条件是它不含任何 device
对象指针**。已按此标准全量扫描 `rhi.h` (正则匹配成员里的 `Device*` / `Buffer*` /
`Texture*` / `Shader*` / `PipelineLayout*` / `RenderPass*` ... ), 结果:

- **32 个**类型持有 device 对象指针 → 必须留在 gpu。含 `SwapChainDescriptor` (607,
  持 `CommandQueue*`)、`TextureCopyDescriptor` (638, 持 `Texture*`)、
  `FramebufferDescriptor` (811, 持 `RenderPass*`)、`ShaderEntry` (1120, 持 `Shader*`)、
  `GraphicsPipelineStateDescriptor` (1125, 持 `PipelineLayout*`)、`SamplerCache` (1596)。
  **注意**: "Descriptor"这个后缀不代表能进 gal, 有 13 个 Descriptor 持有 device 指针。
- shader 层需要的 10 个类型 (`SamplerDescriptor`、`ShaderDescriptor`、
  `ShaderBindingLocation`、`ShaderParameterSetLayoutEntryDescriptor`、
  `ShaderParameterSetLayoutDescriptor`、`PushConstantDescriptor`、
  `PipelineLayoutDescriptor`、`VertexAttribute`、`VertexBufferLayout`、
  `VertexInputState`) **全部零 device 指针** → 全部可进 gal。这是本方案成立的关键前提,
  已逐个验证。
- PSO 固定功能状态 6 个 (`PrimitiveState`、`DepthStencilState`、`MultiSampleState`、
  `ColorTargetState`、`BlendState`、`StencilFaceState`) **全部零 device 指针** → 进 gal。
  于是 8.6 的 `GraphicsPipelineStateKey` 的固定功能段也只依赖 gal。
- 35 个 `enum class` + 16 处 `is_flags`/`EnumFlags` 特化 + 14 个 `format_as` → 全部
  进 gal (`format_as` 的实现即 `rhi.cpp:325-460`, 与设备无关)。

**三, 头文件与目录布局**

```
modules/gal/include/radray/gal/
    gal.h            ← rhi.h 的纯类型部分 (枚举/描述/is_flags/format_as 声明)
modules/gal/src/
    gal.cpp          ← rhi.cpp 的纯函数部分 (格式尺寸表/IsXxxFormat/format_as 定义)
modules/shader/include/radray/shader/
    dxc.h hlsl.h spirv.h spvc.h msl.h        ← 自 render 平移
    shader_manifest.h shader_asset_template.h ← 自 runtime 平移 (8.9 裁决的两组)
modules/shader/src/
    dxc.cpp hlsl.cpp spirv.cpp spvc.cpp msl.cpp d3d12shader.h d3dcommon_adapter.h
    shader_manifest.cpp shader_asset_template.cpp
    shader_manifest_json.h shader_reflection_map.h/.cpp
modules/gpu/include/radray/gpu/
    gpu.h            ← rhi.h 的 device 部分 (RenderBase/IDebugName/32 个持指针类型/所有 class)
    backend/*.h      ← 自 render 平移
modules/gpu/src/
    gpu.cpp d3d12/* vk/*
```

`radraygal` 天然是 header-heavy 但仍需 `gal.cpp` (纯函数的定义)。
`gpu.h` 必须 `#include <radray/gal/gal.h>`。
**`d3d12shader.h` / `d3dcommon_adapter.h` 跟 `dxc.cpp` 走 shader 库** —— 它们是 DXC
反射的非 Windows 适配层, 与 D3D12 后端无关 (名字容易误导)。

**四, 命名空间: 保持 `radray::render` 不变。**

【为何不跟着库名改】三个库共同实现原 `render` 这一层, 拆库是**物理**边界而非概念重命名。
改成 `radray::gal::` / `radray::gpu::` 会让 `ShaderStage` 这类横跨两库使用的类型出现
"声明在 gal 命名空间、被 gpu 大量使用"的割裂, 且要改动全仓库每一处 `render::` 限定
(实测 `shader_manifest.h` 一个文件就有 24 个)。库名表达构建单元, 命名空间表达概念层,
二者不必一致 —— `radraycore` 的内容也在 `radray::` 而非 `radray::core::`。

**五, 执行顺序** (每步独立编过, 保住二分定位能力)

抽库总是"先加边界, 再搬内容, 最后收紧"。**关键: 前 4 步不删旧路径**, 靠 `gal.h` 被
`gpu.h` include 维持全仓库现有 `#include <radray/render/rhi.h>` 可用, 于是每一步都能
全量构建 + 全量 ctest。

1. 建 `modules/gal/`, 从 `rhi.h`/`rhi.cpp` **切出**纯类型与纯函数到 `gal.h`/`gal.cpp`;
   `rhi.h` 顶部 include `gal.h`。此时无任何其他文件改动, 全仓库应当照常编过 —— 这一步
   同时**顺带修掉 G14** (纯函数已在独立 obj, 不再牵连设备工厂)。
   **【8.14 实测: `.cpp` 必须跟着拆, 这是成败关键而非可选优化】** 只拆 `rhi.h` 而把
   `rhi.cpp` 留在原处, `gal` 就仍带着对 `d3d12::CreateDevice` 等 **5 个后端工厂符号**
   的引用, CLI 会照旧被迫链入 d3d12 + vulkan 后端 (约 23 MB), 全部收益归零。
   必须迁入 `gal.cpp` 的最小集合已实测确认为 4 个符号:
   `GetVertexFormatSizeInBytes`、`IsDynamicShaderParameterBindingType`、
   `format_as(ShaderStage)`、`format_as(ShaderBlobCategory)` (连同其余纯函数与
   `format_as` 一并迁移)。留在 `gpu.cpp` 的是 `Device::Create` / `DXGIFactory::Create` /
   `InstanceVulkan::InitEnv` / `SamplerCache` / `SwapChainFrame` 这些。
2. `radrayrender` 更名 `radraygpu`, `rhi.h`→`radray/gpu/gpu.h`, `backend/` 平移。
   全仓库 `#include <radray/render/rhi.h>` → `<radray/gpu/gpu.h>`。
3. 建 `modules/shader/`, 把编译器五组 + 两个私有头自 render 平移进来, 链 `radraygal`。
   此时 shader 库尚不含 manifest, 但已可验证"编译器不需要 gpu"这一核心假设。
4. 把 8.9 裁定的两组格式层文件 (`shader_manifest.*`、`shader_asset_template.*` +
   `shader_manifest_json.h` + `shader_reflection_map.*`) 自 runtime 搬入 shader 库,
   include 路径改 `radray/shader/...`。搬 `RADRAY_ENABLE_SHADER_JIT` /
   `RADRAY_DXC_VERSION` 定义 (含 `FATAL_ERROR`) 与 shaderlib POST_BUILD (8.7 第 2/4 条)。
5. 改两个 CLI: link `radrayshader`, `add_dependencies` 重指 (8.7 第 3 条);
   改 `CMakeLists.txt:60` 的 JIT option 条件为 `RADRAY_BUILD_SHADER AND
   RADRAY_ENABLE_DXC` (8.7 第 2 条); 改 `examples/sphere_demo` 的 include。
6. 搬测试: `test_shader_asset.cpp` / `test_shader_asset_template.cpp` → 
   `modules/shader/tests/` (含 `RADRAY_PROJECT_DIR_DEFAULT` 那段)。
   **`shader_program.*` 与其测试留在 runtime** (8.9 裁决, 不受本节影响)。
7. 收尾: 加 8.11 第三点的守门配置 (`RADRAY_ENABLE_D3D12=OFF` +
   `RADRAY_ENABLE_VULKAN=OFF`, 只构建 gal + shader + CLI); 用 `dumpbin /DEPENDENTS`
   验两个 CLI 不再含 `d3d12.dll` / `dxgi.dll`。

**六, 验收标准**

- 用例总数仍 **360**, 且按 8.8 要求做 exe 自报 ↔ ctest **双向集合比较** (不能只比数字)。
- **主门槛 (须用链接 map, 见 8.14 第四点)**: 两个 CLI 的 `link /MAP` 产物里
  **不含** `d3d12_*.obj` / `vulkan_*.obj` / `D3D12MemAlloc*.obj`。
  【为何不用 `dumpbin /DEPENDENTS`】8.14 实测发现 Vulkan 后端被链入却**不产生 dll
  导入项** (volk 动态加载), 用导入表验会得到假阴性。
- 辅助门槛: `dumpbin /DEPENDENTS` 里 `d3d12.dll` / `dxgi.dll` 消失。
- `radraygpu` 与 `radrayshader` **互不出现在对方的链接闭包**里。
- exe 体积显著下降 (当前各 12.7 / 12.8MB), 参考指标。

**七, 风险与已知代价 (须在开工前认可)**

- **这不再是"纯搬运"。** 8.4c 刻意把抽库排在 PSO 库之后以保住"纯搬运"前提, 本方案
  主动放弃该前提: 第 1 步要把 `rhi.h` (1280 行) 按语义**切开**, 那是判断而非搬运。
  缓解手段是第 1 步不改任何调用方, 使其可被单独验证。
- **`RenderBase` 的归属是唯一真正的判断题。** 它 (`rhi.h:516`) 是所有 GPU 对象的基类,
  语义上属 gpu; 但 `IRenderResourceRecycler` (runtime) 与所有后端都引用它。裁决:
  **进 gpu**, 因为它代表"有设备生命周期的对象", 与 gal 的"纯描述"定位互斥。
  `render_resource_recycler.h` 仍留 runtime (8.9), 它前置声明 `RenderBase` 即可。
- **无跨平台 CI, macOS/Linux 上的收益无法验证** (8.11/8.12 已指出)。本方案能在 Windows
  上证明"CLI 不再链 d3d12", 但"macOS 上不再链 Cocoa"只能靠推理。这是接受用户"一次做完"
  要求时明确承担的风险, 记录在此。
- 第 2 步是全仓库范围的 `#include` 重写 (`rhi.h` 有 26 个 includer)。机械但量大,
  应靠工具批量替换后全量构建验证, 不逐个手改。

### 8.14 实测验证: 拆库后 CLI 的链接内容 (2026-07-28)

问题: 8.13 的三库方案能否保证两个 CLI 只链接必要内容, 不引入多余的?
**答案: 能, 但前提是 `rhi.cpp` 必须跟着 `rhi.h` 一起按语义拆开。**
8.13 第 5 步已经这么写了 (第 1 步同时切 `.h` 与 `.cpp`), 本节用链接器实测证明该步骤是
**必要**的 —— 只拆头文件不拆 .cpp 的话, 全部收益归零。

以下全部结论来自真实链接产物 (`link /MAP` + `dumpbin`), 不是推理。

**一, 当前实况: cook 链入了 8 个 render obj, 其中 4 个是纯浪费。**

用 CMake 生成的完整链接命令重链 `radray_shader_cook` 并产出 map, 得到实际参与链接的
109 个 obj。其中 render 侧 8 个:

| obj | 体积 | 是否必要 |
|---|---|---|
| `dxc.cpp.obj` | 2.3 MB | 必要 (cook 的核心) |
| `hlsl.cpp.obj` | 1.9 MB | 必要 (反射) |
| `spvc.cpp.obj` | 4.8 MB | 必要 (SPIR-V 交叉编译) |
| `rhi.cpp.obj` | 1.4 MB | **仅需其中 4 个纯函数** |
| `d3d12_impl.cpp.obj` | 7.6 MB | **完全无用** |
| `vulkan_impl.cpp.obj` | 11.7 MB | **完全无用** |
| `d3d12_helper.cpp.obj` | 0.6 MB | **完全无用** |
| `vulkan_helper.cpp.obj` | 2.3 MB | **完全无用** |

外加 `D3D12MemAlloc.cpp.obj` (1.1 MB) 与 `d3d10guid.obj`, 以及导入项
`d3d12.dll` / `dxgi.dll`。**约 23 MB 的后端 obj 被链进一个不碰 GPU 的 codegen 工具。**

顺带修正 8.10: 那节只提到 D3D12, 实测 **Vulkan 后端也被链进来了** (`vulkan_impl` 11.7MB
是其中最大的单个 obj)。之所以 8.10 没发现, 是因为 Vulkan 走 volk 动态加载, 不留 dll
导入项 —— **导入表干净不代表没链进来**。这也说明 8.10/8.13 用 `dumpbin /DEPENDENTS`
作验收标准是不够的, 见下面第四点。

**二, 根因确认: 唯一的牵连点是 `rhi.cpp.obj` 的 5 个未解析符号。**

`dumpbin /SYMBOLS rhi.cpp.obj` 显示它对后端有且仅有 5 个 UNDEF 引用:
`d3d12::CreateDevice`、`d3d12::CreateDXGIFactory`、`vulkan::CreateDeviceVulkan`、
`vulkan::InitVulkanEnvImpl`、`vulkan::ShutdownVulkanEnvImpl` —— 全部来自
`Device::Create` / `DXGIFactory::Create` / `InstanceVulkan::InitEnv` 这三个工厂函数
里的 `#ifdef` 分支。

而格式层对 `rhi.cpp.obj` 的需求只有 4 个符号 (`dumpbin /SYMBOLS
shader_manifest.cpp.obj` 的 UNDEF 列表): `GetVertexFormatSizeInBytes`、
`IsDynamicShaderParameterBindingType`、`format_as(ShaderStage)`、
`format_as(ShaderBlobCategory)`。**四个纯函数, 把 23 MB 后端拖了进来。**

编译器三个 obj 则几乎不依赖 `rhi.cpp.obj`: `hlsl.cpp.obj` **0 个**未解析 render 符号,
`spvc.cpp.obj` 只有 `format_as(ShaderStage)` 一个, `dxc.cpp.obj` 只有一个模板析构
thunk。这印证了 8.12/8.13 "编译器不依赖设备层"的核实。

**三, 决定性实验: 两次模拟链接。**

*实验 A (只拆头、不拆 `rhi.cpp`)*: 用 `shader_cook.obj` + 编译器五个 obj +
`rhi.cpp.obj` + 格式层两个 obj, 不给任何后端 lib 去链。
**结果: 失败, 5 个 LNK2019, 全部是上面那 5 个后端工厂符号。**
→ 证明只要 `rhi.cpp` 不拆, `gal` 库里就带着对后端的引用, CLI 必然被迫链入 d3d12 与
vulkan 后端。**光把 `rhi.h` 拆成 `gal.h` + `gpu.h` 是不够的。**

*实验 B (拆掉 `rhi.cpp`, 模拟纯函数已迁入 `gal.cpp`)*: 同上但移除 `rhi.cpp.obj`。
**结果: 恰好 4 个 LNK2019, 且全部是第二点那 4 个纯函数, 零个后端符号。**
→ 证明这 4 个符号一旦搬进 `gal.cpp`, 链接即闭合。**cook 与 gen 将不再引用任何后端
符号**, `d3d12_impl` / `vulkan_impl` / `D3D12MemAlloc` 与 `d3d12.dll` / `dxgi.dll`
全部消失。

两个实验合起来是对 8.13 的一次证伪尝试: 方案通过了, 但暴露出第 1 步的切分粒度是
**成败关键**, 而非可选优化。

**四, 因此修正验收标准 (覆盖 8.10 第四点与 8.13 第六节)。**

`dumpbin /DEPENDENTS` **不足以**作为验收 —— Vulkan 后端被链入却不产生 dll 导入项
(第一点), 用它验会得到假阴性。验收必须用**链接 map**:

- 用 `link /MAP` 或 `/VERBOSE:LIB` 产出 CLI 的实际 obj 清单;
- 断言其中**不含** `d3d12_*.obj`、`vulkan_*.obj`、`D3D12MemAlloc*.obj`;
- 断言不含 `png_*` / `jpeg_*` / `FT_*` / imgui / cgltf (实测当前已不含, 因为静态库按
  obj 粒度取用, 这几项从未被 CLI 引用 —— 8.4c 依据三"CLI 被迫链接 imgui/freetype"
  这句**不准确**, 声明上依赖但链接器并未取用; 真正被浪费的是后端那 23 MB);
- 保留 `dumpbin /DEPENDENTS` 作为辅助 (`d3d12.dll` / `dxgi.dll` 应消失)。

**五, 结论: 会"只链必要内容", 但有三项不可消除的固有成本。**

拆库后 CLI 的 obj 构成应为: `shader_cook.obj` + shader 库 (dxc/hlsl/spvc/spirv/msl +
manifest + reflection_map) + `gal.cpp.obj` + core 侧按需取用的 7 个 obj
(`file` / `json` / `logger` / `binary_io` / `text_encoding` / `allocator` /
`dynamic_library`) + 第三方 (spirv-cross 6 个 obj、fmt、spdlog、yyjson、mimalloc)。

三项固有成本, 属正常而非缺陷:
- **mimalloc 全部 15 个 obj**: 因 `/WHOLEARCHIVE:mimalloc-debug.lib` 而整库链入,
  这是 allocator override 的必然要求。
- **`radraycore` 是单一库**: 它内部不再细分, 但静态库按 obj 取用, 实测只取了 7 个 ——
  PNG/JPEG/freetype/stdexec 均**未**进入 exe。core 不需要拆。
- **spirv-cross**: cook 确实要做 SPIR-V → MSL 交叉编译, 必要。

【一句话】拆库真正消除的是那 23 MB 后端 obj 与 `d3d12.dll`/`dxgi.dll` 导入, 其余部分
现在就已经是"按需链接"的 —— 静态库的 obj 粒度天然提供了这一点, 8.4c 高估了当前的浪费
(以为 imgui/freetype 也被链入), 同时低估了真正的浪费 (漏掉了 Vulkan 后端)。

### 8.15 最终方案 (用户裁决, 覆盖 8.12/8.13): `core ← shader ← render ← runtime` (2026-07-28)

用户提出改动更小且更优的布局, 并要求据此从头设计。**实测验证通过, 本节取代 8.12 与
8.13 的三库方案。**

```
radraycore ← radrayshader ← radrayrender ← radrayruntime
```

- 不新建 `gal` / `gpu` 库, `radrayrender` 保留原名与原职责 (设备 + 后端)。
- `radrayshader` 承载: 从 `rhi.h` 移入的**最小类型集** + 编译器 (`dxc`/`hlsl`/`spirv`/
  `spvc`/`msl`) + shader 格式层 (`shader_manifest.*`/`shader_asset_template.*`)。
- 命名空间**全部保持 `radray::render`** —— 与 8.13 第四节同理, 而且在本布局下更自然:
  `radrayrender` 就在 `radrayshader` 之上, 同一命名空间跨两库是分层实现而非割裂。
- 两个 CLI 只链 `radraycore + radrayshader`。

**为何这个布局优于 8.13 的三库版**

- **少一个库、少一层边界。** 8.13 要建 gal + shader + gpu 三个新构建单元并重命名
  `radrayrender`; 本方案只新增一个库, `radrayrender` 连名字都不动 —— 全仓库现有
  `#include <radray/render/rhi.h>` 与 `target_link_libraries(... radrayrender)` 保持有效。
- **`rhi.h` 只需移出最少的类, 剩下的一行不动。** 8.13 要把 1280 行的 `rhi.h` 按语义
  切成两半 (那是判断, 风险最高的一步); 本方案只从中摘走 19 个类型, 其余 114 个原地不动。
- **依赖方向反而更诚实。** 8.13 让 shader 与 gpu 平级、都依赖 gal, 但实测 device 侧
  重度使用这些类型 (`ShaderParameterBindingType` 153 处、`VertexFormat` 130 处),
  说明它们本就是 render 的**下层**而非旁支。让 render 依赖 shader 更贴合事实。

**一, 必须从 `rhi.h` 移入 `radrayshader` 的最小闭包 = 19 个类型 (已实测)。**

方法: 以格式层与编译器实际用到的类型为种子, 在 `rhi.h` 内做传递闭包
(枚举成员不算类型依赖 —— 这点很关键, 见下面的坑)。结果**恰好收敛于 19 个, 不含任何
device class**:

| 类别 | 成员 |
|---|---|
| 枚举 (9) | `RenderBackend`、`ShaderStage`、`ShaderBlobCategory`、`ShaderParameterBindingType`、`VertexFormat`、`VertexStepMode`、`AddressMode`、`FilterMode`、`CompareFunction` |
| 描述 struct (10) | `SamplerDescriptor`、`ShaderDescriptor`、`ShaderBindingLocation`、`ShaderParameterSetLayoutEntryDescriptor`、`ShaderParameterSetLayoutDescriptor`、`PushConstantDescriptor`、`PipelineLayoutDescriptor`、`VertexAttribute`、`VertexBufferLayout`、`VertexInputState` |

连带移入: `is_flags<ShaderStage>` 特化与 `using ShaderStages = EnumFlags<ShaderStage>`
(`rhi.h:434/462`), 以及 4 个自由函数 —— `GetVertexFormatSizeInBytes`、
`IsDynamicShaderParameterBindingType`、`format_as(ShaderStage)`、
`format_as(ShaderBlobCategory)`; `format_as(VertexFormat)` / `format_as(RenderBackend)`
一并带走 (同属这批枚举)。

**留在 `rhi.h`**: 其余 26 个枚举、全部 device class、`RenderBase` / `IDebugName`、
`SamplerCache`、`std::hash<SamplerDescriptor>` (它服务于 `SamplerCache`, 属 render)、
以及全部持 device 指针的描述 (`ShaderEntry`、`GraphicsPipelineStateDescriptor`、
`FramebufferDescriptor` ...)。**PSO 固定功能状态 (`PrimitiveState` /
`DepthStencilState` / `MultiSampleState` / `ColorTargetState`) 也留在 `rhi.h`** ——
8.13 曾把它们划进 gal, 但本布局下格式层不需要它们 (闭包里没有), 不动即最小改动。

【坑: 闭包计算必须排除枚举成员】首次计算得到 110/133 个类型的"闭包", 险些据此判定
方案不可行。原因是 `ShaderParameterBindingType` 的成员名叫 `Buffer` / `Texture` /
`Sampler`, 与 device class 同名, 正则把枚举**成员**当成了对类型的引用, 于是
`ShaderParameterBindingType → Buffer → Device → ...` 一路污染到全表。排除枚举体后
闭包立刻收敛到 19。**这是本次设计中最容易出错的一步, 记录以防重犯。**

**二, 三个方向的依赖已实测无环。**

- **编译器不依赖 device**: `dxc.cpp`/`hlsl.cpp`/`spirv.cpp`/`spvc.cpp`/`msl.cpp` 对
  `Device`/`SwapChain`/`CommandQueue`/`backend/` 零引用 (8.12 已核实)。头文件侧
  `dxc.h` 仅需 `ShaderStage` + `ShaderBlobCategory`, `spvc.h` 仅需 `ShaderStage`,
  `hlsl.h`/`spirv.h`/`msl.h` **完全不 include `rhi.h`**。
  (`msl.h`/`spirv.h` 里出现的 `Texture`/`Sampler`/`Buffer` 是它们自己枚举的成员名,
  与 RHI 同名但无关 —— 同上一条那个坑。)
- **device 侧不依赖编译器**: `rhi.cpp` / `d3d12/*` / `vk/*` / `backend/*.h` 对五个
  编译器头**零 include**。故 `radrayrender` 依赖 `radrayshader` 只用到那 19 个类型,
  不会反向拖入 DXC。
- **格式层不依赖 device**: 8.9 已核实 (`render::Device` 只出现在一句注释里)。

**三, 决定性实测: CLI 链接闭合且不含任何后端。**

用真实 obj 模拟最终布局链接 `radray_shader_cook`: 给 `shader_cook.obj` + 五个编译器
obj + `shader_manifest.obj` + `shader_reflection_map.obj` + core/第三方 lib,
**不给 `rhi.cpp.obj`、不给任何后端 lib**。

结果: **恰好 4 个未解析符号, 全部是第一节那 4 个自由函数, 零个后端符号。**
→ 这 4 个函数随 19 个类型迁入 `radrayshader` 后链接即闭合。
→ 对照实验 (保留 `rhi.cpp.obj`) 会多出 5 个后端工厂符号
(`d3d12::CreateDevice`、`vulkan::CreateDeviceVulkan` 等), 从而拖入约 23 MB 后端 obj。
**这证明"把那 4 个函数的定义搬进 shader 库的 TU"是成败关键**, 与 8.14 的结论一致。

**四, 目录布局 (改动量最小)**

```
modules/shader/include/radray/shader/
    shader_types.h        ← 从 rhi.h 摘出的 19 个类型 + ShaderStages + 4 个函数声明
    dxc.h hlsl.h spirv.h spvc.h msl.h          ← 自 modules/render 平移
    shader_manifest.h shader_asset_template.h  ← 自 modules/runtime 平移
modules/shader/src/
    shader_types.cpp      ← 4 个自由函数 + format_as 的定义 (自 rhi.cpp 摘出)
    dxc.cpp hlsl.cpp spirv.cpp spvc.cpp msl.cpp
    d3d12shader.h d3dcommon_adapter.h          ← 跟 dxc.cpp 走 (DXC 反射的非 Win 适配层)
    shader_manifest.cpp shader_asset_template.cpp
    shader_manifest_json.h shader_reflection_map.h/.cpp
modules/shader/tests/
    test_shader_asset.cpp test_shader_asset_template.cpp
```

`rhi.h` 顶部加 `#include <radray/shader/shader_types.h>`。**于是全仓库现有的
`#include <radray/render/rhi.h>` 与 `render::` 限定全部继续有效, 无需批量重写** ——
这是本布局相对 8.13 最大的改动量优势 (8.13 第 2 步要改 26 个 includer)。

`shader_program.*` / `shader_asset.*` / `pipeline_state_cache.*` **留在 runtime**
(8.9 裁决, 不受影响 —— `ShaderPassProgram` 持有活的 `PipelineLayout`)。

**五, 执行顺序** (每步独立可编、可全量 ctest)

1. 建 `modules/shader/` + `RADRAY_BUILD_SHADER` option; 从 `rhi.h`/`rhi.cpp` 摘出 19 个
   类型与 4 个函数到 `shader_types.h`/`.cpp`; `rhi.h` include 之; `radrayrender` 链
   `radrayshader`。**此步不改任何调用方**, 全仓库应照常编过。
   (此步即修掉 G14: 那 4 个函数已不在 `rhi.cpp.obj` 里。)
2. 编译器五组 + 两个私有头自 `modules/render` 平移到 `modules/shader`;
   include 路径 `radray/render/dxc.h` → `radray/shader/dxc.h` (7 个 includer, 已列表)。
   把 DXC/spirv-cross 相关的 `target_link_libraries` 与
   `RADRAY_ENABLE_DXC`/`RADRAY_ENABLE_SPIRV_CROSS` 定义从 render 移到 shader。
3. 格式层四组自 `modules/runtime` 平移; include 改 `radray/shader/...`;
   搬 `RADRAY_ENABLE_SHADER_JIT` + `RADRAY_DXC_VERSION` (含 `FATAL_ERROR`) 与
   shaderlib POST_BUILD; 改 `CMakeLists.txt:60` 的 option 条件为
   `RADRAY_BUILD_SHADER AND RADRAY_ENABLE_DXC`。
4. 两个 CLI: `target_link_libraries` 改 `radrayshader`, `add_dependencies` 重指;
   改 `examples/sphere_demo` 的 include。
5. 搬两个格式层测试到 `modules/shader/tests/` (含 `RADRAY_PROJECT_DIR_DEFAULT` 那段)。
6. 加 8.11 第三点的守门配置 (只构 core + shader + CLI, 不构 render/runtime)。

**六, 验收标准**

- 用例总数仍 **360**, 按 8.8 做 exe 自报 ↔ ctest **双向集合比较**。
- **主门槛**: 两个 CLI 的 `link /MAP` 产物**不含** `d3d12_*.obj` / `vulkan_*.obj` /
  `D3D12MemAlloc*.obj` / `rhi.cpp.obj`。(不用 `dumpbin /DEPENDENTS` 作主判据 ——
  8.14 实测 Vulkan 后端被链入却不留 dll 导入项。)
- 辅助: `dumpbin /DEPENDENTS` 里 `d3d12.dll` / `dxgi.dll` 消失; exe 体积从 12.7MB 下降。
- `radrayshader` 的链接闭包**不含 `radrayrender`** (方向正确性)。

**七, 保留的风险**

- 无跨平台 CI, macOS/Linux 上的收益仍只能靠推理 (8.11/8.12 已记)。第 6 步的守门配置
  能在 Windows 上部分替代。
- 第 1 步仍是"判断"而非纯搬运 (要决定哪 19 个类型走), 但范围比 8.13 小一个数量级,
  且已用闭包实测把判断变成可复算的结果。

---

### 8.16 实施结果: `core ← shader ← render ← runtime` 已落地 (2026-07-28)

8.15 的六步已全部执行完毕。**全量构建通过, ctest 360/360, 双向集合比较无差异。**

**一, 最终布局与实测收益**

```
radraycore ← radrayshader ← radrayrender ← radrayruntime
```

| 指标 | 重构前 | 重构后 |
|---|---|---|
| `radray_shader_cook.exe` | ~33 MB, 链入 8 个 render obj | **9.84 MB, 零 render obj** |
| `dumpbin /DEPENDENTS` | 含 `d3d12.dll` | 仅 CRT (KERNEL32/ADVAPI32/MSVCP140D/...) |
| cook 的 `link /MAP` obj 来源 | render + 后端 + core | `radrayshader` 6 个 + `radraycore` 6 个 |

`link /MAP` 确认 cook 只取: `radrayshader:{dxc,hlsl,spvc,shader_manifest,shader_reflection_map,shader_types}.cpp.obj`
+ `radraycore:{binary_io,dynamic_library,file,json,logger,text_encoding}.cpp.obj`。
**零 `d3d12_*` / `vulkan_*` / `D3D12MemAlloc` / `rhi.cpp.obj`** —— 8.15 第六节主门槛达成。

**二, 转发包含: 零调用方改动的关键**

`rhi.h` 顶部加一行 `#include <radray/shader/shader_types.h>` 后, 全仓库现有的
`#include <radray/render/rhi.h>` 与 `render::` 限定**全部继续有效**, 包括后端里那
153 处 `ShaderParameterBindingType` 与 130 处 `VertexFormat` —— 一处未改。命名空间
保持 `radray::render` 是这一点成立的前提。

**三, 唯一的意外: 传递包含断链**

`shader_program.h` 原先靠 `shader_manifest.h` **传递地**拿到 `rhi.h`。格式层迁入
`radrayshader` 后, 它按 8.4b 只包含 `shader_types.h` (刻意不依赖任何 device 类型),
那条链就断了, 于是 `unique_ptr<PipelineLayout>` 与 `RenderPass` / `PrimitiveState` /
`GraphicsPipelineState` 等一片报错 (`pipeline_state_cache.h` 经
`shader_asset.h → shader_program.h` 连坐)。

修法: `shader_program.h` 显式 `#include <radray/render/rhi.h>`。**一处修好即消解整条
级联** —— 这反过来印证了 8.9 的分界线 (是否拥有活的 GPU 对象) 划得对: 真正需要
`rhi.h` 的只有持 `PipelineLayout` 的那一个头。

**教训**: 拆库会暴露所有隐式的传递包含。头文件本就该显式包含自己用到的东西, 靠下游
头"顺带带进来"在边界移动时必然断裂。

**四, 与 8.15 计划的偏离**

- **第 6 步 (守门配置) 未做**, 转为遗留项。理由: 它是"防回归"而非"完成重构", 且当前
  `RADRAY_BUILD_SHADER=ON / RADRAY_BUILD_RENDER=OFF` 这条组合从未构建过, 引入它需要
  单独验证一轮。已记入下面的遗留风险。
- `format_as(RenderBackend)` / `format_as(VertexFormat)` 一并迁走, 故实际迁移的是
  **6 个函数**而非 8.15 第一节说的 4 个 (那 4 个是"未解析符号"的计数, 不是迁移清单)。
- `RADRAY_ENABLE_SHADER_JIT` 定义与 shaderlib/DXC 部署都挂到了 `radrayshader`:
  部署落点靠 `radray_set_build_path` 把静态库的 `TARGET_FILE_DIR` 收进
  `_build/<Config>/`, **已实测** `shaderlib/` 20 个文件与 `dxcompiler.dll` 就在 exe 旁边。

**五, 遗留风险**

- **无跨平台 CI**, macOS/Linux 收益仍只能靠推理。`APPLE AND RADRAY_ENABLE_VULKAN` 那段
  PUBLIC 链 Cocoa/QuartzCore 留在 `radrayrender`, 因为 CLI 已不链 render, 理论上不再
  波及, 但无从实测。
- **守门配置缺失** (8.11 第三点): 没有任何机制阻止后人给 `radrayshader` 加回
  `radrayrender` 依赖, 或给 CLI 改回链 runtime。两个 CMakeLists 里已就地写明理由与
  `link /MAP` 复验要求, 但注释不是约束。
**六, 顺带定位并修掉了 8.8 的遗留隐患 (CMake 4.4 的 bug)**

8.8 曾把"`gtest_discover_tests` 的 hash 后缀在不同目标间可重复 (`e3b0c442`)"记为遗留
隐患, 并推测那次 ctest 注册污染是一次性陈旧产物。**本轮 `--clean-first` 全量重建时它
稳定复现了**, 于是查到根因:

- `GoogleTest.cmake` 生成 `gtest_discover_tests_impl(...)` 调用时**从不传 `TEST_TARGET`**
  (实测生成出的 `*_discovery.cmake` 里只有 `TEST_EXECUTABLE` / `TEST_LIST` / `CTEST_FILE` 等)。
- 而 `GoogleTestAddTests.cmake:203` 做 `string(SHA256 target_hash "${arg_TEST_TARGET}")`,
  空串的 SHA-256 前缀恒为 **`e3b0c44298`** —— 这正是 `cmake_test_discovery_e3b0c44298.json`
  的来源, 也解释了为何 13 个 core 测试目标共用同一个 `e3b0c442` 后缀。
- CMake 自己在该处的注释写明: 这个 hash 存在的唯一目的就是避免同目录多个目标在
  **POST_BUILD 并行**发现时争用同一个 json。**空输入把这个防护彻底废掉了。**

表现: 随机的 `string sub-command JSON failed parsing json string` 构建失败 (此时 exe
自身 `--gtest_list_tests` 完全正常, 所以极易误判为"陈旧产物"), 更坏的情况就是 8.8 遇到的
**注册到错误用例集却仍报绿**。

修法: `radray_add_test` 改用 `DISCOVERY_MODE PRE_TEST` (`cmake/Utility.cmake`)。CMake
同一段注释确认 PRE_TEST 在 ctest 启动阶段串行执行, 无此竞争。验证: 删除整个 `build_debug`
冷启动重建后, `*_tests.cmake` 与 `cmake_test_discovery_*.json` **均为 0 个** (机制已
不再产生这些文件), ctest 仍 360/360 且双向集合一致。

**七, 最终验收记录 (冷启动全量, 删除 `build_debug` 重建)**

- 构建: 零 error / 零 FAILED。
- ctest: **360/360**; 20 个 test exe 自报 360 ↔ ctest 注册 360, `Compare-Object` 双向无差异。
- 两个 CLI 的链接命令行**不含** `radrayrender.lib` / `radrayruntime.lib`。
- `radray_shader_cook` / `radray_shader_gen` / `test_shader_asset` /
  `test_shader_asset_template` 四个二进制的 `dumpbin /DEPENDENTS` 均无
  `d3d12.dll` / `dxgi.dll` / `vulkan` 导入。
- cook 9.84 MB, gen 9.93 MB (重构前约 33 MB)。

---

### 8.17 分层纠正: 8 个 RHI 交接类型退回 render (2026-07-28)

8.16 落地后复查 `shader_types.h`, 发现 19 个类型里有 8 个**在 shader 库内零消费**。
本节把它们退回 `rhi.h`, 并修正了当初的判定标准。

**一, 原判定标准太宽**

8.15/8.16 用的标准是"不含任何 device 对象指针"。这个标准能挡住 device class, 但挡不住
纯粹的 RHI 入参形状 —— `PipelineLayoutDescriptor` / `VertexInputState` /
`ShaderDescriptor` 同样不含 device 指针, 却完全是 render 的词汇。按它筛选的结果是
shader 库承载了 8 个自己从不使用的类型。

**正确的问题是"这个类型是 manifest 的数据吗"** —— 它出现在 `*.shader.json` 里吗?
有 JSON codec 吗? 格式层解析它吗?

**二, 实测的两类划分**

| 类别 | 成员 | 依据 |
|---|---|---|
| **manifest 数据词汇** (留 shader, 11 个) | `ShaderStage`、`ShaderBlobCategory`、`ShaderParameterBindingType`、`VertexFormat`、`VertexStepMode`、`ShaderBindingLocation`、`SamplerDescriptor`、`AddressMode`、`FilterMode`、`CompareFunction`、`RenderBackend` | 都能在 manifest 里找到对应字段或 codec |
| **RHI 入参形状** (退回 render, 8 个) | `ShaderDescriptor`、`PipelineLayoutDescriptor`、`ShaderParameterSetLayoutDescriptor`、`ShaderParameterSetLayoutEntryDescriptor`、`PushConstantDescriptor`、`VertexInputState`、`VertexAttribute`、`VertexBufferLayout` | 不进 json、无 codec、shader 层零消费 |

几个值得记的细节:

- `AddressMode` / `CompareFunction` 在 shader 层**零直接引用**, 但它们是
  `SamplerDescriptor` 的成员, 而 `SamplerDescriptor` 有 JSON codec
  (`shader_manifest.cpp` 序列化 `AddressS`/`MinFilter`/`Compare` 字段)。
  **零引用不等于非依赖** —— 间接被序列化也是真实依赖。
- `RenderBackend` 是这批里唯一的"边界翻译入参": 它只服务
  `GetShaderBlobCategoryForBackend` (D3D12→DXIL / Vulkan→SPIRV / Metal→MSL)。cook 必须
  在没有任何 device 的前提下为目标平台选字节码类型, 所以它在这里的角色是**目标平台
  标识**而非设备句柄。名字比实际语义重, 但另造一个一一对应的平行枚举是纯重复。
- `ShaderBindingLocation` 留下, 因为它是 `ShaderPushConstantDesc::Location` 的字段且
  有 codec; 而消费它的 `PushConstantDescriptor` 走了。两者不同层, 这是正常的。

**三, 承接者: `modules/render/{include/radray/render,src}/shader_layout_binding.h/.cpp`**

随 8 个类型一起搬走的是它们在 shader 层的唯一使用者:

- `ShaderPipelineLayoutStorage` / `ShaderVertexInputStorage` (两个 Storage 类)
- `BuildPipelineLayoutStorage` / `BuildVertexInputStorage`
- `ResolveBindingType` (原 `shader_manifest.cpp` 匿名 namespace, 折叠
  Type + Residency → `Dynamic*`)
- `ShaderBytecode::MakeDescriptor()` → 自由函数 `MakeShaderDescriptor(const ShaderBytecode&)`

`ShaderBytecode` 本身留在 shader 层并变回**纯数据**(不再有成员函数返回 RHI 描述)。
新文件同时 include `rhi.h` 与 `shader_manifest.h` —— 这正是 render 依赖 shader 的正确
用法: 上层看得见下层。

**四, 测试 fixtures 抽成共享头 (避免静默漂移)**

6 个用例 (5 个 layout 构建 + 1 个新增的 `MakeShaderDescriptor`) 迁到新建的
`modules/render/tests/test_shader_layout_binding.cpp`, 只链 `radrayrender`, **不需要
device**。

它们与 `test_shader_asset.cpp` 共用同一批 manifest 正例, 于是把
`kImGuiManifest` / `kForwardManifest` / `kMinimalManifest` 抽到
`modules/shader/tests/shader_manifest_fixtures.h`。**刻意不复制**: 复制会让两边悄悄
漂移 —— 改了一处 binding 声明另一处仍在断言旧值, 且不会有任何编译错误提示。

原 `ShaderResolverTest.DescriptorIsReadyForCreateShader` 保留在 shader 层但改为断言
`ShaderBytecode` 自身就绪 (它本就在验证 resolver); 打包成 descriptor 的部分由新用例
`MakeShaderDescriptorForwardsBytecodeFields` 覆盖, 后者用手工构造的 `ShaderBytecode`,
不必再跑一遍 DXC。

**五, 验收 (删除 build_debug 冷启动)**

- 构建零 error。ctest **361/361**; 21 个 exe 自报 361 ↔ ctest 361, 双向无差异。
  (360 → 361: 迁出 5 个 + 新增 1 个 `MakeShaderDescriptor` 用例 + 原
  `DescriptorIsReadyForCreateShader` 保留。)
- **8 个类型在 `modules/shader/` 内引用数归零** (仅剩 2 处注释提及)。
- 两个 CLI 仍不链 `radrayrender`/`radrayruntime`; cook / gen / test_shader_asset 三者
  `dumpbin /DEPENDENTS` 均无 `d3d12.dll` / `dxgi.dll`。
- cook 9.84 MB → **9.77 MB** (打包逻辑离开 shader 库的净效果)。

**六, 教训**

分层判据要用"这个类型属于谁的词汇", 而不是"这个类型碰不碰某个具体的实现细节"。
后者是**必要条件而非充分条件**: 它能证明"不该在更下层", 却不能证明"应该在这一层"。

**七, 承接位置的实际落点 (与上文四处描述不符, 以此处为准)**

上文写的新建文件 `modules/render/include/radray/render/shader_layout_binding.h` 最终**没有
单独建立**。8 个类型与 4 个函数实际落在 `modules/runtime/include/radray/runtime/shader_program.h`
(声明) 与 `modules/runtime/src/shader_program.cpp` (定义), 对应测试在
`modules/runtime/tests/test_shader_layout_binding.cpp`, 链 `radrayruntime`。

这不影响 8.17 的核心结论 —— 这批类型离开了 `radrayshader`, cook / gen 依旧零后端。但它
把交接层放在了 runtime 而非 render。**若日后 render 层自己需要 manifest → RHI 的打包**
(目前只有 runtime 需要), 应把这批代码下移到 render 并补建 `modules/render/tests`。

### 8.18 `RenderBackend` 退回 render (2026-07-28)

**动机**: 8.17 之后 `shader_types.h` 仍留着 `RenderBackend`, 而它描述的是"用哪个图形 API
跑", 显然不是 manifest 的内容 —— 按 8.17 确立的判据就不该在 shader 层。

**它当初为何被带进来**: 只因为一个函数 `GetShaderBlobCategoryForBackend(RenderBackend)
→ ShaderBlobCategory` 声明在 `shader_manifest.h`。

**调查结论 (决定改法的关键)**: 该函数在**生产代码中零调用**, 唯一引用是
`test_shader_asset.cpp` 的三行断言。生产代码里 backend → category 的实际做法是各调用点
直接给出 `ShaderBlobCategory`:

- `tools/shader_cook/shader_cook.cpp` 由命令行参数直接映射
- `runtime/shader_program.h` 的 `Category` 默认 `DXIL`
- `PipelineStateCache::GetOrCreateGraphics` / `ShaderProgram::GetOrCreateVariant` 都是
  **收 `ShaderBlobCategory` 入参**, 一路由调用方传入

也就是说 shader 层从设计上就只认 `ShaderBlobCategory`, 从不需要 `RenderBackend`。

**改动**:

| 内容 | 从 | 到 |
| --- | --- | --- |
| `enum class RenderBackend` | `shader/shader_types.h` | `render/rhi.h` |
| `format_as(RenderBackend)` | `shader/shader_types.cpp` | `render/rhi.cpp` |
| `GetShaderBlobCategoryForBackend` | `shader/shader_manifest.{h,cpp}` | `render/rhi.{h,cpp}` |
| 3 行断言 | `shader/tests/test_shader_asset.cpp` | `render/tests/test_rhi_types.cpp` (新建) |

函数**保留而非删除**: 它本身是正确的领域知识 (D3D12→DXIL / Vulkan→SPIRV / Metal→MSL),
只是放错了层。放在 `rhi.h` 后, 两个参数类型都是 render 自己的词汇。

顺带补建了 `modules/render/tests/CMakeLists.txt` —— 它此前是个 **0 字节空文件**
(`modules/render/CMakeLists.txt:45` 已经在 `add_subdirectory(tests)`, 只是没有内容),
所以 render 层长期没有任何测试。新增用例除映射表外, 还断言
`format_as(MAX_COUNT) == "UNKNOWN"` (哨兵不该把成员名泄漏进日志)。

**验收**:

- `shader_types.h` 的 manifest 词汇 11 项 → **10 项**。
- `modules/shader/` 与两个 CLI 内 `RenderBackend` 引用归零 (仅剩 2 处解释性注释)。
- ctest **362/362** (361 − 1 迁出 + 2 新增); 22 个 exe 自报 362 ↔ ctest 362,
  双向 `Compare-Object` 无差异。
- `ninja -t commands` 显示 cook 链接行仍只有 `radrayshader.lib` + `radraycore.lib`,
  **无 `radrayrender.lib`**。cook 9.77 MB / gen 9.86 MB 不变。
  (用链接命令行而非 `dumpbin /DEPENDENTS` 判定 —— 后者对 volk 加载的 Vulkan 无效。)

**教训**: 一个类型被放进下层, 有时不是因为下层需要它, 而是因为**某个便利函数**恰好写在了
下层。判断依赖时要问"删掉这个函数后, 本层还需要这个类型吗"。这里的答案是不需要, 而且那个
函数连生产代码都没用过。

### 8.19 `ShaderResolveContext` 拆分: 策略唯一真相 + 文件级源码缓存 (2026-07-28)

**动机**: 原 `ShaderResolveConfig` 把三件不同生命周期的东西塞在一起 —— 进程级策略
(`ShaderRoot` / `Staleness` / `AllowJit`)、每 manifest 一份的 `ManifestPath`、以及借用的
`Dxc*`。后果是每个 `ShaderAsset` 各自复制一份策略 (多处真相), 且**源码读取与 include 闭包
扫描无法跨 manifest 复用** —— 两个 manifest 引用同一个 `.hlsli`, 就要各扫一遍。

**改动形状**:

| 类型 | 职责 | 生命周期 |
| --- | --- | --- |
| `ShaderResolveSettings` | 纯数据: `ShaderRoot` / `Staleness` / `AllowJit` | 值语义 |
| `ShaderResolveContext` | 持策略 + 借用 `Dxc*` + **文件级源码缓存** | 全进程一份 |
| `ShaderResolver` | `(context&, manifestPath)`, 只管自己那份 manifest | 每 manifest |

`ManifestPath` 从策略里移出, 成为 resolver 的构造参数 —— 它本来就不是策略。
`ShaderAssetLoadOptions` 随之从 4 个字段收窄为只剩 `.Context`。

**缓存与失效**: `GetFile` 每次检出文件时间戳, 变化则删该条并 `_closures.clear()`
(闭包是跨文件的传递结果, 无法精确失效单条, 全清是唯一正确解); 文件消失则返回 nullptr。
`ShaderSourceCacheStats{FileReads, IncludeScans, IdentityComputes}` 用于在测试里断言
"第二个 asset 复用了第一个的读取"。

**生命周期不变量** (靠声明顺序固定, 不靠注释): `RenderSystem` 内
`_shaderResolveContext` 声明在 `_dxc` **之后**, 逆序析构保证 dxc 后死; context 必须比
resolver / `ShaderAsset` 活得久。

**新增测试 7 个** (`test_shader_asset.cpp` 5 + `test_shader_program.cpp` 2), 全部针对
缓存路径 —— 因为缓存是**第二套闭包遍历实现**, 与 `ComputeShaderSourceIdentity` 的无缓存
路径并存, 两者必须逐输入等价:

- `CachedIdentityMatchesTheUncachedFunction` / `CachedIdentityRejectsTheSameInputsAsTheUncachedFunction`
  —— 正反两侧都比对 (相同输入同结果, 相同坏输入同拒绝: missing / dangling include /
  macro-based include / 逃出 root)
- `CachedIdentityHandlesDiamondAndCyclicIncludes` —— 菱形不重复读, 环不死循环
- `DeletingAHeaderInvalidatesTheCachedClosure` —— 失效路径
- `TwoAssetsShareOneSourceCache` —— 跨 manifest 复用 (拆分的正当性本身)
- `MissingResolveContextFails` —— `ShaderAssetLoadOptions{}` 缺 context 时报错含 `"Context"`

**验收**: 干净构建 0 error; ctest **373/373** (362 + 4 提交内 + 7 本轮);
22 个 exe 自报 373 ↔ ctest 373, 双向 `Compare-Object` 无差异。

### 8.20 修正 8.x 的一处误判: fmt `/scanDependencies` 不是无害噪音 (2026-07-28)

前几轮把构建输出里的
`clang-cl : error : no such file or directory: '/scanDependencies' [fmt.vcxproj]`
记为"既存无害噪音, 非本次改动引入"。**这个判断是错的**, 干净构建时它是硬失败。

**真实机制** (逐环节已验证):

1. `third_party/fmt/CMakeLists.txt:1` 是 `cmake_minimum_required(VERSION 3.8...3.28)`,
   上界 3.28 使 **CMP0155 在该子目录取 OLD**。
2. CMake 4.4 的 Visual Studio 生成器据此对声明了 C++20 的目标写出
   `<ScanSourceForModuleDependencies>true</ScanSourceForModuleDependencies>`
   (实测: `fmt.vcxproj` 有, `radraycore.vcxproj` 是 false)。
3. MSBuild 于是给 **ClangCL** 传 `/scanDependencies`, 而 clang-cl 不认识该开关。
4. 结果 `format.obj` **根本不生成**, 紧接着 `llvm-lib` 以 `MSB6006` 失败。

**为什么被误判为无害**: 长期存活的 `build_debug` 里已有旧工具链产出的 `fmt.lib`,
增量构建跳过归档步骤, 错误只在日志里刷屏却不阻断。删掉 build 目录后立刻变成硬失败 ——
潜伏破坏被增量状态掩盖了。受影响的不止 fmt: `fmt-c` / `freetype` / `png_static` /
`yyjson` / `zlibstatic` 共 6 个 vcxproj 都带该标志。

**修法** (根 `CMakeLists.txt`, 不动 `third_party/`):
在任何 `add_subdirectory` **之前**设 `set(CMAKE_CXX_SCAN_FOR_MODULES OFF)`。位置是关键 ——
放晚了子目录已经继承了 ON。本项目 `FMT_MODULE` 强制 OFF 且全仓无 `CXX_MODULES` file set,
不使用 C++20 modules, 扫描纯属开销。

**验收**: 重新 configure 后**无任何** vcxproj 带 `ScanSourceForModuleDependencies=true`;
ClangCL 干净构建 0 error, ctest 373/373; 另在临时目录用**纯 MSVC 工具集**
(`Visual Studio 18 2026` + `-A x64`) configure + 构建 `fmt`/`fmt-c`/`radraycore`/
`radrayshader` 同样 0 error, 确认不是把问题从一个工具链挪到另一个。
(注意两个 preset 共用 `binaryDir: build_debug`, 验证 MSVC 必须另指目录, 否则会冲掉
ClangCL 构建树。)

顺带复现了 8.8 的僵尸产物问题: `build_debug/_build/Debug/test_shader_resolver.exe`
(7-26 产出, 拆库前的目标, 已无源文件也无 CMakeLists 引用) 仍被 exe 扫描算作一个用例集,
使 exe 自报 374 ↔ ctest 373。干净构建后消失。**这印证 8.8 的教训应升级**: 凡以用例集合
做验收, 除双向比较外还要留意 build 树里的陈旧 exe —— 双向比较能发现它, 但只有干净构建
能消除它。

**教训**: "既存的、一直在刷的错误"不等于"无害的"。增量构建会把硬失败伪装成噪音,
判定构建健康度必须至少做一次干净构建 —— 这也是本轮唯一一次真正暴露该问题的操作。
