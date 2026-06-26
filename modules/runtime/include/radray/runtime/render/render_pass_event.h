#pragma once

#include <cstdint>

namespace radray::srp {

/// Pass 排序键。照搬 Unity SRP `RenderPassEvent` 的稀疏数值,
/// 故意留出插入间隙,让用户能用 `Event + offset` 在内置事件之间插队。
/// 对应源码:`com.unity.render-pipelines.universal/Runtime/Passes/ScriptableRenderPass.cs:67`。
enum class RenderPassEvent : int32_t {
    BeforeRendering = 0,
    BeforeRenderingShadows = 50,
    AfterRenderingShadows = 100,
    BeforeRenderingPrePasses = 150,
    AfterRenderingPrePasses = 200,
    BeforeRenderingGbuffer = 210,
    AfterRenderingGbuffer = 220,
    BeforeRenderingDeferredLights = 230,
    AfterRenderingDeferredLights = 240,
    BeforeRenderingOpaques = 250,
    AfterRenderingOpaques = 300,
    BeforeRenderingSkybox = 350,
    AfterRenderingSkybox = 400,
    BeforeRenderingTransparents = 450,
    AfterRenderingTransparents = 500,
    BeforeRenderingPostProcessing = 550,
    AfterRenderingPostProcessing = 600,
    AfterRendering = 1000,
};

}  // namespace radray::srp
