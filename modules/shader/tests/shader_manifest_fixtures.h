#pragma once

#include <string_view>

// 两个测试共用的 manifest 正例。
//
// 为什么单独成头: 这批 JSON 同时被 modules/shader/tests/test_shader_asset.cpp (解析、
// 序列化、校验) 与 modules/runtime/tests/test_shader_layout_binding.cpp (manifest -> RHI
// 描述的打包) 使用。复制一份会让两边悄悄漂移 —— 改了一处 binding 声明另一处仍在断言
// 旧值, 且不会有任何编译错误提示。

namespace radray::test {

// 与 shaderlib/imgui/imgui_pass.hlsl 对应的完整 manifest。
// 该 shader 同时用到 push constant (gPush) 与可做 static sampler 的 gSampler,
// 是最小但覆盖面最广的正例。
constexpr std::string_view kImGuiManifest = R"JSON({
  "FormatVersion": 1,
  "Name": "RadRayImGui",
  "Source": "imgui/imgui_pass.hlsl",
  "Passes": [
    {
      "Name": "Default",
      "Stages": [
        { "Stage": "Vertex", "EntryPoint": "VSMain" },
        { "Stage": "Pixel",  "EntryPoint": "PSMain" }
      ],
      "ShaderModel": "SM60",
      "PushConstant": {
        "Name": "gPush",
        "Location": { "Group": 0, "Binding": 0 },
        "Size": 16,
        "Stages": ["Vertex"]
      },
      "BindingGroups": [
        {
          "Group": 1,
          "Bindings": [
            { "Name": "gTexture", "Binding": 0, "Type": "Texture", "Stages": ["Pixel"] },
            {
              "Name": "gSampler", "Binding": 1, "Type": "Sampler", "Stages": ["Pixel"],
              "ImmutableSampler": {
                "AddressS": "ClampToEdge", "AddressT": "ClampToEdge", "AddressR": "ClampToEdge",
                "MinFilter": "Linear", "MagFilter": "Linear", "MipmapFilter": "Linear",
                "LodMin": 0.0, "LodMax": 0.0, "AnisotropyClamp": 0
              }
            }
          ]
        }
      ],
      "VertexInput": {
        "Buffers": [{ "Binding": 0, "ArrayStride": 20, "StepMode": "Vertex" }],
        "Attributes": [
          { "Semantic": "POSITION", "SemanticIndex": 0, "Format": "FLOAT32X2", "BufferBinding": 0, "Offset": 0 },
          { "Semantic": "TEXCOORD", "SemanticIndex": 0, "Format": "FLOAT32X2", "BufferBinding": 0, "Offset": 8 },
          { "Semantic": "COLOR",    "SemanticIndex": 0, "Format": "UNORM8X4",  "BufferBinding": 0, "Offset": 16 }
        ]
      }
    }
  ]
})JSON";

// 与 shaderlib/forward_pipeline/forward_pass.hlsl 对应。覆盖 root descriptor、
// keyword 组、以及被 #ifdef 包起来因此在部分变体里反射不可见的阴影绑定。
constexpr std::string_view kForwardManifest = R"JSON({
  "FormatVersion": 1,
  "Name": "ForwardPrincipled",
  "Source": "forward_pipeline/forward_pass.hlsl",
  "KeywordGroups": [
    { "Name": "BaseColorMap",  "Keywords": ["_BASECOLOR_MAP"],       "IsOptional": true, "Stages": ["Pixel"] },
    { "Name": "NormalMap",     "Keywords": ["_NORMAL_MAP"],          "IsOptional": true, "Stages": ["Pixel"] },
    { "Name": "AlphaMode",     "Keywords": ["_ALPHATEST_ON", "_ALPHABLEND_ON"], "IsOptional": true, "Stages": ["Pixel"] },
    { "Name": "PointShadows",  "Keywords": ["_POINT_SHADOWS"],       "IsOptional": true, "Stages": ["Vertex", "Pixel"] }
  ],
  "Passes": [
    {
      "Name": "Forward",
      "Stages": [
        { "Stage": "Vertex", "EntryPoint": "VSMain" },
        { "Stage": "Pixel",  "EntryPoint": "PSMain" }
      ],
      "ShaderModel": "SM62",
      "BindingGroups": [
        {
          "Group": 0,
          "Bindings": [
            { "Name": "gPerObject", "Binding": 1, "Type": "CBuffer",
              "Residency": "RootDescriptor", "Stages": ["Vertex", "Pixel"] }
          ]
        },
        {
          "Group": 1,
          "Bindings": [
            { "Name": "gView", "Binding": 0, "Type": "CBuffer",
              "Residency": "RootDescriptor", "Stages": ["Vertex", "Pixel"] },
            { "Name": "gShadowCube",    "Binding": 1, "Type": "Texture", "Stages": ["Pixel"] },
            { "Name": "gShadowArray",   "Binding": 2, "Type": "Texture", "Stages": ["Pixel"] },
            { "Name": "gShadowSampler", "Binding": 3, "Type": "Sampler", "Stages": ["Pixel"] }
          ]
        },
        {
          "Group": 2,
          "Bindings": [
            { "Name": "gMaterial",      "Binding": 0, "Type": "CBuffer", "Stages": ["Pixel"] },
            { "Name": "gBaseColorMap",  "Binding": 1, "Type": "Texture", "Stages": ["Pixel"] },
            { "Name": "gMetalRoughMap", "Binding": 2, "Type": "Texture", "Stages": ["Pixel"] },
            { "Name": "gNormalMap",     "Binding": 3, "Type": "Texture", "Stages": ["Pixel"] },
            { "Name": "gOcclusionMap",  "Binding": 4, "Type": "Texture", "Stages": ["Pixel"] },
            { "Name": "gEmissiveMap",   "Binding": 5, "Type": "Texture", "Stages": ["Pixel"] },
            { "Name": "gSampler",       "Binding": 6, "Type": "Sampler", "Stages": ["Pixel"] }
          ]
        }
      ],
      "VertexInput": {
        "Buffers": [{ "Binding": 0, "ArrayStride": 48, "StepMode": "Vertex" }],
        "Attributes": [
          { "Semantic": "POSITION", "Format": "FLOAT32X3", "BufferBinding": 0, "Offset": 0 },
          { "Semantic": "NORMAL",   "Format": "FLOAT32X3", "BufferBinding": 0, "Offset": 12 },
          { "Semantic": "TEXCOORD", "Format": "FLOAT32X2", "BufferBinding": 0, "Offset": 24 },
          { "Semantic": "TANGENT",  "Format": "FLOAT32X4", "BufferBinding": 0, "Offset": 32 }
        ]
      }
    }
  ]
})JSON";

/// 最小合法 manifest, 负例测试在此基础上做单点改动。
constexpr std::string_view kMinimalManifest = R"JSON({
  "FormatVersion": 1,
  "Name": "Minimal",
  "Source": "minimal.hlsl",
  "Passes": [
    {
      "Name": "Main",
      "Stages": [{ "Stage": "Compute", "EntryPoint": "CSMain" }],
      "BindingGroups": [
        {
          "Group": 0,
          "Bindings": [
            { "Name": "gInput", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] }
          ]
        }
      ]
    }
  ]
})JSON";


}  // namespace radray::test
