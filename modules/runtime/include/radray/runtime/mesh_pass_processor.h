#pragma once

#include <optional>
#include <span>

#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/render/rhi.h>
#include <radray/runtime/material.h>
#include <radray/runtime/mesh_batch.h>
#include <radray/runtime/mesh_draw_command.h>

namespace radray {

// 调用方解析句柄并提供匹配记录；仅在 AddMeshBatch 期间借用这些引用。
// 处理器不得修改输入记录或其所属 Scene。
struct MeshBatchContext {
    const PrimitiveSceneInfo& Primitive;
    const MeshBatch& Batch;
    radray::PrimitiveHandle PrimitiveHandle{};
    MeshBatchHandle BatchHandle{};
    Nullable<const MaterialRenderProxy*> Material{nullptr};
    uint32_t VisibilitySortKey{0};
};

struct MeshDrawCommandSharedState {
    render::GraphicsPipelineState* Pipeline;
    uint8_t StencilReference{0};
    std::optional<uint32_t> CustomStateSortKey{};
    std::span<const MeshShaderParameterBinding> ParameterSets{};
    std::optional<MeshPushConstantBinding> PushConstants{};
    bool AllowDeferredInstanceStream{false};
    bool AllowElementMerging{false};
};

class MeshPassDrawListContext {
public:
    virtual ~MeshPassDrawListContext() noexcept = default;

    virtual MeshDrawCommandBindingStorage& GetBindingStorage() noexcept = 0;
    bool HasValidatedElements(const MeshBatch& batch) const noexcept { return _validatedBatch.Get() == &batch; }

    // 从已组装完成的 initializer 转移绑定引用所有权，并结合主元素默认实例范围与 stateSortKey 发布命令。
    // primaryElementIndex 必须对应 initializer.ValidElementMask 的最低有效位；不重写几何字段。
    // 后续添加操作不保证此前返回的命令引用仍有效。
    virtual MeshDrawCommand& AddCommand(const MeshBatchContext& context, size_t primaryElementIndex, MeshDrawCommand& initializer, uint32_t stateSortKey = 0) = 0;

protected:
    // validatedBatch 的元素须已通过校验，且在本上下文生命周期内保持有效、不变。
    explicit MeshPassDrawListContext(Nullable<const MeshBatch*> validatedBatch = nullptr) noexcept;

private:
    Nullable<const MeshBatch*> _validatedBatch;
};

class CachedPassMeshDrawListContext final : public MeshPassDrawListContext {
public:
    CachedPassMeshDrawListContext(CachedPassMeshDrawList& passDrawList, uint32_t batchIndex, Nullable<const MeshBatch*> validatedBatch = nullptr) noexcept;
    MeshDrawCommandBindingStorage& GetBindingStorage() noexcept override;
    // 每个缓存上下文至多发布一条命令。
    MeshDrawCommand& AddCommand(const MeshBatchContext& context, size_t primaryElementIndex, MeshDrawCommand& initializer, uint32_t stateSortKey = 0) override;

private:
    uint32_t _batchIndex;
    CachedPassMeshDrawList* _passDrawList;
    bool _committed{false};
};

// 命令与可见命令指针仅在本帧内有效；Clear 使此前全部输出失效。
class DynamicMeshPassDrawListContext final : public MeshPassDrawListContext {
public:
    DynamicMeshPassDrawListContext() noexcept = default;
    explicit DynamicMeshPassDrawListContext(vector<VisibleMeshDrawCommand>& visibleCommands, MeshPassSortMode sortMode = MeshPassSortMode::StateFirst) noexcept;
    MeshDrawCommandBindingStorage& GetBindingStorage() noexcept override;
    // 重新绑定时须将已输出项保留在原位置，例如在列表移动之后。
    void BindVisibleCommands(vector<VisibleMeshDrawCommand>& visibleCommands) noexcept;
    MeshDrawCommand& AddCommand(const MeshBatchContext& context, size_t primaryElementIndex, MeshDrawCommand& initializer, uint32_t stateSortKey = 0) override;
    // 在全部命令完成后、排序、复制或提交可见项之前调用。
    // 仅发布一次稳定指针；继续添加命令前必须 Clear，并使用新的可见列表。
    void FinalizeVisibleCommands() noexcept;
    void Clear() noexcept;
    void SetSortMode(MeshPassSortMode mode) noexcept;
    MeshPassSortMode GetSortMode() const noexcept { return _sortMode; }

    // FinalizeVisibleCommands 之后至 Clear 之前，不得调整大小、预留容量、删除元素或替换存储。
    vector<MeshDrawCommand> Commands;

private:
    // 所属帧绘制列表移动时，绑定存储的身份保持稳定。
    unique_ptr<MeshDrawCommandBindingStorage> _bindings;

    struct PendingVisibleCommand {
        size_t CommandIndex;
        size_t VisibleIndex;
    };

    Nullable<vector<VisibleMeshDrawCommand>*> _visibleCommands{nullptr};
    vector<PendingVisibleCommand> _pendingVisibleCommands;
    bool _finalized{false};
    MeshPassSortMode _sortMode{MeshPassSortMode::StateFirst};
};

class MeshPassProcessor {
public:
    MeshPassProcessor() noexcept = default;
    virtual ~MeshPassProcessor() noexcept = default;
    // 向输出上下文生成至多一条 Batch 命令；未生成命令就返回表示跳过该 Batch。
    virtual void AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext& output) = 0;

protected:
    // 元素不得超过 32 个；合法空 Batch 不输出命令，其余输入必须满足命令构建前置条件。
    // 仅 Debug 校验输入并在违约时终止程序；Release 信任调用方，不保证违约诊断。
    static void BuildMeshDrawCommands(const MeshBatchContext& context, const MeshDrawCommandSharedState& sharedState, MeshPassDrawListContext& output);
};

struct MeshPassRegistration {
    MeshPassId Pass{};
    MeshPassProcessor* Processor;
    MeshPassSortMode SortMode{MeshPassSortMode::StateFirst};
};

}  // namespace radray
