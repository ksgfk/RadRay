#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <stdexcept>

#include <imgui.h>
#ifdef RADRAY_PLATFORM_WINDOWS
#include <imgui_impl_win32.h>
#endif

#include <radray/channel.h>
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

struct ImGuiApplicationDescriptor {
    std::string_view AppName;
    Eigen::Vector2i WindowSize;
    bool Resizeable;
    bool IsFullscreen;
    render::RenderBackend Backend;
    uint32_t FrameCount;
    render::TextureFormat RTFormat;
    bool EnableValidation;
    bool EnableVSync;
    bool IsWaitFrame;
    bool IsRenderMultiThread;
};

class ImGuiApplicationException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    template <typename... Args>
    explicit ImGuiApplicationException(fmt::format_string<Args...> fmt, Args&&... args) : _msg(::radray::format(fmt, std::forward<Args>(args)...)) {}
    ~ImGuiApplicationException() noexcept override = default;

    const char* what() const noexcept override;

private:
    string _msg;
};

class ImGuiApplication {
public:
    class Frame {
    public:
        Frame(size_t index, shared_ptr<render::CommandBuffer> cmdBuffer) noexcept;
        ~Frame() noexcept = default;
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        Frame(Frame&&) noexcept = delete;
        Frame& operator=(Frame&&) noexcept = delete;

        size_t _frameIndex;
        uint64_t _completeFrame{std::numeric_limits<uint64_t>::max()};
        shared_ptr<render::CommandBuffer> _cmdBuffer{};
        render::Texture* _rt{};
        render::TextureView* _rtView{};
    };

    explicit ImGuiApplication(const ImGuiApplicationDescriptor& desc);
    virtual ~ImGuiApplication() noexcept;

    ImGuiApplication(const ImGuiApplication&) = delete;
    ImGuiApplication& operator=(const ImGuiApplication&) = delete;
    ImGuiApplication(ImGuiApplication&&) = delete;
    ImGuiApplication& operator=(ImGuiApplication&&) = delete;

    void Run();
    void Destroy() noexcept;

protected:
    void NewSwapChain();
    void RecreateSwapChain();

    // TODO: use strategy pattern
    virtual void MainUpdate();
    virtual void RenderUpdateMultiThread();
    void RunMultiThreadRender();
    void RunSingleThreadRender();

    virtual void OnResizing(int width, int height);
    virtual void OnResized(int width, int height);
    virtual void OnUpdate();
    virtual void OnImGui();
    virtual void OnRender(ImGuiApplication::Frame* frame);
    virtual void OnDestroy() noexcept;

    render::TextureView* SafeGetRTView(radray::render::Texture* rt);
    Nullable<ImGuiApplication::Frame*> GetAvailableFrame();

    template <class F>
    void ExecuteOnRenderThreadBeforeAcquire(F&& func) {
        _beforeAcquire.WaitWrite(std::forward<F>(func));
    }
    void ExecuteBeforeAcquire();

protected:
    void OnResizingCb(int width, int height);
    void OnResizedCb(int width, int height);

    uint32_t _frameCount;
    render::TextureFormat _rtFormat;
    bool _enableVSync;
    bool _isWaitFrame;
    bool _multithreadRender;

    unique_ptr<ImGuiContextRAII> _imguiContext;
    shared_ptr<std::function<Win32WNDPROC>> _win32ImguiProc;
    unique_ptr<NativeWindow> _window;
    unique_ptr<render::InstanceVulkan> _vkIns;
    shared_ptr<render::Device> _device;
    render::CommandQueue* _cmdQueue;
    shared_ptr<render::SwapChain> _swapchain;
    unique_ptr<ImGuiDrawContext> _imguiDrawContext;
    unique_ptr<std::thread> _renderThread;
    BoundedChannel<size_t> _freeFrame;
    BoundedChannel<size_t> _waitFrame;
    radray::UnboundedChannel<std::function<void(void)>> _beforeAcquire;
    vector<unique_ptr<Frame>> _frames;
    uint64_t _currRenderFrame;

    unordered_map<render::Texture*, shared_ptr<render::TextureView>> _rtViews;
    sigslot::scoped_connection _resizingConn;
    sigslot::scoped_connection _resizedConn;
    Eigen::Vector2i _renderRtSize;
    bool _isResizingRender{false};
    bool _isRenderAcquiredRt{false};
    std::atomic_bool _needClose{false};

    double _logicTime{0};
    std::atomic<double> _renderTime{0};
};

bool InitImGui();
bool InitPlatformImGui(const ImGuiPlatformInitDescriptor& desc);
bool InitRendererImGui(ImGuiContext* context);
void TerminateRendererImGui(ImGuiContext* context);
void TerminatePlatformImGui(ImGuiContext* context);
void SetWin32DpiAwarenessImGui();

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
