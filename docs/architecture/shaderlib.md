> - 适用: 在 `shaderlib/` 里找现成实现；增加 HLSL 数学、光照、阴影或产品 pass
> - 权威: 本文是 schema 7 当前 HLSL 共享库边界与 target gate 契约；完整编译契约见 shader pipeline 架构文档
> - 锚点: `shaderlib/core/math.hlsli`, `shaderlib/core/color.hlsli`, `shaderlib/core/frame.hlsli`, `shaderlib/core/platform.hlsli`, `shaderlib/bsdf/principled.hlsli`, `shaderlib/lighting/lights.hlsli`, `shaderlib/shadow/filtering.hlsli`, `shaderlib/pipelines/forward/bindings.hlsli`, `shaderlib/pipelines/forward/forward.hlsl`, `shaderlib/pipelines/forward/depth_only.hlsl`, `modules/shader_compiler/tests/data/depth.hlsl`, `modules/shader_compiler/tests/data/compute.hlsl`

# shaderlib

`shaderlib/` 是 HLSL 源码树，也是 compiler include 解析使用的逻辑根目录。共享数学、材质、
光照和阴影原语位于库层；具体渲染管线的产品 source 位于 `pipelines/`。
compiler discovery 从根 `.hlsl` 推导 entry topology 和 keyword domain；
作者不维护 sidecar metadata，active binding 与 type tree 随 concrete target lane artifact 生成。

测试专用 shader 随所属模块放在 `modules/<module>/tests/`，不放入 `shaderlib/`。
shader compiler 的最小 depth/compute 测试 source 位于 `modules/shader_compiler/tests/data/`；
测试以仓库相对路径读取它们，共享 include root 仍是 `shaderlib/`。

示例专用 shader 随示例维护，不放入 `shaderlib/`。Tidal Atrium 的 surface、sky、panel、HUD
与 signal compute source 位于 `examples/example_tidal_atrium/shaders/`；该示例以仓库根作为
`ShaderSourceRoot`，以仓库相对路径请求这些 source 和共享的
`shaderlib/pipelines/forward/depth_only.hlsl`，`ShaderIncludePaths` 仍指向 `shaderlib/`。

## 分层

下层不得 include 上层。

| 层 | 内容 | 约束 |
|---|---|---|
| `core/` | 标量数学、局部 frame、颜色转换 | 不包含光源、材质或后端绑定 |
| `bsdf/` | Fresnel、微表面分布、Principled BSDF | 不包含光源和资源绑定 |
| `lighting/` | 光源布局与辐照度求值 | 不包含 entry pass |
| `shadow/` | 过滤、级联和 cube shadow 原语 | 不包含 pipeline binding |
| `pipelines/` | 具体 pipeline 的 binding ABI 与产品 pass | group 语义只在所属 pipeline 内有效 |

## 文件导航

