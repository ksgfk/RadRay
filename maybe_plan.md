可以，现代版就不要走旧的 `io.Fonts->GetTexDataAsRGBA32() + SetTexID()` 一次性字体纹理路径了。按 ImGui 1.92 的 backend 协议设计：`ImGuiRenderer` 同时支持 `RendererHasTextures`、`RendererHasViewports`、`RendererHasVtxOffset`。

**总体结构**

```cpp
class ImGuiRendererDescriptor {
public:
    GpuRuntime* Runtime{nullptr};
    render::TextureFormat BackBufferFormat{render::TextureFormat::UNKNOWN};
    uint32_t BackBufferCount{0};
    uint32_t FlightFrameCount{1};
    uint32_t QueueSlot{0};
    bool EnableViewports{false};
};

class ImGuiRenderer {
public:
    static Nullable<unique_ptr<ImGuiRenderer>> Create(const ImGuiRendererDescriptor& desc);
    ~ImGuiRenderer() noexcept;

    void NewFrame();
    void RenderDrawData(ImDrawData* drawData, GpuFrameContext* frame);
    void NotifySubmitted(const GpuTask& task); // 主 viewport submit 后调用
    void Shutdown() noexcept;

    ImTextureID RegisterTexture(render::TextureView* view, render::Sampler* sampler);
    void UnregisterTexture(ImTextureID id);

private:
    void RegisterBackend();
    void CreateDeviceObjects();
    void ProcessTextureRequests(ImDrawData* drawData, GpuAsyncContext* context);
    void UpdateTexture(ImTextureData* tex, GpuAsyncContext* context);
    void DestroyTexture(ImTextureData* tex);

    static void RendererCreateWindow(ImGuiViewport* viewport);
    static void RendererDestroyWindow(ImGuiViewport* viewport);
    static void RendererSetWindowSize(ImGuiViewport* viewport, ImVec2 size);
    static void RendererRenderWindow(ImGuiViewport* viewport, void* renderArg);
    static void RendererSwapBuffers(ImGuiViewport* viewport, void* renderArg);
};
```

**Backend 注册**

```cpp
ImGuiIO& io = ImGui::GetIO();
io.BackendRendererUserData = this;
io.BackendRendererName = "radray_imgui";
io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

if (_enableViewports) {
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;

    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    platformIo.Renderer_CreateWindow = RendererCreateWindow;
    platformIo.Renderer_DestroyWindow = RendererDestroyWindow;
    platformIo.Renderer_SetWindowSize = RendererSetWindowSize;
    platformIo.Renderer_RenderWindow = RendererRenderWindow;
    platformIo.Renderer_SwapBuffers = RendererSwapBuffers;
}
```

`ImGuiSystem::Create()` 里要先创建 renderer，再打开 `ImGuiConfigFlags_ViewportsEnable`，否则 ImGui 会在 renderer backend 未声明能力时进入 viewport 路径。

**Texture 设计**

`ImTextureID` 不直接放 `GpuTextureHandle`，放一个稳定指针：

```cpp
struct ImGuiTextureBinding {
    unique_ptr<render::Texture> OwnedTexture;      // ImGui-managed texture
    unique_ptr<render::TextureView> View;
    unique_ptr<render::DescriptorSet> DescriptorSet;
    render::Sampler* Sampler{nullptr};
    GpuTask LastUsedTask{};
    bool OwnedByImGui{false};
};
```

对于 ImGui 自己请求的纹理：

```cpp
tex->BackendUserData = binding;
tex->SetTexID(reinterpret_cast<ImTextureID>(binding));
tex->SetStatus(ImTextureStatus_OK);
```

绘制时：

```cpp
auto* binding = reinterpret_cast<ImGuiTextureBinding*>(pcmd->GetTexID());
encoder->BindDescriptorSet(TextureSetIndex, binding->DescriptorSet.get());
```

用户纹理走 `RegisterTexture()`，也返回同样的 `ImTextureID`。这样 `ImGui::Image(ImTextureRef(id), size)` 和 font atlas 使用同一条路径。

**RendererHasTextures 核心**

每次 `RenderDrawData()` 开头必须处理：

```cpp
void ImGuiRenderer::ProcessTextureRequests(ImDrawData* drawData, GpuAsyncContext* context) {
    if (drawData->Textures == nullptr) {
        return;
    }

    for (ImTextureData* tex : *drawData->Textures) {
        if (tex->Status == ImTextureStatus_WantCreate ||
            tex->Status == ImTextureStatus_WantUpdates) {
            UpdateTexture(tex, context);
        } else if (tex->Status == ImTextureStatus_WantDestroy &&
                   tex->UnusedFrames >= static_cast<int>(_flightFrameCount)) {
            DestroyTexture(tex);
        }
    }
}
```

