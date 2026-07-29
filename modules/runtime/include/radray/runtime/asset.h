#pragma once

#include <concepts>
#include <filesystem>
#include <string_view>

#include <radray/sparse_set.h>
#include <radray/runtime_type.h>
#include <radray/intrusive_ptr.h>

namespace radray {

class IRenderResourceRecycler;
class AssetManager;

/// 资产的持久标识。落盘/去重缓存的 key,跨进程有效。
using AssetId = Guid;

/// 资产在单个 AssetManager 内的一次 slot 生命周期标识。slot 回收复用后 generation 变化。
/// 仅用于运行时关联与悬垂检测,不可序列化。
using AssetHandle = SparseSetHandle;

/// 运行时类型标识。用于类型擦除后的向下转换校验(StreamingAssetRefAny -> StreamingAssetRef<T>)。
/// 不依赖 RTTI,由资产类型手填固定 Guid,跨进程/跨模块稳定。
/// TypeId 始终表示最终实例的精确类型;Is/CastTo 通过 RuntimeTypeTrait<T>::Bases 支持基类视图。
using AssetTypeId = RuntimeTypeId;

/// 内容构造许可证。只有 AssetManager 能造出它。
///
/// 【为何要它】: AssetContent 必须在构造期拿到 recycler, 而"拿对 recycler"不能靠纪律。
/// 若内容的构造函数是公开可达的, 就存在第二条创建路径能塞进一个错的 (或空的) recycler,
/// 而删掉 AssetManager 里那个 static 兜底 recycler 正是为了消灭这类静默错配。有了本许可证,
/// "绕过 AssetManager 创建内容"直接编译不过。
class AssetContentKey {
private:
    friend class AssetManager;
    AssetContentKey() noexcept = default;
};

/// 资产的【不可变内容】。生命周期独立于资产槽位, 由引用计数管理。
///
/// == 为何内容要与槽位分离 ==
///
/// 资产普遍向外交出指向自身内部的指针 —— ShaderAsset 交出 ShaderPassProgram*,
/// TextureAsset 交出 render::TextureView*, StaticMesh 交出 GpuMesh::DrawData*, 而这些
/// 指针会被长期缓存 (PSO 缓存条目、写进描述符的 view、SceneProxy 的 draw args)。
/// 与此同时 AssetManager::Unload 可以单方面终止"资产存活期"。两者不能并存。
///
/// 把内容做成引用计数对象后, "谁在用它就还活着"由类型系统保证, 不再依赖每个依赖者记得
/// 注册失效回调 —— 漏注册是不可检测的。Unload 于此变得诚实: 它销毁【槽位】(标识失效,
/// StreamingAssetRef 报 Unloaded), 内容则活到最后一个使用者放手。
///
/// == 为何不可变 ==
///
/// 重载要求新旧共存: GPU 可能还在用上一帧录制的 PSO / 描述符。原地替换会让所有缓存了派生
/// 数据的地方在同一瞬间陈旧, 而我们无法枚举它们。改为"建新内容、旧内容按引用计数自然消亡"
/// 后, 缓存失效退化为天然 miss, 不需要任何 invalidate 通知。
///
/// == 为何延迟销毁在基类 ==
///
/// StaticMesh 曾用 shared_ptr<GpuMesh> 自行做了一半内容分离, 结果 OnUnload 里那句
/// use_count() == 1 判断使"别人还持有"这条分支【绕过了 recycler】—— 而那恰恰是唯一有 GPU
/// 危险的分支, GPU 对象会在最后一个持有者归零时立即析构, 不等 fence。归零动作因此必须由
/// 基类统一接管, 而不能留给每个资产各写一遍。
///
/// == 为何自持 recycler 指针 ==
///
/// 内容可以合法地比 AssetManager 活得久 —— 这正是分离要的效果。若归零时回头向 AssetManager
/// 索取 recycler, 内容就重新依赖 AssetManager 存活, 分离白做。同 SharedPipelineLayout
/// 自持 _cache 指针。app 保证 recycler (GpuSystem) 比所有内容活得久, 见 Application::Shutdown。
class AssetContent {
public:
    /// 只能经 AssetManager::MakeContent 创建, 见 AssetContentKey。
    AssetContent(AssetContentKey, IRenderResourceRecycler& recycler) noexcept
        : _recycler(&recycler) {}
    AssetContent(const AssetContent&) = delete;
    AssetContent(AssetContent&&) = delete;
    AssetContent& operator=(const AssetContent&) = delete;
    AssetContent& operator=(AssetContent&&) = delete;
    virtual ~AssetContent() noexcept = default;

