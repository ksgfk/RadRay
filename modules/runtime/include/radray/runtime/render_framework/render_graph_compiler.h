#pragma once

#include <span>
#include <radray/types.h>

namespace radray {

inline constexpr uint32_t RgInvalidIndex = UINT32_MAX;

struct RenderGraphCompileOptions {
    bool CullPasses{true};
    bool ReuseResources{true};
    bool MergeRasterPasses{true};
    bool OptimizeAttachmentStores{true};
    bool EliminateBarriers{true};
    bool BatchBarriers{true};
};

/// One initialized or discarded content range. Predecessor describes storage ordering,
/// while pass Reads describe content dependencies; they must not be conflated for culling.
struct RgResourceVersionNode {
    uint32_t Resource{RgInvalidIndex};
    uint32_t Cell{0};
    uint32_t Version{0};
    uint32_t Producer{RgInvalidIndex};
    uint32_t Predecessor{RgInvalidIndex};
    bool Initialized{false};
};

struct RgExecutionNode {
    vector<uint32_t> Reads;
    vector<uint32_t> Writes;
    bool SideEffect{false};
};

struct RgCompilerDiagnostic {
    string Code;
    string Message;
    uint32_t Pass{RgInvalidIndex};
    uint32_t Resource{RgInvalidIndex};
};

struct RgCompiledPass {
    vector<uint32_t> Reads;
    vector<uint32_t> Writes;
    vector<uint32_t> DataDependencies;
    vector<uint32_t> HazardDependencies;
    bool Live{false};
    string LivenessReason;
};

struct RgResourceLifetime {
    int32_t FirstUse{-1};
    int32_t LastUse{-1};
};

struct CompiledRenderGraph {
    vector<RgResourceVersionNode> Versions;
    vector<RgCompiledPass> Passes;
    vector<uint32_t> ExecutionOrder;
    vector<RgResourceLifetime> Lifetimes;
    vector<RgCompilerDiagnostic> Diagnostics;
    bool IsValid() const noexcept { return Diagnostics.empty(); }
};

/// Pure CPU compilation. Every index is local to this input, and roots name the exact
/// content versions observed outside the graph. No RHI object or shader program is accessed.
CompiledRenderGraph CompileRenderGraph(
    uint32_t resourceCount,
    std::span<const RgResourceVersionNode> versions,
    std::span<const RgExecutionNode> passes,
    std::span<const uint32_t> roots,
    const RenderGraphCompileOptions& options = {});

}  // namespace radray
