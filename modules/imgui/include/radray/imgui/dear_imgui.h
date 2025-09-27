#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <imgui.h>
#ifdef RADRAY_PLATFORM_WINDOWS
#include <imgui_impl_win32.h>
#endif

#include <radray/render/common.h>
#include <radray/window/native_window.h>

namespace radray {

class ImGuiPlatformInitDescriptor {
public:
    PlatformId Platform;

    NativeWindow* Window;
};

class ImGuiRendererData {
public:
};

class ImGuiDrawTexture {
public:
    ImGuiDrawTexture(shared_ptr<render::Texture> tex, shared_ptr<render::TextureView> srv) noexcept;
    ~ImGuiDrawTexture() noexcept;

public:
    shared_ptr<render::Texture> _tex;
    shared_ptr<render::TextureView> _srv;
};

class ImGuiDrawDescriptor {
public:
    render::Device* Device;
    render::TextureFormat RTFormat;
    int FrameCount;
};

class ImGuiDrawContext {
public:
    void Draw(ImDrawData* drawData);
    void UpdateTexture(ImTextureData* tex);

    render::Device* _device;
    shared_ptr<render::DescriptorSetLayout> _rsLayout;
    shared_ptr<render::RootSignature> _rs;
    shared_ptr<render::GraphicsPipelineState> _pso;
    unordered_map<render::Texture*, unique_ptr<ImGuiDrawTexture>> _texs;

    ImGuiDrawDescriptor _desc;
};

bool InitImGui();
bool InitPlatformImGui(const ImGuiPlatformInitDescriptor& desc);
bool InitRendererImGui();
void TerminateRendererImGui();
void TerminatePlatformImGui();
void TerminateImGui();

Nullable<Win32WNDPROC> GetImGuiWin32WNDPROC() noexcept;
std::span<const byte> GetImGuiShaderDXIL_VS() noexcept;
std::span<const byte> GetImGuiShaderDXIL_PS() noexcept;
std::span<const byte> GetImGuiShaderSPIRV_VS() noexcept;
std::span<const byte> GetImGuiShaderSPIRV_PS() noexcept;
Nullable<unique_ptr<ImGuiDrawContext>> CreateImGuiDrawContext(const ImGuiDrawDescriptor& desc) noexcept;

}  // namespace radray

#endif

// imgui shader hlsl
/*
struct VS_INPUT
{
    [[vk::location(0)]] float2 pos : POSITION;
    [[vk::location(1)]] float4 col : COLOR0;
    [[vk::location(2)]] float2 uv  : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    [[vk::location(0)]] float4 col : COLOR0;
    [[vk::location(1)]] float2 uv  : TEXCOORD0;
};

cbuffer vertexBuffer : register(b0)
{
    float4x4 ProjectionMatrix;
};
SamplerState sampler0 : register(s0);
Texture2D texture0 : register(t0);

PS_INPUT VSMain(VS_INPUT input)
{
    PS_INPUT output;
    output.pos = mul( ProjectionMatrix, float4(input.pos.xy, 0.f, 1.f));
    output.col = input.col;
    output.uv  = input.uv;
    return output;
}

float4 PSMain(PS_INPUT input) : SV_Target
{
    float4 out_col = input.col * texture0.Sample(sampler0, input.uv);
    return out_col;
}
*/
