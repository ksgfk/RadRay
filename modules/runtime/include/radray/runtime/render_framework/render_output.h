#pragma once

#include <atomic>
#include <thread>

#include <radray/render/rhi.h>

namespace radray {

struct RenderOutputId {
    uint64_t Value{0};
    bool IsValid() const noexcept { return Value != 0; }
    friend bool operator==(const RenderOutputId&, const RenderOutputId&) = default;
};

struct RenderOutputIdHash {
    size_t operator()(RenderOutputId id) const noexcept { return std::hash<uint64_t>{}(id.Value); }
};

enum class RenderOutputKind : uint8_t { Presentation,
                                        ExternalColorTexture };

struct RenderOutputInfo {
    RenderOutputId Id;
    RenderOutputKind Kind{RenderOutputKind::Presentation};
    string Name;
    uint32_t Width{0}, Height{0};
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
    uint32_t SampleCount{1};
    bool Active{false};
};

struct ExternalRenderOutputDesc {
    string Name;
    Nullable<render::Texture*> Texture{nullptr};
    Nullable<render::TextureView*> ColorAttachmentView{nullptr};
    render::TextureStates CurrentState{render::TextureState::Undefined};
    render::TextureStates RequiredFinalState{render::TextureState::ShaderRead};
    bool PreserveContents{false};
};

struct RenderSurfaceFrame {
    RenderOutputId Id;
    render::Texture* Texture;
    render::TextureView* ColorAttachmentView;
    render::TextureDescriptor Desc;
    render::TextureStates CurrentState{render::TextureState::Undefined};
    render::TextureStates RequiredFinalState{render::TextureState::UNKNOWN};
    bool PreserveContents{false};
    bool Written{false};
};

class RenderOutputRegistry {
public:
    RenderOutputRegistry();
    RenderOutputRegistry(const RenderOutputRegistry&) = delete;
    RenderOutputRegistry& operator=(const RenderOutputRegistry&) = delete;

    /// Host-only lifecycle gate. Set true only after the render thread has stopped consuming plans.
    void SetRenderIdle(bool idle) noexcept;
    RenderOutputId RegisterPresentation(string name, const render::TextureDescriptor& desc);
    RenderOutputId RegisterExternal(const ExternalRenderOutputDesc& desc);
    bool Unregister(RenderOutputId id);
    bool UpdatePresentation(RenderOutputId id, uint32_t width, uint32_t height, bool active);
    bool UpdateExternalOutputState(RenderOutputId id, render::TextureStates state, bool preserveContents);
    vector<RenderOutputInfo> GetGameThreadInfos() const;
    Nullable<const RenderOutputInfo*> Find(RenderOutputId id) const noexcept;
    std::optional<RenderSurfaceFrame> ResolveExternal(RenderOutputId id) const;
    void CommitExternalState(const RenderSurfaceFrame& surface);
    void Clear();

private:
    struct Record {
        RenderOutputInfo Info;
        std::optional<ExternalRenderOutputDesc> External;
    };
    void AssertMutable() const noexcept;
    vector<Record> _records;
    std::thread::id _gameThread;
    bool _renderIdle{true};
};

}  // namespace radray
