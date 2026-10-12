#pragma once

#include <functional>
#include <radray/render/rhi.h>

namespace radray {

enum class RasterNodeKind : uint8_t { Raster,
                                      Fullscreen,
                                      Copy,
                                      Clear };
enum class RasterAccess : uint8_t { Read,
                                    Write,
                                    ReadWrite };
using RasterResourceId = uint32_t;
using RasterNodeId = uint32_t;

/// Whole-texture hazards on one graphics queue. The caller owns textures through completion.
/// A null texture permits CPU-only compilation; it cannot be recorded.
struct RasterGraphResource {
    Nullable<render::Texture*> Texture{nullptr};
    render::TextureStates Enter{render::TextureState::Undefined};
    render::TextureStates Exit{render::TextureState::ShaderRead};
    bool Initialized{false};
};
struct RasterGraphUse {
    RasterResourceId Resource;
    RasterAccess Access;
    render::TextureStates State;
};
struct RasterGraphNode {
    RasterNodeKind Kind{RasterNodeKind::Raster};
    vector<RasterGraphUse> Uses{};
    vector<RasterNodeId> After{};
    /// Records only the node body. Graph owns texture transitions, including entry/exit.
    /// Borrowed inputs and callback captures must survive synchronous Record.
    std::function<bool(render::CommandBuffer&)> Record{};
};
struct CompiledRasterGraph {
    struct Step {
        RasterNodeId Node;
        vector<render::BarrierTextureDescriptor> Barriers;
    };
    vector<RasterGraphResource> Resources;
    vector<RasterGraphNode> Nodes;
    vector<Step> Steps;
    vector<render::BarrierTextureDescriptor> ExitBarriers;
    /// Does not allocate, submit, present or update persistent resource state.
    bool Record(render::CommandBuffer& command) const;
};
class RasterGraph {
public:
    RasterResourceId AddResource(RasterGraphResource resource);
    RasterNodeId AddNode(RasterGraphNode node);
    /// Pure CPU scheduling. Rejects cycles, duplicate resource uses and reads before writes.
    /// Insertion order resolves resource hazards; explicit After edges may add dependencies.
    std::optional<CompiledRasterGraph> Compile() const;

private:
    vector<RasterGraphResource> _resources;
    vector<RasterGraphNode> _nodes;
};

}  // namespace radray
