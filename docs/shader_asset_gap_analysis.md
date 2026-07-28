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

| 路径 | 行数 | 性质 |
|---|---|---|
| `modules/runtime/include/radray/runtime/shader_manifest.h` | 910 | 公开接口 |
| `modules/runtime/src/shader_manifest.cpp` | 3419 | 全部实现 (未拆分) |
| `modules/runtime/src/shader_asset_json.h` | 20 | private, JSON 字段表唯一实现点 |
| `modules/runtime/src/shader_reflection_map.h` / `.cpp` | 59 / 140 | private, 反射→RHI 映射, validator 与 generator 共用 |
| `modules/runtime/include/radray/runtime/shader_asset_template.h` | 249 | manifest 模板生成器 |
| `modules/runtime/src/shader_asset_template.cpp` | 1155 | 同上 |
| `modules/runtime/tests/test_shader_manifest.cpp` | 4050 | 223 个 TEST |
| `modules/runtime/tests/test_shader_asset_template.cpp` | 903 | |

gtest suite: `ShaderAssetTest`(78) / `ShaderArtifactTest`(60) / `ShaderResolverTest`(35) /
`ShaderBakeSetTest`(26) / `ShaderVariantTest`(22) / `ShaderAssetSampleTest`(2)。

### 1.2 声明与实现完整性

头文件声明的**每个符号都有实现**, 实现文件里**零 TODO / FIXME / XXX**。

已逐项核对: `ShaderVariantDomain` 全部方法 (`shader_manifest.cpp:1619-1833`)、
`GetEffectiveBakeSet`(`:1836`)、`ExpandShaderBakeSet`(`:1842-1958`)、
`ShaderResolver` 全部成员 (`:1976-2310`)、`CookShaderAsset`(`:3276`)、
`CookShaderAssetFile`(`:3390`)、`ValidateShaderReflection` DXIL(`:2510-2598`) 与
SPIRV(`:2600-2691`)、`BuildPipelineLayoutStorage`(`:2445`)、
`BuildVertexInputStorage`(`:2480`)、`ComputeShaderSourceIdentity`(`:2702-2783`)。
18 个 JSON codec 宏声明各有 `Write` + `Read` 定义 (共 36 个)。

### 1.3 已硬化的设计决策

这些是系统坚固性的实际来源:

- **manifest 是唯一 ABI 来源, 反射只做核对**。由此 `BuildPipelineLayoutStorage`
  (`:2445`) 对 target 与 variant 都不变, PipelineLayout 可在编译任何字节码前建好。
  这一条支撑了后面所有分层。
- **内容寻址 blob + stage 投影去重**。`ProjectToStage`(`:1778`) 把无关组一律归
  `kShaderKeywordOff`, 于是「两个变体投影结果相同 ⟺ 共用同一份字节码」成立,
  pixel-only keyword 不污染 VS。测试 `VertexBlobSharedAcrossPixelOnlyVariants`
  (`test_shader_manifest.cpp:3535`) 断言 `Deduplicated == 1`。
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
(`test_shader_manifest.cpp:3501`) 印证。

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
- `BuildPipelineLayoutStorage` 调用点仅测试 (`test_shader_manifest.cpp:345/366/387`)。
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
(`test_shader_manifest.cpp:3558-3573`)。每个未来调用方都要重走一遍, 是会被抄错的地方。

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
  (`test_shader_manifest.cpp` 约 20 处 + `test_shader_asset_template.cpp:772/815`)。
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
- 同步 `forward_pass.shader.json`、`test_shader_manifest.cpp`、`test_shader_asset_template.cpp`。
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
`GTEST_SKIP` 兜底, 抄 `test_shader_manifest.cpp:3977` 的模式。

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

**两个新的设备无关用例** (`test_shader_manifest.cpp`, `ShaderAssetSampleTest`):
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

1. **G1 + G2 + G4 实施** (设计见第 8 节)。顺序: `MakeShaderAssetId` + `ShaderAsset`
   骨架 → `ShaderPassProgram` → `LoadShaderAsset` 工厂 → 切片改用 `ShaderAsset`。
2. **PSO 库** (`RenderSystem` 侧), 条目持 `StreamingAssetRef<ShaderAsset>` 以防 8.5 的
   layout 悬垂。瞬态 `Shader` 在此层内创建。连带检验 5.6 结论 3 的 PSO 缓存 key。
3. **绕序契约归属**。切片用 `CullMode::None` 绕开了 D3D12/Vulkan 绕序相反的问题
   (见 5.6), material/mesh 层必须显式裁决 —— 目前无人负责。
4. G8 vertex input 校验补强、G7 补齐其余 manifest (`shadow_pass`、`imgui_pass`)。
   G5 线程模型已由 8.3 绕开 (加载路径不碰 DXC), 待 material 层触发大批量 JIT 时再看。

**cook 之后新浮现的**: PSO 缓存 key (5.6 结论 3) 仍未检验 —— 切片现在两条路径都走通了,
但都只解析一次, 没有缓存。这条要等 material 层。

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

### 8.5 新浮现: PSO 持 `PipelineLayout` 裸指针会悬垂

两个后端的 PSO 都存了 `_layout` 裸指针 (`d3d12_impl.h:894`、`vulkan_impl.h:1016`), 而
`PipelineLayout` 按 pass 归 `ShaderAsset` 所有。于是 `ShaderAsset` 卸载时, 若 PSO 库里
还有引用其 layout 的 PSO, 即悬垂。

解法: PSO 库条目持一个 `StreamingAssetRef<ShaderAsset>`。引用计数机制已存在, PSO 存活
即钉住 asset, `OnUnload` 不可能在 PSO 之前跑。比在 `RenderSystem` 里做失效通知简单,
也避免了 asset → RenderSystem 的反向依赖。

**本轮 (G1) 不做 PSO 库** —— 只做到 `LoadShaderAsset` 工厂。切片仍自己临时建 `Shader`
与 PSO (它现在就是这么干的), 改动面更小。PSO 库单列一步, 形状参照 `RenderPassRegistry`
(`gpu_resource.h:306`)。