`WantCreate`：

- 创建 `render::Texture`
- 创建 `render::TextureView`
- 创建 descriptor set
- 写入 texture view + sampler
- 设置 `BackendUserData` 和 `TexID`
- 继续执行 upload

`WantUpdates`：

- 用 `tex->UpdateRect` 或 `tex->Updates`
- 当前 RadRay 的 `CopyBufferToTexture()` 只支持整 subresource，不支持 x/y partial region
- 第一版建议直接上传整张 texture，功能完整，只是低效
- 以后再给 RHI 加 `CopyBufferToTextureRegion()`

格式策略：

- `ImTextureFormat_RGBA32` -> `render::TextureFormat::RGBA8_UNORM`
- `ImTextureFormat_Alpha8` -> 后端 CPU 侧扩展成 RGBA8，写 `(255,255,255,alpha)`，避免 shader 为 Alpha8 特判

`UpdateTexture()` 录入到当前 frame/async context：

```cpp
// 1. 创建 upload buffer
// 2. map 拷贝像素
// 3. texture barrier: ShaderResource/Undefined -> CopyDestination
// 4. CopyBufferToTexture(full subresource)
// 5. texture barrier: CopyDestination -> ShaderResource
// 6. tex->SetStatus(ImTextureStatus_OK)
```

**DrawData 渲染**

`RenderDrawData()` 顺序：

```cpp
ProcessTextureRequests(drawData, frame);

UploadVerticesAndIndices(drawData, frame);

BeginRenderPass(backbuffer, LoadAction::Load);

BindPipeline();
BindRootSignature();
PushProjection(drawData->DisplayPos, drawData->DisplaySize);

for each draw list:
    for each cmd:
        if callback: handle
        bind texture descriptor from pcmd->GetTexID()
        set scissor using DisplayPos/FramebufferScale
        DrawIndexed(..., pcmd->VtxOffset, pcmd->IdxOffset)
```

scissor 必须用 absolute viewport 坐标修正：

```cpp
clip.x = (pcmd->ClipRect.x - drawData->DisplayPos.x) * drawData->FramebufferScale.x;
clip.y = (pcmd->ClipRect.y - drawData->DisplayPos.y) * drawData->FramebufferScale.y;
```

**Viewport 管理**

secondary viewport 数据：

```cpp
struct ImGuiViewportRendererData {
    unique_ptr<GpuSurface> Surface;
    uint32_t Width{};
    uint32_t Height{};
    bool NeedRecreate{};
};
```

`RendererCreateWindow()`：

- 从 `viewport->PlatformHandleRaw` 拿 HWND
- 创建 `GpuSurface`
- 放进 `viewport->RendererUserData`

`RendererRenderWindow()`：

```cpp
auto begin = _runtime->BeginFrame(data->Surface.get());
if (begin.Status != render::SwapChainStatus::Success) return;

RenderDrawData(viewport->DrawData, begin.Context.Get());

auto submit = _runtime->SubmitFrame(begin.Context.Release());
TrackSubmitted(submit.Task);
```

`RendererSwapBuffers()` 第一版可以空实现，因为 RadRay 的 `SubmitFrame()` 已经 present。

**ImGuiSystem 每帧**

```cpp
void ImGuiSystem::NewFrame() {
    _context->SetCurrent();
    NewFrameImGuiInternal(_context->Get()); // Win32 NewFrame
    _renderer->NewFrame();
    ImGui::NewFrame();
    ImGui::DockSpaceOverViewport();
}

void ImGuiSystem::Render(GpuFrameContext* frame) {
    ImGui::Render();
    _renderer->RenderDrawData(ImGui::GetDrawData(), frame);

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}
```

主窗口 submit 后再调：

```cpp
_imguiRenderer->NotifySubmitted(submit.Task);
```

用于安全退休 ImGui-managed textures。

**关键改动点**

- 不再调用 `io.Fonts->SetTexID()`。
- 必须设置 `ImGuiBackendFlags_RendererHasTextures`。
- 每次 `RenderDrawData()` 先处理 `drawData->Textures`。
- `ImDrawCmd` 纹理统一通过 `pcmd->GetTexID()`。
- `ImTextureID` 统一解释为 `ImGuiTextureBinding*`。
- multi-viewport 的每个 HWND 独立 `GpuSurface`，独立 `BeginFrame/SubmitFrame`。
- 当前 RHI 可以先全量上传 texture；要做高效增量更新，再补 `CopyBufferToTextureRegion()`。