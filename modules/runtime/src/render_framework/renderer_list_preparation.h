#pragma once

#include <algorithm>
#include <radray/runtime/render_framework/frame_draw_resources.h>

namespace radray {

struct PreparedRendererList::Storage {
    vector<Draw> Draws;
    vector<GroupReference> Groups;
    vector<PreparedShaderGroup> LocalGroups;
    vector<Geometry> Geometries;
    vector<std::span<const render::VertexBufferBinding>> VertexRuns;
    size_t ActiveLocalGroups{0};

    void Reset() noexcept {
        Draws.clear();
        Groups.clear();
        Geometries.clear();
        VertexRuns.clear();
        // Retain both the outer rows and spilled dynamic-offset capacity of each inactive row.
        ActiveLocalGroups = 0;
    }
    uint32_t AddLocalGroup(const PreparedShaderGroup& group) {
        const auto index = static_cast<uint32_t>(ActiveLocalGroups++);
        if (index == LocalGroups.size())
            LocalGroups.push_back(group);
        else
            LocalGroups[index] = group;
        return index;
    }
    size_t CapacityBytes() const noexcept {
        size_t bytes = Draws.capacity() * sizeof(Draw) + Groups.capacity() * sizeof(GroupReference) +
                       LocalGroups.capacity() * sizeof(PreparedShaderGroup) + Geometries.capacity() * sizeof(Geometry) +
                       VertexRuns.capacity() * sizeof(std::span<const render::VertexBufferBinding>);
        for (const auto& group : LocalGroups)
            if (group.DynamicOffsets.capacity() > group.DynamicOffsets.inline_capacity)
                bytes += group.DynamicOffsets.capacity() * sizeof(render::ShaderParameterDynamicOffset);
        return bytes;
    }
};

struct PreparedRendererList::Workspace {
    class Index {
    public:
        void Reset() noexcept {
            _count = 0;
            if (++_epoch == 0) {
                for (auto& slot : _slots) slot.Epoch = 0;
                ++_epoch;
            }
        }
        template <class Predicate>
        uint32_t Find(size_t hash, Predicate&& equal) const noexcept {
            if (_slots.empty()) return UINT32_MAX;
            size_t slot = hash & (_slots.size() - 1);
            while (_slots[slot].Epoch == _epoch) {
                if (_slots[slot].Hash == hash && equal(_slots[slot].Value)) return _slots[slot].Value;
                slot = (slot + 1) & (_slots.size() - 1);
            }
            return UINT32_MAX;
        }
        void Insert(size_t hash, uint32_t index) {
            if ((_count + 1) * 2 > _slots.size()) {
                vector<Slot> grown(std::max<size_t>(16, _slots.size() * 2));
                for (const auto& entry : _slots) {
                    if (entry.Epoch != _epoch) continue;
                    size_t slot = entry.Hash & (grown.size() - 1);
                    while (grown[slot].Epoch == _epoch) slot = (slot + 1) & (grown.size() - 1);
                    grown[slot] = entry;
                }
                _slots = std::move(grown);
            }
            size_t slot = hash & (_slots.size() - 1);
            while (_slots[slot].Epoch == _epoch) slot = (slot + 1) & (_slots.size() - 1);
            _slots[slot] = {_epoch, hash, index};
            ++_count;
        }
        size_t CapacityBytes() const noexcept { return _slots.capacity() * sizeof(Slot); }

    private:
        struct Slot {
            uint64_t Epoch{0};
            size_t Hash{0};
            uint32_t Value{UINT32_MAX};
        };
        vector<Slot> _slots;
        uint64_t _epoch{1};
        size_t _count{0};
    };
    struct PipelineEntry {
        ShaderProgram* Program;
        uint64_t StateId;
        const MaterialPipelineState* State;
        PrimitiveVertexLayoutId Layout;
        PrimitiveTopology Topology;
        render::GraphicsPipelineState* Pipeline;
    };
    struct BindingEntry {
        const ShaderProgram* Program;
        FrameDrawBindingId FrameBinding;
        uint32_t First, Count;
    };
    struct ProgramEntry {
        const ShaderProgram* Program;
        uint32_t First, Count;
    };
    vector<PipelineEntry> Pipelines;
    vector<BindingEntry> Bindings;
    vector<ProgramEntry> Programs;
    vector<PrimitiveVertexLayoutId> GeometryLayouts;
    vector<GroupReference> OrderedGroups, ProgramGroups, NativeScratch, MergedScratch;
    Index PipelineIndex, GeometryIndex, LocalGroupIndex, BindingIndex, ProgramIndex;

    // A lease protects the sealed arrays across pool growth, concurrent live lists and epochs.
    // Scratch is shared only during sequential render-thread preparation and never enters Ready.
    shared_ptr<Storage> Acquire() {
        const size_t count = _storage.size();
        for (size_t i = 0; i < count; ++i) {
            const size_t index = (_next + i) % count;
            if (_storage[index].use_count() != 1) continue;
            _next = index + 1;
            _storage[index]->Reset();
            ResetScratch();
            return _storage[index];
        }
        auto storage = make_shared<Storage>();
        _storage.push_back(storage);
        _next = _storage.size();
        ResetScratch();
        return storage;
    }
    void BeginFrame() noexcept {
        _next = 0;
        ResetScratch();
    }
    size_t CapacityBytes() const noexcept {
        size_t bytes = sizeof(Workspace) + _storage.capacity() * sizeof(shared_ptr<Storage>) +
                       Pipelines.capacity() * sizeof(PipelineEntry) + Bindings.capacity() * sizeof(BindingEntry) +
                       Programs.capacity() * sizeof(ProgramEntry) + GeometryLayouts.capacity() * sizeof(PrimitiveVertexLayoutId) +
                       (OrderedGroups.capacity() + ProgramGroups.capacity() + NativeScratch.capacity() + MergedScratch.capacity()) * sizeof(GroupReference) +
                       PipelineIndex.CapacityBytes() + GeometryIndex.CapacityBytes() + LocalGroupIndex.CapacityBytes() + BindingIndex.CapacityBytes() + ProgramIndex.CapacityBytes();
        for (const auto& storage : _storage) bytes += sizeof(Storage) + storage->CapacityBytes();
        return bytes;
    }

private:
    void ResetScratch() noexcept {
        Pipelines.clear();
        Bindings.clear();
        Programs.clear();
        GeometryLayouts.clear();
        OrderedGroups.clear();
        ProgramGroups.clear();
        NativeScratch.clear();
        MergedScratch.clear();
        PipelineIndex.Reset();
        GeometryIndex.Reset();
        LocalGroupIndex.Reset();
        BindingIndex.Reset();
        ProgramIndex.Reset();
    }
    vector<shared_ptr<Storage>> _storage;
    size_t _next{0};
};

}  // namespace radray
