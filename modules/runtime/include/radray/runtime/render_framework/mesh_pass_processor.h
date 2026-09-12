#pragma once

#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/cpu_draw_record.h>

#include <optional>

namespace radray {

enum class MeshPassRejectReason : uint8_t { ProcessorRejected,
                                            MissingPass,
                                            InvalidBindings,
                                            InvalidGeometry,
                                            PrepareResourceFailed };

/// One indexed draw or explicit dynamic command per batch. Rejection or multiple publications discard the candidate.
class MeshPassDrawListContext {
public:
    void AddCommand(MeshDrawCommand&& command, Nullable<FrameDrawResources*> resources = nullptr) {
        if (_published || _rejected) {
            Reject(MeshPassRejectReason::ProcessorRejected);
            return;
        }
        _command = std::move(command);
        _resources = resources;
        _published = true;
    }
    void Reject(MeshPassRejectReason reason) noexcept {
        _command.reset();
        _resources = nullptr;
        _binding = {};
        _reason = reason;
        _rejected = true;
    }
    bool HasCommand() const noexcept { return _command.has_value(); }
    bool HasDraw() const noexcept { return HasCommand() || (_resources && _binding.IsValid()); }
    void AddRecord(FrameDrawResources& resources, FrameDrawBindingId binding) {
        if (_published || _rejected || !resources.IsValid(binding)) {
            Reject(MeshPassRejectReason::ProcessorRejected);
            return;
        }
        _resources = &resources;
        _binding = binding;
        _published = true;
    }
    bool AppendTo(RendererList& list, const RenderSceneSnapshot& scene, uint32_t recordIndex) {
        if (_command) {
            const bool success = list.AppendDynamic(std::move(*_command), _resources);
            _command.reset();
            _resources = nullptr;
            return success;
        }
        if (!_resources) return false;
        const bool success = list.AppendStatic(scene, recordIndex, *_resources.Get(), _binding);
        _resources = nullptr;
        return success;
    }
    MeshPassRejectReason Reason() const noexcept { return _reason; }
    /// Move the candidate directly into final storage. Successful consumption clears HasCommand().
    /// Publication remains single-use, including after consumption; no internal reference escapes.
    bool AppendCommandTo(vector<MeshDrawCommand>& commands) {
        if (!_command) return false;
        commands.push_back(std::move(*_command));
        _command.reset();
        _resources = nullptr;
        return true;
    }

private:
    friend bool BuildRendererList(const RendererListDesc&, MeshPassProcessor&, RendererList&);
    std::optional<MeshDrawCommand> _command;
    Nullable<FrameDrawResources*> _resources{nullptr};
    FrameDrawBindingId _binding;
    MeshPassRejectReason _reason{MeshPassRejectReason::ProcessorRejected};
    bool _rejected{false};
    bool _published{false};
};

class MeshPassProcessor {
public:
    virtual ~MeshPassProcessor() noexcept = default;
    virtual void AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                              const MeshBatch& batch, MeshPassDrawListContext& out) = 0;
    virtual void PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                               const DrawRecord& record, MeshPassDrawListContext& out);
};

}  // namespace radray
