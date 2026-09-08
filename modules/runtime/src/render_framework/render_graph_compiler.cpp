#include <radray/runtime/render_framework/render_graph_compiler.h>

#include <algorithm>
#include <functional>
#include <fmt/format.h>

namespace radray {
namespace {
void AddDependency(vector<uint32_t>& dependencies, uint32_t dependency, uint32_t pass) {
    if (dependency != RgInvalidIndex && dependency != pass &&
        std::find(dependencies.begin(), dependencies.end(), dependency) == dependencies.end())
        dependencies.push_back(dependency);
}
}  // namespace

CompiledRenderGraph CompileRenderGraph(uint32_t resourceCount,
                                       std::span<const RgResourceVersionNode> versions,
                                       std::span<const RgExecutionNode> passes,
                                       std::span<const uint32_t> roots,
                                       const RenderGraphCompileOptions& options) {
    RenderGraphCompilerWorkspace workspace;
    return CompileRenderGraph(resourceCount, versions, passes, roots, options, workspace);
}

CompiledRenderGraph CompileRenderGraph(uint32_t resourceCount,
                                       std::span<const RgResourceVersionNode> versions,
                                       std::span<const RgExecutionNode> passes,
                                       std::span<const uint32_t> roots,
                                       const RenderGraphCompileOptions& options,
                                       RenderGraphCompilerWorkspace& workspace) {
    CompiledRenderGraph result;
    result.Versions.assign(versions.begin(), versions.end());
    result.Passes.resize(passes.size());
    result.Lifetimes.resize(resourceCount);
    const auto error = [&](std::string_view code, string message, uint32_t pass = RgInvalidIndex, uint32_t resource = RgInvalidIndex) {
        result.Diagnostics.push_back({string{code}, std::move(message), pass, resource});
    };
    auto& readers = workspace.Readers;
    auto& writeOwners = workspace.WriteOwners;
    auto& successors = workspace.Successors;
    readers.resize(versions.size());
    for (auto& entries : readers) entries.clear();
    writeOwners.assign(versions.size(), RgInvalidIndex);
    successors.assign(versions.size(), RgInvalidIndex);
    for (uint32_t v = 0; v < versions.size(); ++v) {
        const auto& value = versions[v];
        if (value.Resource >= resourceCount || (value.Producer != RgInvalidIndex && value.Producer >= passes.size()))
            error("InvalidVersion", fmt::format("Version node {} has an invalid resource or producer", v), value.Producer);
        if (value.Predecessor != RgInvalidIndex &&
            (value.Predecessor >= v || versions[value.Predecessor].Resource != value.Resource || versions[value.Predecessor].Cell != value.Cell))
            error("InvalidVersionChain", fmt::format("Version node {} has an invalid storage predecessor", v), value.Producer, value.Resource);
        else if (value.Predecessor != RgInvalidIndex) {
            auto& successor = successors[value.Predecessor];
            if (successor != RgInvalidIndex)
                error("VersionBranch", "Overlapping in-place content versions branch; use an explicit copy for independent storage", value.Producer, value.Resource);
            successor = v;
        }
    }
    if (!result.IsValid()) return result;
    for (uint32_t p = 0; p < passes.size(); ++p) {
        result.Passes[p].Reads = passes[p].Reads;
        result.Passes[p].Writes = passes[p].Writes;
        for (const auto v : passes[p].Reads) {
            if (v >= versions.size()) {
                error("InvalidVersion", "Read references an unknown version", p);
                continue;
            }
            const auto& value = versions[v];
            if (!value.Initialized)
                error("UninitializedRead", fmt::format("Version {} cell {} has no valid contents", value.Version, value.Cell), p, value.Resource);
            if (value.Producer == p)
                error("Feedback", "A pass cannot consume its own output version", p, value.Resource);
            AddDependency(result.Passes[p].DataDependencies, value.Producer, p);
            AddDependency(readers[v], p, RgInvalidIndex);
        }
        for (const auto v : passes[p].Writes) {
            if (v >= versions.size() || versions[v].Producer != p || writeOwners[v] != RgInvalidIndex) {
                error("InvalidProducer", "A written version must have exactly one matching producer", p);
                continue;
            }
            writeOwners[v] = p;
        }
    }
    for (uint32_t v = 0; v < versions.size(); ++v)
        if (versions[v].Producer != RgInvalidIndex && writeOwners[v] == RgInvalidIndex)
            error("MissingProducer", "A produced version is absent from its pass writes", versions[v].Producer, versions[v].Resource);
    auto& pending = workspace.Pending;
    pending.clear();
    for (uint32_t p = 0; p < passes.size(); ++p) {
        if (passes[p].SideEffect || !options.CullPasses) {
            pending.push_back(p);
            result.Passes[p].LivenessReason = passes[p].SideEffect ? "explicit side effect" : "culling disabled";
        }
    }
    for (const auto v : roots) {
        if (v >= versions.size()) {
            error("InvalidRoot", "An exported version does not exist");
            continue;
        }
        const auto& value = versions[v];
        if (!value.Initialized) error("UninitializedExport", "An exported range has no valid contents", value.Producer, value.Resource);
        if (value.Producer != RgInvalidIndex) {
            pending.push_back(value.Producer);
            result.Passes[value.Producer].LivenessReason = "observable final content";
        }
    }
    if (!result.IsValid()) return result;
    while (!pending.empty()) {
        const uint32_t p = pending.back();
        pending.pop_back();
        auto& pass = result.Passes[p];
        if (pass.Live) continue;
        pass.Live = true;
        if (pass.LivenessReason.empty()) pass.LivenessReason = "content consumed by a live pass";
        pending.insert(pending.end(), pass.DataDependencies.begin(), pass.DataDependencies.end());
    }
    for (uint32_t p = 0; p < passes.size(); ++p) {
        auto& compiled = result.Passes[p];
        if (!compiled.Live) {
            compiled.LivenessReason = "unconsumed or overwritten content";
            continue;
        }
        compiled.HazardDependencies = compiled.DataDependencies;
        for (const auto v : passes[p].Writes) {
            auto predecessor = versions[v].Predecessor;
            while (predecessor != RgInvalidIndex) {
                for (const auto reader : readers[predecessor])
                    if (result.Passes[reader].Live) AddDependency(compiled.HazardDependencies, reader, p);
                const auto producer = versions[predecessor].Producer;
                if (producer != RgInvalidIndex && result.Passes[producer].Live) {
                    AddDependency(compiled.HazardDependencies, producer, p);
                    break;
                }
                predecessor = versions[predecessor].Predecessor;
            }
        }
        std::sort(compiled.DataDependencies.begin(), compiled.DataDependencies.end());
        std::sort(compiled.HazardDependencies.begin(), compiled.HazardDependencies.end());
    }
    auto& consumers = workspace.Consumers;
    auto& indegrees = workspace.Indegrees;
    auto& ready = workspace.Ready;
    consumers.resize(passes.size());
    for (auto& entries : consumers) entries.clear();
    indegrees.assign(passes.size(), 0);
    ready.clear();
    for (uint32_t p = 0; p < passes.size(); ++p) {
        if (!result.Passes[p].Live) continue;
        auto& dependencies = workspace.Dependencies;
        dependencies = result.Passes[p].DataDependencies;
        for (const auto dependency : result.Passes[p].HazardDependencies) AddDependency(dependencies, dependency, p);
        indegrees[p] = static_cast<uint32_t>(dependencies.size());
        for (const auto dependency : dependencies) consumers[dependency].push_back(p);
        if (dependencies.empty()) {
            ready.push_back(p);
            std::push_heap(ready.begin(), ready.end(), std::greater<uint32_t>{});
        }
    }
    while (!ready.empty()) {
        std::pop_heap(ready.begin(), ready.end(), std::greater<uint32_t>{});
        const auto p = ready.back();
        ready.pop_back();
        result.ExecutionOrder.push_back(p);
        for (const auto consumer : consumers[p]) {
            if (--indegrees[consumer] != 0) continue;
            ready.push_back(consumer);
            std::push_heap(ready.begin(), ready.end(), std::greater<uint32_t>{});
        }
    }
    for (uint32_t p = 0; p < passes.size(); ++p)
        if (result.Passes[p].Live && indegrees[p] != 0)
            error("DependencyCycle", "Pass participates in an unsatisfied content/storage dependency cycle", p);
    for (uint32_t order = 0; order < result.ExecutionOrder.size(); ++order) {
        const auto p = result.ExecutionOrder[order];
        const auto touch = [&](uint32_t v) {
            auto& lifetime = result.Lifetimes[versions[v].Resource];
            if (lifetime.FirstUse < 0) lifetime.FirstUse = static_cast<int32_t>(order);
            lifetime.LastUse = static_cast<int32_t>(order);
        };
        for (const auto v : passes[p].Reads) touch(v);
        for (const auto v : passes[p].Writes) touch(v);
    }
    return result;
}

}  // namespace radray