    /// 仅用于诊断与测试。业务代码不该按计数分支。
    uint32_t GetRefCount() const noexcept { return _counter.Load(); }

protected:
    /// 引用计数归零时调用一次, 把 GPU 对象交给 recycler。
    /// 【在析构之前执行】, 故派生类成员此刻仍然完整可用。
    virtual void ReleaseRenderResources(IRenderResourceRecycler& recycler) noexcept = 0;

private:
    friend void IntrusivePtrAddRef(const AssetContent* obj) noexcept;
    friend void IntrusivePtrRelease(AssetContent* obj) noexcept;

    /// 【恒非空】: 构造期注入且无 setter, 故不存在"未装配"状态。
    IRenderResourceRecycler* _recycler{nullptr};
    /// 非原子: 资产操作全部在主线程。渲染线程只解引用主线程取好的 AssetContentRef,
    /// 不得自行 acquire/release。(该约束的运行期校验待统一加。)
    IntrusiveSingleThreadCounter _counter;
};

void IntrusivePtrAddRef(const AssetContent* obj) noexcept;
void IntrusivePtrRelease(AssetContent* obj) noexcept;

/// 内容的强引用。持有它即保证内容存活。
///
/// 【T 必须是完整类型】: 约束求值会实例化 derived_from, 对前向声明的 T 会得到 false 并被
/// 缓存, 之后即使 T 补全也仍报错。只想"保住某份内容别死"而不访问其成员的地方 (如
/// PipelineStateCache 的条目) 请用 AssetContentAnyRef, 它不需要具体类型完整。
template <class T>
requires std::derived_from<T, AssetContent>
using AssetContentRef = IntrusivePtr<T>;

/// 类型擦除的内容强引用。仅用于"保命", 不提供内容访问。
using AssetContentAnyRef = IntrusivePtr<AssetContent>;

/// 所有资产的多态基类。对应 UE5 的 UObject 资产(如 UStaticMesh)在本项目中的最小化等价物。
///
/// 设计要点:
/// - 基类【不区分 CPU/GPU】。具体资产(如 StaticMesh)自行持有 CPU 源数据与 GPU 资源,
///   自己决定上传/丢弃/释放时机。基类只提供生命周期钩子。
/// - 资产采用【构造函数一次性初始化】:Asset 在被放入 AssetManager 时已经是完整、可用状态
///   (CPU 数据 + GPU 资源都已就绪)。加载/上传发生在构造之前,由加载协程完成。
/// - Asset 本身不持有引用计数;AssetManager 为每个 slot 维护独立的引用控制块。
///   StreamingAssetRef/StreamingAssetRefAny 参与计数,引用归零的 slot 可由 CollectUnreferenced 回收。
///   应用层仍可显式 Unload 强制回收;此时尚存引用会通过 generation 检查安全失效。
///
/// 【Asset 与 AssetContent 的分工】(见 AssetContent):
/// - Asset = 标识 + 加载状态 + 重载支点。回答"这个 id 现在指向哪份内容"。
/// - AssetContent = 内容 + 派生数据 + GPU 资源。回答"实际的数据是什么"。
/// 需要分离的资产类型在 Asset 上只暴露 AcquireContent(),【不得】添加任何转发到内容的
/// 访问函数 —— 那会让"哪个是真相"重新变得含糊, 而分离正是为了消除这种含糊。
///
/// 【哪些资产需要分离】判据: 是否有外部实体缓存了指向本资产内部的指针或派生数据, 且其寿命
/// 超过一次调用。注意这【不是】"是否持有 GPU 资源" —— ImageAsset 即使将来缓存解码后的
/// mip 链也不需要分离, 因为没人跨帧存它的内部指针。判据的核心是"存在无法枚举的指针持有者",
/// 那才是引用计数唯一能解决的问题。
///
/// 【系统里因此有两套计数, 各管一件事, 不是冗余】:
/// - AssetRefControl (shared_ptr 控制块) 管"槽位还在吗" —— weak 语义, 必须能在 slot 死后
///   继续存活并报告 Unloaded, 故刻意不迁到侵入式计数 (见 intrusive_ptr.h 的说明)。
/// - AssetContent 的侵入式计数管"内容还在用吗" —— strong 语义。
class Asset {
public:
    Asset() noexcept = default;
    Asset(const Asset&) = delete;
    Asset(Asset&&) = delete;
    Asset& operator=(const Asset&) = delete;
    Asset& operator=(Asset&&) = delete;
    virtual ~Asset() noexcept = default;

