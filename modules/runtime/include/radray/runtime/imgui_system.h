#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <cstddef>
#include <span>

#include <imgui.h>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

class Application;
class AppWindow;
class GpuAsyncContext;
class ImGuiSystem;
class ImGuiTextureBinding;

enum class ImGuiRenderCommandKind {
    DrawIndexed,
    ResetRenderState
};

class ImGuiRenderCommandSnapshot {
public:
    ImGuiRenderCommandKind Kind{ImGuiRenderCommandKind::DrawIndexed};
    ImVec4 ClipRect{};
    ImTextureID TexID{ImTextureID_Invalid};
    uint32_t ElemCount{0};
    uint32_t IdxOffset{0};
    uint32_t VtxOffset{0};
};

class ImGuiTextureUploadSnapshot {
public:
    ImGuiTextureBinding* Binding{nullptr};
    vector<byte> Pixels;
    uint32_t Width{0};
    uint32_t Height{0};
    uint64_t RowPitch{0};
    bool IsCreate{false};
};

class ImGuiDrawListSnapshot {
public:
    void Clear() noexcept;

public:
    vector<ImDrawVert> Vertices;
    vector<ImDrawIdx> Indices;
    vector<ImGuiRenderCommandSnapshot> Commands;
};

class ImGuiRenderSnapshot {
public:
    void Clear() noexcept;

public:
    ImVec2 DisplayPos{};
    ImVec2 DisplaySize{};
    ImVec2 FramebufferScale{};
    int32_t TotalIdxCount{0};
    int32_t TotalVtxCount{0};
    vector<ImGuiDrawListSnapshot> DrawLists;
    vector<ImGuiTextureUploadSnapshot> TextureUploads;
    bool Valid{false};
};

class ImGuiUploadedDrawList {
public:
    render::VertexBufferView VertexBuffer{};
    render::IndexBufferView IndexBuffer{};
};

class ImGuiUploadedRenderData {
public:
    void Clear() noexcept {
        Uploaded = false;
        DrawLists.clear();
    }

public:
    vector<ImGuiUploadedDrawList> DrawLists;
    bool Uploaded{false};
};

class ImGuiTextureBinding {
public:
    GpuTextureHandle Texture{};
    GpuTextureViewHandle View{};
    GpuDescriptorSetHandle DescriptorSet{};
    render::TextureState State{render::TextureState::Undefined};
};

class ImGuiViewportRendererData {
public:
    ImGuiViewport* Viewport{nullptr};
    AppWindow* Window{nullptr};
    vector<ImGuiRenderSnapshot> Mailboxes;
    vector<ImGuiUploadedRenderData> UploadedMailboxes;
};

class ImGuiSystemDescriptor {
public:
    Application* App{nullptr};
    AppWindow* MainWnd{nullptr};
    bool EnableViewports{false};
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
    explicit ImGuiRenderer(Application* app, AppWindow* mainWnd);
    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer(ImGuiRenderer&&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(ImGuiRenderer&&) = delete;
    ~ImGuiRenderer() noexcept;

    static Nullable<unique_ptr<ImGuiRenderer>> Create(Application* app, AppWindow* mainWnd);

    static void CreateWindow(ImGuiViewport* viewport);
    static void DestroyWindow(ImGuiViewport* viewport);
    static void SetWindowSize(ImGuiViewport* viewport, ImVec2 size);
    static void RenderWindow(ImGuiViewport* viewport, void* renderArg);
    static void SwapBuffers(ImGuiViewport* viewport, void* renderArg);

public:
    Application* _app{nullptr};
    AppWindow* _mainWnd{nullptr};
    ImGuiSystem* _system{nullptr};
    GpuShaderHandle _vs{};
    GpuShaderHandle _ps{};
    GpuRootSignatureHandle _rs{};
    GpuGraphicsPipelineStateHandle _pso{};
    vector<unique_ptr<ImGuiTextureBinding>> _textureBindings;
};

class ImGuiSystem {
public:
    ImGuiSystem(Application* app, AppWindow* mainWnd, unique_ptr<ImGuiContextRAII> context);
    ImGuiSystem(const ImGuiSystem&) = delete;
    ImGuiSystem(ImGuiSystem&&) = delete;
    ImGuiSystem& operator=(const ImGuiSystem&) = delete;
    ImGuiSystem& operator=(ImGuiSystem&&) = delete;
    ~ImGuiSystem() noexcept;

    void NewFrame();
    void DestroyTextureBinding(ImGuiTextureBinding* binding) noexcept;
    void DestroyTextureBindings() noexcept;
    void PrepareRenderData(AppWindow* window, uint32_t mailboxSlot);
    void Upload(AppWindow* window, uint32_t mailboxSlot, GpuAsyncContext* context, render::CommandBuffer* cmd);
    void Render(AppWindow* window, uint32_t mailboxSlot, render::GraphicsCommandEncoder* encoder);

    static Nullable<unique_ptr<ImGuiSystem>> Create(const ImGuiSystemDescriptor& desc);

public:
    Application* _app;
    AppWindow* _mainWnd;
    unique_ptr<ImGuiContextRAII> _context;
    unique_ptr<ImGuiRenderer> _renderer;
    vector<unique_ptr<ImGuiViewportRendererData>> _viewportRendererData;
};

std::span<const std::byte> GetImGuiHLSL() noexcept;
std::span<const std::byte> GetImGuiVertexShaderDXIL() noexcept;
std::span<const std::byte> GetImGuiPixelShaderDXIL() noexcept;
std::span<const std::byte> GetImGuiVertexShaderSPIRV() noexcept;
std::span<const std::byte> GetImGuiPixelShaderSPIRV() noexcept;

}  // namespace radray

#endif
