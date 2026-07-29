#pragma once

#include <optional>
#include <span>

#include <radray/intrusive_ptr.h>
#include <radray/render/rhi.h>

// PipelineLayout 的内容去重缓存。
//
// == 为什么必须缓存 ==
//
// PipelineLayoutDescriptor 只由 binding 布局与 push constant 决定, 没有任何 per-pass
// 输入 (D3D12 侧的 root signature flags 是硬编码常量)。而 binding 布局由 pipeline 契约
// 决定, 不由材质决定 —— forward_pipeline/bindings.hlsli 的 RADRAY_FORWARD_* 宏使所有
// forward 材质共享同一份 per-view / per-frame 组, shadow caster 与 depth prepass 这类
// pass 更是几十份材质逐字节相同。于是材质库规模化后"少量不同 layout × 大量 pass"是
// 必然分布, per-pass 独占创建必然大量重复。
//
// == 为什么不放 Device ==
//
// SamplerCache 能放 device 是因为 sampler 的 key 空间是有界枚举叉乘。layout 的 key 空间
// 随材质库开放增长, device 级无界缓存会单调涨到进程退出。更要紧的是 shader 热更新: 卸载
// 一个 shader 资产必须让它不再需要的 GPU 对象跟着走, 而 device 缓存的条目不知道谁在引用,
// ShaderAsset::OnUnload 也没有立场去动一个 device 拥有的共享对象。
//
// (Vulkan 后端的 DescriptorSetLayoutCacheVulkan 正是这个坑的现成样本: 它的
// VkDescriptorSetLayout 由 device 缓存拥有, 只在 device 析构时释放。今天没暴雷是因为
// 那层 key 空间小得多且不可见, 但热更新泄漏同样存在。留待单独处理。)
//
// == 为什么手写侵入式计数而非 shared_ptr + weak_ptr ==
//
// 关停顺序是 RenderSystem (本缓存的宿主) 先死, 之后 AssetManager 才 force-unload 全部
// 资产, 那时持有者才放开最后一个引用 (见 application.cpp 的 Shutdown)。这个顺序不能对调:
// PipelineStateCache 的条目持 StreamingAssetRef, 而 ref 内部存 AssetManager* 裸指针,
// AssetManager 先死同样是 UAF。所以存在一个真实的顺序环。
//
// 解法是让"零引用时的销毁"不经过缓存: 计数活在 SharedPipelineLayout 自己身上, 归零即
// 自毁, 缓存只是非拥有索引。缓存先死时把残留条目的反向指针清空, 之后的 Release 直接
// 销毁 —— 缓存的死亡不影响任何存活对象的正确性。weak_ptr 方案下缓存仍是"发现失效"的
// 必经之路, 撑不过这个顺序。
//
// == 不走 IRenderResourceRecycler ==
//
// 归零即销毁, 不延迟。recycler 的抽象意图是延迟释放, 但唯一的生产实现
// ImmediateRenderResourceRecycler 就是 obj.reset(), 测试里的两个是 no-op —— 延迟释放
// 目前是个未兑现的抽象, 所以直接销毁不构成回退。真要延迟时应在此处统一接入, 而不是让
// 每个持有者各自决定。

namespace radray {

class PipelineLayoutCache;

/// PipelineLayoutCache 的 key。PipelineLayoutDescriptor 含 std::span, 既无 operator==
/// 也无 std::hash, 不能直接做 key, 故拍平成值型。
///
/// 【为何在 key 而非 BuildPipelineLayoutStorage 里归一化】: 两个语义相同但 manifest 书写
/// 顺序不同的 pass 必须命中同一条目, 否则缓存退化成假 miss。而两个后端在创建时本就各自
/// 排序 (d3d12_impl.cpp 的 CreateRootSignatureInternal、vulkan_impl.cpp 的
/// DescriptorSetLayoutCacheVulkan), 所以 descriptor 的顺序对 RHI 无所谓 —— 只有做 key 时
/// 才必须归一化。BuildPipelineLayoutStorage 因此保持为 manifest 的忠实打包, 归一化只此一处。
struct PipelineLayoutCacheKey {
    /// 按 GroupIndex 升序。每组的 Entries 按 Binding 升序。
    struct Group {
        uint32_t GroupIndex{0};
        vector<render::ShaderParameterSetLayoutEntryDescriptor> Entries;

        friend bool operator==(const Group&, const Group&) noexcept = default;
    };

    vector<Group> Groups;
    std::optional<render::PushConstantDescriptor> PushConstant{};

    /// 从一份 descriptor 拍平并归一化。descriptor 内的 span 只需活到调用返回。
    static PipelineLayoutCacheKey Build(const render::PipelineLayoutDescriptor& desc);

