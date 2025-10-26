#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <imgui.h>
#ifdef RADRAY_PLATFORM_WINDOWS
#include <imgui_impl_win32.h>
#endif

#include <radray/render/common.h>
#include <radray/window/native_window.h>

namespace radray {

class ImGuiContextRAII {
public:
    explicit ImGuiContextRAII(ImFontAtlas* sharedFontAtlas = nullptr);
    ImGuiContextRAII(const ImGuiContextRAII&) = delete;
    ImGuiContextRAII(ImGuiContextRAII&&) noexcept;
    ImGuiContextRAII& operator=(const ImGuiContextRAII&) = delete;
    ImGuiContextRAII& operator=(ImGuiContextRAII&&) noexcept;
    ~ImGuiContextRAII() noexcept;

    bool IsValid() const noexcept;
    void Destroy() noexcept;
    ImGuiContext* Get() const noexcept;

    void SetCurrent();

    friend constexpr void swap(ImGuiContextRAII& a, ImGuiContextRAII& b) noexcept {
        using std::swap;
        swap(a._ctx, b._ctx);
    }

private:
    ImGuiContext* _ctx{nullptr};
};

class ImGuiPlatformInitDescriptor {
public:
    PlatformId Platform;
    ImGuiContext* Context;
    NativeWindow* Window;
};

class ImGuiDrawDescriptor {
public:
    render::Device* Device;
    render::TextureFormat RTFormat;
    int FrameCount;
};

class ImGuiRendererData {
public:
};

class ImGuiDrawTexture {
public:
    ImGuiDrawTexture(shared_ptr<render::Texture> tex, shared_ptr<render::TextureView> srv) noexcept;
    ImGuiDrawTexture(const ImGuiDrawTexture&) = default;
    ImGuiDrawTexture& operator=(const ImGuiDrawTexture&) = default;
    ImGuiDrawTexture(ImGuiDrawTexture&&) = default;
    ImGuiDrawTexture& operator=(ImGuiDrawTexture&&) = default;
    ~ImGuiDrawTexture() noexcept;

public:
    shared_ptr<render::Texture> _tex;
    shared_ptr<render::TextureView> _srv;
};

class ImGuiDrawContext {
public:
    void ExtractDrawData(int frame, ImDrawData* drawData);
    void ExtractTexture(int frame, ImTextureData* tex);

    void BeforeDraw(int frameIndex, render::CommandBuffer* cmdBuffer);
    void Draw(int frame, render::CommandEncoder* encoder);
    void AfterDraw(int frameIndex);
    void SetupRenderState(int frame, render::CommandEncoder* encoder, int fbWidth, int fbHeight);

    struct DrawCmd {
        ImVec4 ClipRect;
        ImTextureRef TexRef;
        unsigned int VtxOffset;
        unsigned int IdxOffset;
        unsigned int ElemCount;
        ImDrawCallback UserCallback;

        ImTextureID GetTexID() const noexcept;
    };

    struct DrawList {
        vector<DrawCmd> CmdBuffer;
        int VtxBufferSize;
        int IdxBufferSize;
    };

    struct DrawData {
        vector<DrawList> CmdLists;
        ImVec2 DisplayPos;
        ImVec2 DisplaySize;
        ImVec2 FramebufferScale;
        int TotalVtxCount;

        void Clear() noexcept;
    };

    struct UploadTexturePayload {
        render::Texture* _tex;
        render::Buffer* _upload;
        bool _isNew;
    };

    class Frame {
    public:
        vector<shared_ptr<render::DescriptorSet>> _descSets;
        size_t _usableDescSetIndex{0};
        vector<shared_ptr<render::Buffer>> _tempUploadBuffers;
        vector<UploadTexturePayload> _needCopyTexs;
        vector<render::Texture*> _waitDestroyTexs;

        shared_ptr<render::Buffer> _vb;
        shared_ptr<render::Buffer> _ib;
        int32_t _vbSize{0};
        int32_t _ibSize{0};

        DrawData _drawData;
    };

    render::Device* _device;
    shared_ptr<render::DescriptorSetLayout> _rsLayout;
    shared_ptr<render::RootSignature> _rs;
    shared_ptr<render::GraphicsPipelineState> _pso;
    vector<Frame> _frames;
    unordered_map<render::Texture*, unique_ptr<ImGuiDrawTexture>> _texs;

    ImGuiDrawDescriptor _desc;
};

class ImGuiApplication {
public:
public:
    ImGuiContextRAII _context;
};

bool InitImGui();
bool InitPlatformImGui(const ImGuiPlatformInitDescriptor& desc);
bool InitRendererImGui(ImGuiContext* context);
void TerminateRendererImGui(ImGuiContext* context);
void TerminatePlatformImGui(ImGuiContext* context);

Nullable<Win32WNDPROC*> GetImGuiWin32WNDPROC() noexcept;
Nullable<std::function<Win32WNDPROC>> GetImGuiWin32WNDPROCEx(ImGuiContext* context) noexcept;
std::span<const byte> GetImGuiShaderDXIL_VS() noexcept;
std::span<const byte> GetImGuiShaderDXIL_PS() noexcept;
std::span<const byte> GetImGuiShaderSPIRV_VS() noexcept;
std::span<const byte> GetImGuiShaderSPIRV_PS() noexcept;
Nullable<unique_ptr<ImGuiDrawContext>> CreateImGuiDrawContext(const ImGuiDrawDescriptor& desc) noexcept;

}  // namespace radray

#endif

// imgui shader hlsl
// .\dxc.exe -all_resources_bound -HV 2021 -O3 -Ges -T vs_6_0 -E VSMain -Qstrip_reflect -Fo imgui_vs.dxil imgui.hlsl
// .\dxc.exe -all_resources_bound -HV 2021 -O3 -Ges -T ps_6_0 -E PSMain -Qstrip_reflect -Fo imgui_ps.dxil imgui.hlsl
// .\dxc.exe -spirv -all_resources_bound -HV 2021 -O3 -Ges -T vs_6_0 -E VSMain -Fo imgui_vs.spv imgui.hlsl
// .\dxc.exe -spirv -all_resources_bound -HV 2021 -O3 -Ges -T ps_6_0 -E PSMain -Fo imgui_ps.spv imgui.hlsl
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

struct PreObject
{
    float4x4 ProjectionMatrix;
};

[[vk::push_constant]] PreObject g_preObj : register(b0);
[[vk::binding(0)]] Texture2D texture0 : register(t0);
[[vk::binding(1)]] SamplerState sampler0 : register(s0);

PS_INPUT VSMain(VS_INPUT input)
{
    PS_INPUT output;
    output.pos = mul(g_preObj.ProjectionMatrix, float4(input.pos.xy, 0.f, 1.f));
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
