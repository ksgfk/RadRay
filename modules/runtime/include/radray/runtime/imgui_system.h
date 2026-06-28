#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <cstddef>
#include <limits>
#include <span>
#include <optional>
#include <utility>

#include <imgui.h>
#include <sigslot/signal.hpp>

#include <radray/types.h>
#include <radray/runtime_type.h>
#include <radray/nullable.h>
#include <radray/render/common.h>
#include <radray/runtime/application.h>

namespace radray {

class AppWindow;
class WindowManager;
class AppFrameContext;
class Application;
class ImGuiSystem;
class ImGuiRenderer;
class NativeWindow;
enum class KeyCode;
enum class MouseButton;
struct AppSwapChainRecreateContext;
struct AppFrameTarget;

struct ImGuiRendererDescriptor {
    render::Device* Device;
    render::TextureFormat RenderTargetFormat{render::TextureFormat::UNKNOWN};
    uint32_t FlightDataCount;
};

struct ImGuiSystemDescriptor {
    AppWindow* MainWindow;
    WindowManager* Windows{nullptr};
    render::Device* Device;
    render::TextureFormat RenderTargetFormat{render::TextureFormat::UNKNOWN};
    uint32_t FlightDataCount;
    render::CommandQueue* DirectQueue{nullptr};
    uint32_t BackBufferCount{3};
    render::PresentMode PresentMode{render::PresentMode::FIFO};
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
    void Swap(ImGuiContextRAII& other) noexcept;

private:
    ImGuiContext* _ctx{nullptr};
};

class ImGuiRenderer {
public:
    class ImGuiTexture;

    struct DrawCmd {
        ImVec4 ClipRect{};
        ImGuiTexture* Texture{nullptr};
        bool HasExternalTexture{false};
        uint32_t VtxOffset{0};
        uint32_t IdxOffset{0};
        uint32_t ElemCount{0};
        ImDrawCallback UserCallback{nullptr};
    };

    struct DrawList {
        vector<DrawCmd> Cmd;
        int32_t VtxBufferSize{0};
        int32_t IdxBufferSize{0};
    };

    struct DrawData {
        vector<DrawList> Cmds;
        ImGuiID ViewportId{0};
        ImVec2 DisplayPos{};
        ImVec2 DisplaySize{};
        ImVec2 FramebufferScale{};
        uint32_t VtxOffset{0};
        uint32_t IdxOffset{0};
        int32_t TotalVtxCount{0};
        int32_t TotalIdxCount{0};
    };

    class ImGuiTexture {
    public:
        // 标记外部纹理构造：描述符集按 flight 延迟创建
        struct ExternalTag {};

        ImGuiTexture(
            unique_ptr<render::Texture> texture,
            unique_ptr<render::TextureView> srv,
            unique_ptr<render::DescriptorSet> descriptorSet) noexcept
            : _texture(std::move(texture)),
              _srv(std::move(srv)),
              _descriptorSet(std::move(descriptorSet)) {}

        // 外部纹理：每个 flight 持有独立描述符集，避免在命令缓冲飞行中改写同一描述符集
        explicit ImGuiTexture(ExternalTag) noexcept : _isExternal(true) {}

        render::Texture* GetTexture() const noexcept { return _texture.get(); }
        // 普通纹理使用单一描述符集；外部纹理按当前 flight 取对应描述符集
        render::DescriptorSet* GetDescriptorSet(uint32_t flightIndex) const noexcept {
            if (_isExternal) {
                return flightIndex < _externalSets.size() ? _externalSets[flightIndex].get() : nullptr;
            }
            return _descriptorSet.get();
        }
        render::DescriptorSet* GetExternalSet(uint32_t flightIndex) const noexcept {
            return flightIndex < _externalSets.size() ? _externalSets[flightIndex].get() : nullptr;
        }
        void SetExternalSet(uint32_t flightIndex, unique_ptr<render::DescriptorSet> set) noexcept {
            if (flightIndex >= _externalSets.size()) {
                _externalSets.resize(static_cast<size_t>(flightIndex) + 1);
            }
            _externalSets[flightIndex] = std::move(set);
        }
        bool UpdateExternalResource(uint32_t flightIndex, render::TextureView* srv) noexcept;
        bool IsExternal() const noexcept { return _isExternal; }

    private:
        unique_ptr<render::Texture> _texture;
        unique_ptr<render::TextureView> _srv;
        unique_ptr<render::DescriptorSet> _descriptorSet;
        vector<unique_ptr<render::DescriptorSet>> _externalSets;
        bool _isExternal{false};
    };

    struct UploadTexturePayload {
        render::Texture* Dst{nullptr};
        render::Buffer* Src{nullptr};
        bool IsNew{false};
    };