| 文件 | 内容 |
|---|---|
| `core/math.hlsli` | `RADRAY_PI`、倒数和幂函数等标量数学 |
| `core/frame.hlsli` | 局部着色帧、ONB 和切线对齐 |
| `core/color.hlsli` | linear/sRGB 转换与色调映射 |
| `bsdf/fresnel.hlsli` | 介电 Fresnel 与折射几何辅助量 |
| `bsdf/microfacet.hlsli` | GGX 的 D/G 项和 roughness 转换 |
| `bsdf/principled.hlsli` | Principled BRDF 反射半球求值 |
| `lighting/lights.hlsli` | CPU/GPU 共享的光源布局与辐照度 |
| `shadow/filtering.hlsli` | 阴影 UV、bias、PCF 原语 |
| `shadow/cascade.hlsli` | 方向光级联阴影数据与求值 |
| `shadow/cube.hlsli` | 点光源 cube shadow 数据与面序 |
| `core/platform.hlsli` | `VK_LOCATION`、`VK_BINDING`、`VK_PUSH_CONSTANT`、`VK_IMAGE_FORMAT` target gate |
| `pipelines/forward/bindings.hlsli` | forward 的 view/material/object binding ABI |
| `pipelines/forward/forward.hlsl` | 纹理 Lambert 光照与颜色转换产品 pass |
| `pipelines/forward/depth_only.hlsl` | Forward 的 depth-only 产品 pass，执行 view/object 变换 |
| `pipelines/forward/pbr.hlsl`, `pipelines/forward/surface.hlsli` | HDR Forward 材质、GGX 光照与 cutout 契约 |
| `pipelines/forward/depth_normals_motion.hlsl`, `pipelines/forward/shadow_caster.hlsl` | 深度/法线/刚体 motion 与阴影投影 |
| `pipelines/forward/tile_lights.hlsl`, `pipelines/forward/local_light.hlsli` | tile 局部灯列表与溢出完整遍历 |
| `pipelines/forward/linear_depth.hlsl`, `pipelines/forward/depth_pyramid.hlsl` | 线性深度与逐 mip 归约 |
| `pipelines/forward/ambient_occlusion.hlsl`, `pipelines/forward/ao_blur.hlsl` | 半分辨率 AO 与 bilateral 滤波 |
| `pipelines/forward/temporal_resolve.hlsl`, `pipelines/forward/sky.hlsl` | opaque/sky 时域重投影 |
| `pipelines/forward/firefly_update.hlsl`, `pipelines/forward/firefly_draw.hlsl` | Compute 生成间接参数与发光粒子绘制 |
| `pipelines/forward/bloom.hlsl`, `pipelines/forward/output.hlsl`, `pipelines/forward/debug.hlsl` | Bloom、曝光/tone map、SDR 合成与调试显示 |

## 编码约定

所有 include 使用 shaderlib-root-relative 的尖括号路径，例如：

```hlsl
#include <core/math.hlsli>
#include <bsdf/principled.hlsli>
```

共享结构的字段顺序、对齐和矩阵约定是 shader ABI；compiler 为每个 active CBuffer declaration
发布指向当前 target-lane payload root 的 owner，runtime 从该 root 按成员名逐字段打包，不按 type
发射顺序猜测，也不写 CPU mirror struct 或 `offsetof` 断言。`shadow/filtering.hlsli` 中的序列化枚举值
和 `lights.hlsli` 中的上限属于 ABI，不能因为重命名或排版而改变。

`core/platform.hlsli` 只提供 DXIL/SPIR-V target gate：`VK_LOCATION`、`VK_BINDING` 和
`VK_PUSH_CONSTANT` 在 SPIR-V lane 展开为属性，在 DXIL lane 展开为空。它不分配编号，
也不提供把 group/slot 打包成单一宏的 numbered wrapper。ordinary resource同时写`register()`与
`VK_BINDING`；push declaration同时写`register()`与`VK_PUSH_CONSTANT`，不得再写`VK_BINDING`。
两target数字不要求相等，compiler按canonical declaration identity关联。

`[RootSignature]`不是`platform.hlsli`的另一套numbering gate，而是compiler-owned跨target base policy：
DXIL保留serialized carrier，SPIR-V把table/root descriptor/RootConstants/StaticSampler lower成对应
Vulkan records。没有attribute时D3与Vulkan各用普通默认layout；pipeline的精确Target layout modifier
不进入shaderlib。RootSignature中的static sampler必须唯一关联active `SamplerState` declaration，
SPIR-V metadata保留完整filter/address/LOD/bias/anisotropy/compare/border/reduction state，不能只记
immutable bit。

新pass的keyword pragma必须写在根`.hlsl`；每个concrete compile request为每个group提供一个合法
assignment。`RadRayShaderLibPass`验证forward产品source及测试目录中的depth/compute source的双target
compile、active binding和actual stage visibility。DXIL的`tN`、`sN`数字可以相同，但资源与sampler namespace不能
混淆。
