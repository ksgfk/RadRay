#pragma once

#include <cstddef>

#include <radray/render/common.h>
#include <radray/runtime/asset.h>
#include <radray/types.h>

namespace radray {

/// 一个 Pass 的逻辑 shader variant 身份。
///
/// Defines 必须在进入缓存前规范化：删除空字符串和重复项，并按稳定顺序排序。
/// 是否声明、是否互斥以及是否属于当前 Pass，由 ShaderAsset 负责校验；本类型只
/// 负责保存已经解析好的 key。容器的 Find/GetOrCreate 会再次调用 Normalize，避免
/// 调用方忘记排序时产生重复缓存项。
struct ShaderVariantKey {
    AssetId ShaderId{};
    vector<string> Defines{};
    uint32_t PassIndex{0};

    /// 只做与资产无关的规范化；keyword 合法性需要结合 ShaderAsset 判断。
    void Normalize();

    friend bool operator==(const ShaderVariantKey&, const ShaderVariantKey&) noexcept;
};

struct ShaderVariantKeyHasher {
    size_t operator()(const ShaderVariantKey& key) const noexcept;
};

/// 单 stage 编译模块的 key。
///
/// Defines 是根据 ShaderKeywordGroupDesc::Stages 投影到当前 Stage 后的集合。这样
/// 只影响 pixel 的 keyword 不会导致 vertex module 被重复编译。Stage 必须是单个
/// stage bit，而不是 Graphics 等组合值。
struct ShaderModuleKey {
    AssetId ShaderId{};
    vector<string> Defines{};
    uint32_t PassIndex{0};
    render::ShaderStage Stage{render::ShaderStage::UNKNOWN};

    void Normalize();

    friend bool operator==(const ShaderModuleKey&, const ShaderModuleKey&) noexcept;
};

struct ShaderModuleKeyHasher {
    size_t operator()(const ShaderModuleKey& key) const noexcept;
};

/// PipelineLayout 的规范化签名。
///
/// Entries 由最终有效的 binding / push-constant / static-sampler 描述编码而成，
/// 必须使用稳定格式并排序。不能把 Shader*、PipelineLayout* 或 ShaderVariantKey
/// 放进来，否则不同 variant 无法共享同一个 layout。当前 PipelineLayout 对外提供
/// FindParameter(name)，因此编码 Entries 时应保留参数名；以后若把名称到 binding
/// 的映射移到 variant 层，再可以去掉名称以扩大复用范围。
struct ShaderPipelineLayoutKey {
    vector<string> Entries{};

    /// 只规范化条目顺序，不删除重复项；重复 binding 应由签名构造阶段报错。
    void Normalize();

    friend bool operator==(const ShaderPipelineLayoutKey&, const ShaderPipelineLayoutKey&) noexcept;
};

struct ShaderPipelineLayoutKeyHasher {
    size_t operator()(const ShaderPipelineLayoutKey& key) const noexcept;
};

/// variant cache 对外提供的编译结果视图。
///
/// Shader 和 Layout 的所有权分别属于 ShaderVariantCache 内部的 module/layout
/// 容器；这里使用裸指针是为了让多个 variant 共享资源。指针只在 cache 清空前
/// 有效，清空前必须先销毁引用这些对象的 PSO。
struct CompiledShaderVariant {
    render::Shader* Vertex{nullptr};
    render::Shader* Pixel{nullptr};
    render::Shader* Compute{nullptr};
    render::PipelineLayout* Layout{nullptr};

    bool HasShader() const noexcept;
};

struct ShaderModuleEntry {
    /// DxcOutput 的字节码和反射在创建 Shader 后即可释放，不在这里重复保存。
    unique_ptr<render::Shader> Object;
};

struct ShaderPipelineLayoutEntry {
    unique_ptr<render::PipelineLayout> Object;
};

enum class ShaderVariantState {
    Building,
    Ready,
    Failed,
};

struct ShaderVariantEntry {
    ShaderVariantState State{ShaderVariantState::Building};
    CompiledShaderVariant Value{};
    /// 失败结果缓存的诊断信息；资产失效或重试时由管理器清除。
    string Error{};
};

/// 仅负责 variant/module/layout 的去重和所有权，不负责 DXC 编译。
///
/// 这是第一版的同步容器骨架。编译器接入后，管理器应先插入 Building entry，
/// 完成所有 stage 和 layout 创建后再切换到 Ready；失败则保留 Failed entry，
/// 避免每帧重复编译同一个坏 key。
///
/// 当前容器不支持单项淘汰：所有裸指针都在 Clear 前有效。若以后加入热重载，
/// 需要把 module/layout entry 改为带引用计数的句柄，或在安全 GPU fence 后回收。
class ShaderVariantCache {
public:
    ShaderVariantCache() noexcept = default;
    ShaderVariantCache(const ShaderVariantCache&) = delete;
    ShaderVariantCache(ShaderVariantCache&&) = delete;
    ShaderVariantCache& operator=(const ShaderVariantCache&) = delete;
    ShaderVariantCache& operator=(ShaderVariantCache&&) = delete;

    ~ShaderVariantCache() noexcept;

    const ShaderVariantEntry* Find(const ShaderVariantKey& sourceKey) const;
    ShaderVariantEntry* Find(const ShaderVariantKey& sourceKey);

    /// 插入或取得一个 variant entry。调用方负责填充状态和值，并保证引用的
    /// module/layout 属于本 cache。
    ShaderVariantEntry& GetOrCreateEntry(ShaderVariantKey sourceKey);

    const ShaderModuleEntry* Find(const ShaderModuleKey& sourceKey) const;
    ShaderModuleEntry* Find(const ShaderModuleKey& sourceKey);
    ShaderModuleEntry& GetOrCreateEntry(ShaderModuleKey sourceKey);

    const ShaderPipelineLayoutEntry* Find(const ShaderPipelineLayoutKey& sourceKey) const;
    ShaderPipelineLayoutEntry* Find(const ShaderPipelineLayoutKey& sourceKey);
    ShaderPipelineLayoutEntry& GetOrCreateEntry(ShaderPipelineLayoutKey sourceKey);

    void Clear() noexcept;

    size_t GetVariantCount() const noexcept;
    size_t GetModuleCount() const noexcept;
    size_t GetLayoutCount() const noexcept;

private:
    unordered_map<ShaderVariantKey, ShaderVariantEntry, ShaderVariantKeyHasher> _variants;
    unordered_map<ShaderModuleKey, ShaderModuleEntry, ShaderModuleKeyHasher> _modules;
    unordered_map<ShaderPipelineLayoutKey, ShaderPipelineLayoutEntry, ShaderPipelineLayoutKeyHasher> _layouts;
};

}  // namespace radray