    class Frame {
    public:
        Frame() noexcept = default;

    private:
        friend class ImGuiRenderer;

        unique_ptr<render::Buffer> _vb;
        unique_ptr<render::Buffer> _ib;
        vector<DrawData> _drawData;
        int32_t _vbSize{0};
        int32_t _ibSize{0};

        vector<UploadTexturePayload> _uploadTexReqs;
        deque<unique_ptr<render::Buffer>> _tempBufs;
        deque<unique_ptr<ImGuiTexture>> _waitForFreeTexs;
    };

    explicit ImGuiRenderer() noexcept;
    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer(ImGuiRenderer&&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(ImGuiRenderer&&) = delete;
    ~ImGuiRenderer() noexcept;

    uint32_t GetViewportDrawDataCount(uint32_t frameIndex) const noexcept;
    std::optional<uint32_t> FindViewportDrawDataIndex(uint32_t frameIndex, ImGuiViewport* viewport) const noexcept;

    void ExtractDrawData(uint32_t frameIndex);
    void OnRenderBegin(uint32_t frameIndex, render::CommandBuffer* cmdBuffer);
    void OnRenderViewport(uint32_t frameIndex, ImGuiViewport* viewport, render::GraphicsCommandEncoder* encoder);
    void OnRenderComplete(uint32_t frameIndex);
    void OnSwapChainRecreate(const AppSwapChainRecreateContext& ctx);
    void SetupRenderState(uint32_t frameIndex, uint32_t drawDataIndex, render::GraphicsCommandEncoder* encoder, int32_t fbWidth, int32_t fbHeight);
    render::Device* GetDevice() const noexcept { return _device; }
    ImTextureID CreateOrUpdateExternalTexture(ImTextureID textureId, uint32_t flightIndex, render::TextureView* srv);

    static Nullable<unique_ptr<ImGuiRenderer>> Create(const ImGuiRendererDescriptor& desc) noexcept;

private:
    friend class ImGuiSystem;

    void ExtractDrawDataToFrame(Frame& frame, std::span<ImDrawData*> drawDataList);
    void OnRenderBeginFrame(Frame& frame, render::CommandBuffer* cmdBuffer);
    void OnRenderFrame(uint32_t frameIndex, Frame& frame, uint32_t drawDataIndex, render::GraphicsCommandEncoder* encoder);
    void OnRenderCompleteFrame(Frame& frame);
    void SetupRenderStateForFrame(const Frame& frame, uint32_t drawDataIndex, render::GraphicsCommandEncoder* encoder, int32_t fbWidth, int32_t fbHeight) const;
    bool OwnsTexture(const ImGuiTexture* texture) const noexcept;

    ImGuiSystem* _system;
    render::Device* _device{nullptr};
    unique_ptr<render::RootSignature> _rootSig;
    unique_ptr<render::GraphicsPipelineState> _pso;
    render::BindingParameterId _pushConstantId{0};
    vector<unique_ptr<Frame>> _frames;
    vector<unique_ptr<ImGuiTexture>> _aliveTexs;
};

class ImGuiSystem : public AppSubsystem {
public:
    struct ViewportWindow {
        ImGuiViewport* Viewport{nullptr};
        AppWindow* Window{nullptr};
        vector<sigslot::scoped_connection> Connections;

        void AttachInput(ImGuiSystem* system);
        NativeWindow* GetWindow() const noexcept;
        render::SwapChain* GetSwapChain() const noexcept;
        render::TextureView* GetOrCreateBackBufferView(const render::SwapChainFrame& frame, uint32_t ownerFlightIndex) const noexcept;
    };

    ImGuiSystem() noexcept = default;
    ImGuiSystem(
        ImGuiContextRAII context,
        NativeWindow* window);
    ImGuiSystem(const ImGuiSystem&) = delete;
    ImGuiSystem(ImGuiSystem&&) = delete;
    ImGuiSystem& operator=(const ImGuiSystem&) = delete;
    ImGuiSystem& operator=(ImGuiSystem&&) = delete;
    ~ImGuiSystem() noexcept;

    bool IsValid() const noexcept;
    void Destroy() noexcept;
    /// 显式开始一帧 ImGui 录制。返回 true 时调用方可直接调用 ImGui::* API,
    /// 并必须在同一帧调用 End()。
    bool Begin(const AppUpdateContext& ctx);
    /// 结束当前 ImGui 录制帧并提取 draw data。
    void End();

