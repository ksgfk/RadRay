> - 适用: 新增或修改 shaderlib 根 `.hlsl` pass、keyword domain、binding 或 target gate
> - 权威: 本文是当前 HLSL authoring 约定；wire 与 runtime 边界见 shader pipeline 架构文档
> - 锚点: `shaderlib/core/platform.hlsli`, `shaderlib/passes/forward.hlsl`, `shaderlib/passes/depth.hlsl`, `shaderlib/passes/compute.hlsl`, `modules/shader_compiler/tests/test_shaderlib_passes.cpp`

# HLSL authoring

## 文件边界

`shaderlib/` 本身就是 include root。根 `.hlsl` 是一个 pass source unit，`.hlsli` 是共享库
header；include 必须使用 root-relative、尖括号路径：

```hlsl
#include <core/platform.hlsli>
#include <core/color.hlsli>
```

不要把 `shaderlib/` 重复写进 include path，也不要用物理文件系统路径作为 `SourceName`。caller
传入的逻辑路径应类似 `passes/forward.hlsl`，它会进入诊断和 compile input identity。

## Entry 与 keyword

使用标准 stage attribute，entry name 不需要额外登记：

```hlsl
#pragma radray_keyword_group QUALITY "low" "high"

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0f);
}
```

graphics 至少有 vertex；pixel 可省略以表达 depth-only。compute source 只能有一个 compute
entry。entry 不能被条件编译包围，也不能让 graphics 与 compute 共用一个 source unit。

每个 keyword group 的 concrete compile request 必须选择一个合法值。普通 `Defines` 只用于
不属于 keyword domain 的编译输入；不要用普通 define 覆盖 keyword group。

## Binding 与 target gate

新 pass 的 binding 只通过 `core/platform.hlsli` 的 gate 宏书写：

```hlsl
VK_BINDING(6, 2) Texture2D<float4> AlbedoTexture;
VK_BINDING(7, 2) SamplerState LinearSampler;
```

SPIR-V lane 会把宏展开为标准 `vk::binding` 属性，DXIL lane 会展开为空；两套 target 的实际
binding 数字可以不同，不能假设 set 等于 space 或 binding 数字相等。位置和 push constants
使用 `VK_LOCATION` 与 `VK_PUSH_CONSTANT`。

不要在 pass 中手写 backend attribute、添加 `register` 声明、创建 numbered binding wrapper，
或维护 sidecar metadata。HLSL declaration name 是 runtime lookup identity；render layout 会
为当前 artifact 生成不透明 `BindingHandle`。

## 现有最小 pass

| Pass | 用途 | contract facts |
|---|---|---|
| `passes/forward.hlsl` | vertex + pixel | texture、sampler、颜色转换、`QUALITY` |
| `passes/depth.hlsl` | vertex-only | depth topology、`DEPTH_MODE` |
| `passes/compute.hlsl` | compute dispatch | storage buffer、`COMPUTE_MODE` |

这三条 pass 由 `RadRayShaderLibPass` 读取真实 shaderlib source，执行 discovery，并同时编译
DXIL/SPIR-V lane。新增 pass 后应在同一测试中验证 entry 数量、active binding、stage visibility
和 target-specific binding facts。

## 验收命令

```powershell
cmake --build build_debug --parallel 24
ctest --test-dir build_debug -C Debug -R "RadRayShaderLibPass" --output-on-failure
```

GPU runtime pass 由 `RadRayRuntimeShaderJit` 验证；没有可用 GPU 时测试可以 skip，但已创建设备
后的资源、pipeline、提交或 readback 错误必须失败。