    /// 返回的 descriptor 内 span 指向本对象, 有效期至【下一次 Get() 调用或本对象被修改】。
    ///
    /// 【每次调用都重建视图】: span 的后备存储是本对象的一个 mutable 成员, 拷贝 / 移动后
    /// 它会指向旧位置。让 Get() 无条件重建, 调用方就不必记住"拷完要刷一次" —— 那种约定
    /// 迟早会被漏掉, 而漏掉的表现是读到已释放内存。Get() 每个 layout 只在创建时调一次,
    /// 重建的成本无关紧要。
    ///
    /// 【它也是 ShaderPassProgram 不再需要 ShaderPipelineLayoutStorage 的原因】: 二者都是
    /// "PipelineLayoutDescriptor 的稳定后备存储", 而 key 由 SharedPipelineLayout 持有,
    /// 生命周期覆盖 layout 本身, 故 program 无需再存一份。
    render::PipelineLayoutDescriptor Get() const;

    /// 【不能用 = default】: _setsView 是 span 的容器, ShaderParameterSetLayoutDescriptor
    /// 没有 operator==。它也不该参与比较 —— 它是 Groups 的派生视图。
    friend bool operator==(const PipelineLayoutCacheKey& lhs, const PipelineLayoutCacheKey& rhs) noexcept {
        return lhs.Groups == rhs.Groups && lhs.PushConstant == rhs.PushConstant;
    }

private:
    /// Get() 用的 span 后备存储。派生自 Groups, 不参与相等比较。
    mutable vector<render::ShaderParameterSetLayoutDescriptor> _setsView;
};

/// 一个共享的 render::PipelineLayout。
///
/// 【为什么包一层而不给 RenderBase 加计数】: 引用语义只有 layout 需要, 给 RenderBase
/// 加会把它强加给 Buffer / Texture / PSO 等绝大多数不需要的类型。
///
/// 【为什么不继承 IntrusiveRefCounted】: 归零时要先从缓存索引摘除再销毁, 且必须能在
/// 缓存已死时跳过摘除, 基类的"归零即 delete"表达不了。
class SharedPipelineLayout {
public:
    SharedPipelineLayout(
        PipelineLayoutCache* cache,
        PipelineLayoutCacheKey key,
        unique_ptr<render::PipelineLayout> object) noexcept;
    SharedPipelineLayout(const SharedPipelineLayout&) = delete;
    SharedPipelineLayout& operator=(const SharedPipelineLayout&) = delete;
    ~SharedPipelineLayout() noexcept;

    /// 永不为空 —— 缓存只在创建成功后才构造本对象。
    render::PipelineLayout* Get() const noexcept { return _object.get(); }
    const PipelineLayoutCacheKey& GetKey() const noexcept { return _key; }

    /// 仅用于诊断与测试。
    uint32_t GetRefCount() const noexcept { return _refCount; }

private:
    friend class PipelineLayoutCache;
    friend void IntrusivePtrAddRef(const SharedPipelineLayout* obj) noexcept;
    friend void IntrusivePtrRelease(const SharedPipelineLayout* obj) noexcept;

    /// 缓存先死时被置空 —— 之后归零直接销毁, 不回调。
    PipelineLayoutCache* _cache{nullptr};
    PipelineLayoutCacheKey _key;
    unique_ptr<render::PipelineLayout> _object;
    mutable uint32_t _refCount{1};
};

void IntrusivePtrAddRef(const SharedPipelineLayout* obj) noexcept;
void IntrusivePtrRelease(const SharedPipelineLayout* obj) noexcept;

using SharedPipelineLayoutRef = IntrusivePtr<SharedPipelineLayout>;

/// 按 binding 布局内容去重 PipelineLayout。非拥有索引 —— 条目的生命周期由持有者的
/// 引用计数决定, 归零时对象自己从本索引摘除。
///
/// 非线程安全 (与 RenderPassRegistry / PipelineStateCache 一致)。
class PipelineLayoutCache {
public:
    explicit PipelineLayoutCache(render::Device* device) noexcept;
    ~PipelineLayoutCache() noexcept;
    PipelineLayoutCache(const PipelineLayoutCache&) = delete;
    PipelineLayoutCache& operator=(const PipelineLayoutCache&) = delete;

    /// 取或建。命中时 +1 返回既有对象, 未命中则创建。失败返回空。
    SharedPipelineLayoutRef GetOrCreate(const render::PipelineLayoutDescriptor& desc) noexcept;

    /// key 已经归一化好时的入口 (避免重复拍平)。
    SharedPipelineLayoutRef GetOrCreate(PipelineLayoutCacheKey key) noexcept;

    /// 本缓存创建 layout 所用的 device。析构后为空。
    Nullable<render::Device*> GetDevice() const noexcept { return _device; }

    uint32_t GetLayoutCount() const noexcept { return static_cast<uint32_t>(_entries.size()); }
    uint64_t GetHitCount() const noexcept { return _hits; }
    uint64_t GetMissCount() const noexcept { return _misses; }

private:
    friend void IntrusivePtrRelease(const SharedPipelineLayout* obj) noexcept;

    /// 归零回调。由 IntrusivePtrRelease 在销毁对象【之前】调用。
    void Detach(SharedPipelineLayout* layout) noexcept;

    render::Device* _device{nullptr};
    /// 非拥有。条目在对象归零时被摘除, 故正常关停时本容器应已空。
    vector<SharedPipelineLayout*> _entries;
    uint64_t _hits{0};
    uint64_t _misses{0};
};

}  // namespace radray
