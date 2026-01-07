#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <stdexcept>
#include <span>
#include <thread>

#include <imgui.h>

#include <radray/channel.h>
#include <radray/render/common.h>
#include <radray/window/native_window.h>

namespace radray {

class ImGuiApplicationException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    template <typename... Args>
    explicit ImGuiApplicationException(fmt::format_string<Args...> fmt, Args&&... args) : std::runtime_error(fmt::format(fmt, std::forward<Args>(args)...)) {}
    ~ImGuiApplicationException() noexcept override = default;
};

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

class ImGuiRenderer {
public:
    struct DrawCmd {
        ImVec4 ClipRect;
        ImTextureRef TexRef;
        uint32_t VtxOffset;
        uint32_t IdxOffset;
        uint32_t ElemCount;
        ImDrawCallback UserCallback{nullptr};
    };

    struct DrawList {
        vector<DrawCmd> Cmd;
        int32_t VtxBufferSize;
        int32_t IdxBufferSize;
    };

    struct DrawData {
        vector<DrawList> Cmds;
        ImVec2 DisplayPos;
        ImVec2 DisplaySize;
        ImVec2 FramebufferScale;
        int32_t TotalVtxCount;
    };

    class ImGuiTexture {
    public:
        ImGuiTexture(
            unique_ptr<render::Texture> tex,
            unique_ptr<render::TextureView> srv,
            unique_ptr<render::DescriptorSet> descSet) noexcept
            : _tex(std::move(tex)),
              _srv(std::move(srv)),
              _descSet(std::move(descSet)) {}
        ~ImGuiTexture() noexcept = default;

    public:
        unique_ptr<render::Texture> _tex;
        unique_ptr<render::TextureView> _srv;
        unique_ptr<render::DescriptorSet> _descSet;
    };

    struct UploadTexturePayload {
        render::Texture* _dst;
        render::Buffer* _src;
        bool _isNew;
    };

    class Frame {
    public:
        unique_ptr<render::Buffer> _vb;
        void* _vbMapped{nullptr};
        unique_ptr<render::Buffer> _ib;
        void* _ibMapped{nullptr};
        DrawData _drawData;
        int32_t _vbSize{0};
        int32_t _ibSize{0};

        vector<UploadTexturePayload> _uploadTexReqs;
        deque<unique_ptr<render::Buffer>> _tempBufs;
        unordered_map<render::TextureView*, unique_ptr<render::DescriptorSet>> _tempTexSets;
        deque<unique_ptr<ImGuiTexture>> _waitForFreeTexs;
    };

    ImGuiRenderer(
        render::Device* device,
        render::TextureFormat rtFormat,
        uint32_t inflightFrameCount);
    ~ImGuiRenderer() noexcept;

    void ExtractDrawData(uint32_t frameIndex, ImDrawData* drawData);
    void OnRenderBegin(uint32_t frameIndex, render::CommandBuffer* cmdBuffer);
    void OnRender(uint32_t frameIndex, render::CommandEncoder* encoder);
    void OnRenderComplete(uint32_t frameIndex);
    void SetupRenderState(int frameIndex, render::CommandEncoder* encoder, int fbWidth, int fbHeight);

public:
    render::Device* _device;
    unique_ptr<render::RootSignature> _rootSig;
    unique_ptr<render::GraphicsPipelineState> _pso;
    vector<unique_ptr<Frame>> _frames;
    vector<unique_ptr<ImGuiTexture>> _aliveTexs;
};

struct ImGuiAppConfig {
    string AppName;
    string Title;
    uint32_t Width{1280};
    uint32_t Height{720};

    render::RenderBackend Backend;
    std::optional<render::DeviceDescriptor> DeviceDesc{};
    uint32_t BackBufferCount{3};
    uint32_t InFlightFrameCount{2};
    render::TextureFormat RTFormat{render::TextureFormat::RGBA8_UNORM};
    bool EnableVSync{true};
    bool EnableMultiThreading{true};
    bool EnableFrameDropping{false};
    bool EnableValidation{true};
};

class ImGuiApplication {
public:
    ImGuiApplication() = default;
    virtual ~ImGuiApplication() noexcept = default;

    ImGuiApplication(const ImGuiApplication&) = delete;
    ImGuiApplication(ImGuiApplication&&) = delete;
    ImGuiApplication& operator=(const ImGuiApplication&) = delete;
    ImGuiApplication& operator=(ImGuiApplication&&) = delete;

    void Setup(const ImGuiAppConfig& config);
    void Run();
    void Destroy() noexcept;

protected:
    virtual void OnStart(const ImGuiAppConfig& config);
    virtual void OnDestroy() noexcept;
    virtual void OnUpdate();
    virtual void OnImGui();
    virtual vector<render::CommandBuffer*> OnRender(uint32_t frameIndex) = 0;

    void Init(const ImGuiAppConfig& config);
    void RecreateSwapChain();
    render::TextureView* GetDefaultRTV(uint32_t backBufferIndex);

private:
    void LoopSingleThreaded();
    void LoopMultiThreaded();

protected:
    // imgui
    unique_ptr<ImGuiContextRAII> _imgui;
    // window
    unique_ptr<NativeWindow> _window;
    // render objects
    unique_ptr<render::InstanceVulkan> _vkIns;
    shared_ptr<render::Device> _device;
    render::CommandQueue* _cmdQueue{nullptr};
    unique_ptr<render::SwapChain> _swapchain;
    vector<unique_ptr<render::Fence>> _inFlightFences;
    vector<unique_ptr<render::Semaphore>> _renderFinishSemaphores;
    vector<unique_ptr<render::Semaphore>> _imageAvailableSemaphores;
    vector<render::Texture*> _backBuffers;
    vector<unique_ptr<render::TextureView>> _defaultRTVs;
    unique_ptr<ImGuiRenderer> _imguiRenderer;
    // multi-threading
    unique_ptr<std::thread> _renderThread;
    unique_ptr<BoundedChannel<uint32_t>> _freeFrames;
    unique_ptr<BoundedChannel<uint32_t>> _submitFrames;
    // global configs
    uint32_t _rtWidth{0};
    uint32_t _rtHeight{0};
    uint32_t _backBufferCount{0};
    uint32_t _inFlightFrameCount{0};
    render::TextureFormat _rtFormat{render::TextureFormat::UNKNOWN};
    bool _enableVSync{false};
    bool _enableMultiThreading{false};
    bool _enableFrameDropping{false};
    // state
    std::atomic_bool _needClose{false};
    uint64_t _frameCount{0};
    vector<uint8_t> _frameState;  // true: frame is submitted but not yet completed, false: frame is completed
    double _time{0};
    double _deltaTime{0};
    std::atomic<double> _gpuTime{0};
    std::atomic<double> _gpuDeltaTime{0};
};

std::span<const byte> GetImGuiShaderDXIL_VS() noexcept;
std::span<const byte> GetImGuiShaderDXIL_PS() noexcept;
std::span<const byte> GetImGuiShaderSPIRV_VS() noexcept;
std::span<const byte> GetImGuiShaderSPIRV_PS() noexcept;

}  // namespace radray

#endif
