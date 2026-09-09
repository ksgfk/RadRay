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

/// One indexed command per batch. Rejection or multiple publications discard the candidate.
class MeshPassDrawListContext {
public:
    void AddCommand(MeshDrawCommand command) {
        if (_command || _rejected) {
            Reject(MeshPassRejectReason::ProcessorRejected);
            return;
        }
        _command = std::move(command);
    }
    void Reject(MeshPassRejectReason reason) noexcept {
        _command.reset();
        _reason = reason;
        _rejected = true;
    }
    bool HasCommand() const noexcept { return _command.has_value(); }
    MeshPassRejectReason Reason() const noexcept { return _reason; }
    MeshDrawCommand TakeCommand() { return std::move(*_command); }

private:
    friend bool BuildRendererList(const RendererListDesc&, MeshPassProcessor&, RendererList&);
    std::optional<MeshDrawCommand> _command;
    MeshPassRejectReason _reason{MeshPassRejectReason::ProcessorRejected};
    bool _rejected{false};
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
