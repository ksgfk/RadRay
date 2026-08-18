> - 适用: 在 `shaderlib/` 里找现成实现；增加 HLSL 数学、光照、阴影或最小产品 pass
> - 权威: 本文是当前 HLSL 共享库边界与 target gate 约定的唯一说明；完整编译契约见 shader pipeline 架构文档
> - 锚点: `shaderlib/core/math.hlsli`, `shaderlib/core/color.hlsli`, `shaderlib/core/frame.hlsli`, `shaderlib/core/platform.hlsli`, `shaderlib/bsdf/principled.hlsli`, `shaderlib/lighting/lights.hlsli`, `shaderlib/shadow/filtering.hlsli`, `shaderlib/pipelines/forward/bindings.hlsli`, `shaderlib/pipelines/forward/forward.hlsl`, `shaderlib/passes/depth.hlsl`, `shaderlib/passes/compute.hlsl`

# shaderlib

`shaderlib/` 是 HLSL 源码树，也是 compiler include 解析使用的逻辑根目录。共享数学、材质、
光照和阴影原语位于库层；具体渲染管线的产品 source 位于 `pipelines/`，独立的 depth/compute
最小 pass 位于 `passes/`。compiler discovery 从根 `.hlsl` 推导 entry topology 和 keyword domain；
作者不维护 sidecar metadata，active binding 与 type tree 随 concrete target lane artifact 生成。

## 分层

下层不得 include 上层。

| 层 | 内容 | 约束 |
|---|---|---|
| `core/` | 标量数学、局部 frame、颜色转换 | 不包含光源、材质或后端绑定 |
| `bsdf/` | Fresnel、微表面分布、Principled BSDF | 不包含光源和资源绑定 |
| `lighting/` | 光源布局与辐照度求值 | 不包含 entry pass |
| `shadow/` | 过滤、级联和 cube shadow 原语 | 不包含 pipeline binding |
| `pipelines/` | 具体 pipeline 的 binding ABI 与产品 pass | group 语义只在所属 pipeline 内有效 |
| `passes/` | 独立的 depth-only、compute 根 pass | 只通过 `core/platform.hlsli` 使用 target gate |

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
| `core/platform.hlsli` | `VK_LOCATION`、`VK_BINDING`、`VK_PUSH_CONSTANT` target gate |
| `pipelines/forward/bindings.hlsli` | forward 的 view/material/object binding ABI |
| `pipelines/forward/forward.hlsl` | 纹理 Lambert 光照与颜色转换产品 pass |
| `passes/depth.hlsl` | vertex-only depth topology pass |
| `passes/compute.hlsl` | storage buffer compute pass |

## 编码约定

所有 include 使用 shaderlib-root-relative 的尖括号路径，例如：

```hlsl
#include <core/math.hlsli>
#include <bsdf/principled.hlsli>
```

共享结构的字段顺序、对齐和矩阵约定是 shader ABI；runtime 根据 compiler type tree 按成员名
逐字段打包，不写 CPU mirror struct 或 `offsetof` 断言。`shadow/filtering.hlsli` 中的序列化枚举值
和 `lights.hlsli` 中的上限属于 ABI，不能因为重命名或排版而改变。

`core/platform.hlsli` 只提供 DXIL/SPIR-V target gate：`VK_LOCATION`、`VK_BINDING` 和
`VK_PUSH_CONSTANT` 在 SPIR-V lane 展开为属性，在 DXIL lane 展开为空。它不分配编号，
也不提供把 group/slot 打包成单一宏的 numbered wrapper。新 pass 的 keyword pragma 必须写在
根 `.hlsl`；每个 concrete compile request 为每个 group 提供一个合法 assignment。`RadRayShaderLibPass`
验证 forward、depth 与 compute 三条 source 的双 target compile、active binding 和 stage visibility。DXIL 的 `tN`、`sN`
数字可以相同，但资源与 sampler namespace 不能混淆；static sampler 必须能关联 active
`SamplerState` declaration，缺失或冲突时 compiler fail closed。
