> - 适用: 在 `shaderlib/` 里找现成实现；加新的 HLSL 库文件；理解绑定 ABI
> - 权威: 本文是 HLSL 库分层与绑定 ABI 的唯一说明。作者侧操作流程见 `guide/shader-authoring.md`
> - 锚点: `shaderlib/core/platform.hlsli`, `shaderlib/forward_pipeline/bindings.hlsli`, `shaderlib/forward_pipeline/view.hlsli`

# shaderlib

`shaderlib/` 既是 HLSL 源码树，也是 include 根。全部 include 都相对它解析，尖括号形式。

`RADRAY_ENABLE_SHADER_JIT` 下 `radrayshader` 的 POST_BUILD 会把整个目录拷到
`$<TARGET_FILE_DIR:radrayshader>/shaderlib`（先 `rm -rf`）。**运行时与 CLI 读的是那份副本**。

## 分层

下层不得 include 上层。

| 层 | 内容 | 不含 |
|---|---|---|
| `core/` | 后端 shim、数学、着色帧、色彩 | 任何光照或材质语义 |
| `bsdf/` | fresnel、微表面分布、Principled BSDF | 光源、贴图 |
| `lighting/` | 光源 GPU 布局与辐照度求值 | 阴影 |
| `shadow/` | 共享过滤原语 + 每技术一文件 | 管线绑定 |
| `forward_pipeline/`, `imgui/` | 管线专属绑定与入口点 | — |

## core/

| 文件 | 内容 |
|---|---|
| `platform.hlsli` | **跨后端差异的唯一收口处**。`VK_LOCATION` / `VK_BINDING` / `VK_PUSH_CONSTANT` / `VK_IMAGE_FORMAT` 在 SPIR-V 与 Metal 下展开为标注，在 DXIL 下展开为空 |
| `math.hlsli` | 与渲染无关的标量数学：`RADRAY_PI`、`safe_rcp`、`pow2/4/5` |
| `frame.hlsli` | 局部着色帧（n = +Z），无分支 ONB（`make_frame`）与切线对齐变体 |
| `color.hlsli` | 色彩空间转换与色调映射（linear ↔ sRGB） |

`platform.hlsli` 存在的理由：DXC 编到 SPIR-V/Metal 时需要显式 location/binding/push_constant
标注，编到 DXIL 时这些标注非法。成套提供后 shader 只写一份声明即可两边通吃。
换后端时只需要读这一个文件。

## bsdf/

| 文件 | 内容 |
|---|---|
| `fresnel.hlsli` | 介电 Fresnel，带折射几何输出参数 |
| `microfacet.hlsli` | GGX 的 D/G 项、`roughness_to_alpha` |
| `principled.hlsli` | Disney Principled BRDF 反射半球，逐项对齐 Mitsuba3 的 `principled.cpp` |

全部在局部着色帧内求值（n = +Z）。

**方向约定**（`principled.hlsli`）：`wi` = 朝相机，`wo` = 朝光源（Mitsuba 记法）。
返回值**已包含余弦投影**，调用方不要再乘 N·L。

## lighting/

`lights.hlsli` — 光源 GPU 布局（CPU/GPU 共享的 ABI）与辐照度求值。
上限 `RADRAY_MAX_DIRECTIONAL_LIGHTS = 8`、`RADRAY_MAX_POINT_LIGHTS = 8`。

## shadow/

| 文件 | 内容 |
|---|---|
| `filtering.hlsli` | 共享原语：世界→阴影 uv、bias、PCF 核。`RADRAY_SHADOW_FILTER_*` 的**取值是序列化 ABI** |
| `cascade.hlsli` | 方向光 CSM。`CascadeShadow` 结构，`RADRAY_MAX_CASCADES = 4` |
| `cube.hlsli` | 点光源立方体阴影。`CubeShadow`，6 个面矩阵，`RADRAY_CUBE_FACE_COUNT` |

两个技术都参考 UE5（CSM 用 practical split + texel snapping 稳定化；cube 用
OnePassPointLightShadow 风格），但**用引擎的标准深度约定**（clear = 1.0，比较 `LessEqual`），
不是 UE5 的 reverse-Z。