    /// 资产被回收(Unload)或 AssetManager 析构前调用一次。派生类在此释放 GPU 资源、丢弃 CPU 数据等。
    virtual void OnUnload(IRenderResourceRecycler& recycler) = 0;

    /// 返回资产自身的运行时类型 id。
    virtual AssetTypeId GetTypeId() const noexcept = 0;

    const AssetId& GetAssetId() const noexcept { return _id; }
    AssetHandle GetAssetHandle() const noexcept { return _handle; }

private:
    friend class AssetManager;

    AssetId _id;
    AssetHandle _handle{AssetHandle::Invalid()};
};

template <>
struct RuntimeTypeTrait<Asset> {
    static constexpr RuntimeTypeId value{0x8b445298, 0x4242, 0x4524, 0xb3, 0x7f, 0x37, 0x24, 0xc3, 0x5b, 0x3c, 0x94};
    using Bases = std::tuple<>;
};

/// 文件路径派生的 AssetId。同一份文件必须得到同一个 id, 不同文件必须得到不同 id。
///
/// namespacePrefix 做资产类型的命名空间隔离 ("shader" / "image" / ...): 同一路径在不同
/// 资产类型下必须得到不同 id, 否则一份 *.png 既当 ImageAsset 又当 TextureAsset 时会
/// 撞进同一个 slot。
///
/// 【路径先归一化再哈希, 这是正确性要求而非优化】: 未归一化时 "a/../b/x" 与 "b/x" 得到
/// 两个 id 却指同一个文件, 于是同一份 manifest 被建成两个资产, 各自持有一套
/// PipelineLayout 与字节码缓存 —— 表现为"shader 编了两遍、layout 缓存命中率莫名减半",
/// 且没有任何报错。
///
/// 归一化口径:
///   1. weakly_canonical —— 消掉 "." / ".." 并解 symlink, 与 shader_manifest.cpp 计算
///      源码身份时的口径一致 (那里同样用 weakly_canonical), 故两侧对"同一个文件"的
///      判断不会分叉。失败时 (盘符不可用、权限不足) 退到 absolute + lexically_normal,
///      再失败退到纯词法归一化 —— 兜底必须是确定的, 不能让 id 依赖于当时的 IO 结果。
///   2. generic_string —— 分隔符统一为 '/'。
///   3. Windows 下转小写 —— NTFS 路径大小写不敏感, 而 weakly_canonical 【不】做这层
///      归一化, 于是 "C:/Foo/x" 与 "c:/foo/x" 仍是两个 id。POSIX 下刻意不转:
///      那里大小写是显著的, 转了会把两个真实不同的文件合并。
AssetId MakeAssetIdFromPath(std::string_view namespacePrefix, const std::filesystem::path& path);

}  // namespace radray