    void OnInit(Application& app) override;
    void OnUpdate(Application& app, const AppUpdateContext& ctx) override;
    bool OnRender(Application& app, AppFrameContext& ctx) override;
    void OnRenderComplete(Application& app, const AppRenderCompleteContext& ctx) override;
    void OnSwapChainRecreate(Application& app, const AppSwapChainRecreateContext& ctx) override;
    void OnShutdown(Application& app) override;
    RuntimeTypeId GetTypeId() const noexcept override;

    bool BeginFrame(uint32_t frameIndex, float deltaTimeSeconds);
    void EndFrame();

    /// 游戏线程：EndFrame 后提取本帧全部 viewport 的绘制数据到 flight 槽位。
    void ExtractDrawData(uint32_t frameIndex);

    /// 渲染线程：遇到全部 ImGui viewport 窗口，逐个 acquire/barrier/开 RenderPass/画 UI/收尾 barrier。
    /// 每个 viewport 都会先交给 Application::RenderViewContent 录制 UI 背后的应用内容。
    bool RenderViewports(Application& app, AppFrameContext& ctx);

    /// 渲染线程：该 flight 渲染完成后释放本帧临时资源。
    void NotifyRenderComplete(uint32_t frameIndex);

    /// 创建或更新一张外部纹理（如离屏 RT）供 ImGui::Image 使用。
    /// flightIndex 必须传入当前帧的 flight 序号：外部纹理按 flight 持有独立描述符集，
    /// 避免在命令缓冲飞行中改写仍被引用的描述符集（VUID-vkUpdateDescriptorSets-None-03047）。
    ImTextureID CreateOrUpdateExternalTexture(ImTextureID textureId, uint32_t flightIndex, render::TextureView* srv);

    /// swapchain 重建通知转发给 renderer。
    void HandleSwapChainRecreate(const AppSwapChainRecreateContext& ctx);

    static Nullable<unique_ptr<ImGuiSystem>> Create(const ImGuiSystemDescriptor& desc);

private:
    bool Initialize(const ImGuiSystemDescriptor& desc);

    bool CreateViewportSwapChainTarget(ViewportWindow* viewportWindow, ImGuiViewport* viewport) noexcept;
    void RequestViewportSwapChainCreate(ViewportWindow* viewportWindow) noexcept;
#ifdef RADRAY_PLATFORM_WINDOWS
    bool IsAnyImGuiWindowFocused() const noexcept;
    void UpdateMouseState();
#endif
    void CreatePlatformWindow(ImGuiViewport* viewport);
    void DestroyPlatformWindow(ImGuiViewport* viewport);
    void InitRendererBackend();
    void InitNativePlatform(AppWindow* mainAppWindow);

    static void PlatformCreateWindowCallback(ImGuiViewport* viewport);
    static void PlatformDestroyWindowCallback(ImGuiViewport* viewport);
    static void RendererCreateWindowCallback(ImGuiViewport* viewport);
    static void RendererDestroyWindowCallback(ImGuiViewport* viewport);

    ImGuiContextRAII _context;
    NativeWindow* _window;
    unique_ptr<ImGuiRenderer> _renderer;
    vector<unique_ptr<ViewportWindow>> _viewportWindows;
    WindowManager* _windowManager{nullptr};
    render::CommandQueue* _directQueue{nullptr};
    render::TextureFormat _renderTargetFormat{render::TextureFormat::UNKNOWN};
    uint32_t _flightDataCount{0};
    uint32_t _backBufferCount{0};
    render::PresentMode _presentMode{render::PresentMode::FIFO};
    uint32_t _activeFrameIndex{std::numeric_limits<uint32_t>::max()};
    bool _frameActive{false};
    bool _leftCtrl{false};
    bool _rightCtrl{false};
    bool _leftShift{false};
    bool _rightShift{false};
    bool _leftAlt{false};
    bool _rightAlt{false};
    bool _leftSuper{false};
    bool _rightSuper{false};
};

template <>
struct RuntimeTypeTrait<ImGuiSystem> {
    static constexpr RuntimeTypeId value{0x31e624a6, 0x1f92, 0x4d9f, 0x9b, 0x68, 0x9b, 0x72, 0x25, 0x08, 0x5d, 0x9f};
};

ImGuiKey MapKeyboardToImGuiKey(KeyCode key) noexcept;
int MapMouseButtonToImGui(MouseButton button) noexcept;

std::span<const std::byte> GetImGuiHLSL() noexcept;
std::span<const std::byte> GetImGuiVertexShaderDXIL() noexcept;
std::span<const std::byte> GetImGuiPixelShaderDXIL() noexcept;
std::span<const std::byte> GetImGuiVertexShaderSPIRV() noexcept;
std::span<const std::byte> GetImGuiPixelShaderSPIRV() noexcept;

}  // namespace radray

#endif
