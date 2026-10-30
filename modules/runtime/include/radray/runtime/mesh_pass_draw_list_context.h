#pragma once

#include <radray/runtime/mesh_draw_command.h>

namespace radray {

struct MeshBatchContext;

class MeshPassDrawListContext {
public:
    virtual ~MeshPassDrawListContext() noexcept = default;

    // Copies the shared initializer into output storage. The reference must survive until FinalizeCommand returns.
    // The caller must finalize each command before adding another; references need not survive later additions.
    virtual MeshDrawCommand& AddCommand(const MeshDrawCommand& initializer) = 0;

    // Fills the selected Element's indexed draw arguments. Overrides call this before adding their own output data.
    // context is borrowed for this call; elementIndex must identify an existing Element.
    virtual void FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command);
};

// Owns commands for the current build. Appending does not clear earlier output; no persistent cache is maintained.
class DynamicMeshPassDrawListContext final : public MeshPassDrawListContext {
public:
    MeshDrawCommand& AddCommand(const MeshDrawCommand& initializer) override;

    vector<MeshDrawCommand> Commands;
};

}  // namespace radray