结构定长，可整块塞进普通 cbuffer，不需要 `StructuredBuffer`。字段**须与 CPU 端逐字段对齐**，
列主序。cube 的面序必须与 CPU 端一致。

## forward_pipeline/

### 绑定 ABI

`bindings.hlsli` 是 **D3D register / Vulkan binding 编号的唯一定义处**。编号必须与 CPU 端
forward 管线的 binding group 分配一致；散落的字面量无法被编译器检查出与 group 编号的分歧。

| group | 用途 | 更新频率 |
|---|---|---|
| 0 | per-object | 每 draw |
| 1 | per-view | 每视图，由管线提供 |
| 2 | per-material | 材质持久绑定，由各 pass 按需声明 |

group 号同时是 D3D12 的 `RegisterSpace` 与 Vulkan 的 set index。这是后端已硬化的不变量，
manifest 不做重映射。

宏把 slot 与 binding 分开传，因为 D3D 的 t/s/b 各自独立编号，而 Vulkan 的 binding
在一个 set 内**统一编号**。所以 manifest 里 b/t/s/u 共用一个编号空间。

`ObjectConstants`（b1, space0）定义在 `bindings.hlsli` 而非各 pass 内部：所有 forward 系 pass
共用同一份布局与同一个槽，两份独立定义的布局分歧编译器查不出来。

### 文件

| 文件 | 内容 |
|---|---|
| `bindings.hlsli` | 上述 ABI 与 `RADRAY_FORWARD_*` 宏 |
| `view.hlsli` | per-view 数据（`ViewConstants`：相机、光源列表、`CubeShadow`、`CascadeShadow`）+ 阴影绑定的 keyword 声明 |
| `standard_material.hlsli` | glTF metallic-roughness + Principled 材质绑定 |
| `forward_pass.hlsl` | 主前向入口（VSMain/PSMain），声明 6 个 keyword 组 |
| `shadow_pass.hlsl` | 仅深度的阴影投射 pass，`_POINT_SHADOW_LAYERED` keyword |
| `error_pass.hlsl` | 兜底洋红 pass，无材质绑定 |
| `forward_pass.shader.json`, `error_pass.shader.json` | 手工维护的 manifest |

### 两处刻意的"总是声明"

**`view.hlsli` 的 `ViewConstants` 里 `CubeShadow` / `CascadeShadow` 总是存在**，
只有那两个 texture 绑定受 keyword 守护。`_POINT_SHADOWS` / `_DIRECTIONAL_SHADOWS` 的声明
就贴在它们守护的 `#ifdef` 旁边——这是唯一不会失同步的位置，且每个 include 本文件的入口
shader 自动继承这两个变体维度。

**`standard_material.hlsli` 的 5 个 texture 槽 + sampler 总是声明**，keyword 只控制是否采样。
这样 keyword 永不改变描述符 ABI。

`shadow_pass.hlsl` 的 `ViewProj[6]` 同理总是存在，使 layered 与非 layered 共用一个描述符 range。

`ViewConstants::LightCounts` 用 `+1` 编码"投阴影的光源序号"（0 = 无），
省掉一个单独的 flag 字段。

### keyword 与固定功能状态的分界

`forward_pass.hlsl` 是这条分界的活样本：

- `_ALPHATEST_ON` **是** keyword —— 需要 `clip()`，固定功能状态表达不了。
- blend 与 cull **不是** keyword —— 由材质侧 `MaterialRenderState` 表达。shader 无条件写出
  Alpha 并按 `SV_IsFrontFace` 翻转法线，两者在对应的 BlendState / CullMode 下自然失效。

判据三条见 `guide/shader-authoring.md`。

## imgui/

`imgui_pass.hlsl` — Dear ImGui 绘制 pass。同时是 `tools/generate_imgui_shader.py` 的输入，
该脚本把编译结果嵌进运行时，**所以这个文件不能 `#include` 任何东西**。
