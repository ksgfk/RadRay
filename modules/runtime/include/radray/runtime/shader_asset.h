#pragma once

#include <filesystem>

#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/shader_program.h>

// shader 资产层: 一份 manifest = 一个 Asset。
//
// == 三层的分工 ==
//
//   shader_manifest.h  格式层。manifest desc、变体域、产物索引、ShaderResolver、cook。
//                      不含 Asset —— tools/shader_cook 只需要这一层, 不该为此吃下
//                      AssetManager 传递带来的 stdexec 编译开销。
//   shader_program.h   对象层。ShaderPassProgram: 共享 PipelineLayout 引用 + 字节码缓存。
//                      不含 Asset, 故 material 层可以单独用它。
//   shader_asset.h     资产层 (本文件)。ShaderAsset 把上面两层接进 AssetManager。
//
// 这条分界照 image_data.h (数据格式) 与 image_asset.h (Asset) 的既有先例。

namespace radray {

/// 一份 shader manifest 的资产。
///
/// 【粒度 = manifest, 不是 (manifest, pass)】: 产物布局已经定死了这条边界 ——
/// index.json 每份 manifest 一个, artifact 目录由 manifest 路径推导,
/// ShaderArtifactIndex::Entries 混装该 manifest 下所有 pass 的 stage (PassName 只是
/// 条目里的一个字段)。若按 pass 切, 两个资产会共享一份 index.json 与一份 resolver
/// 缓存, "谁持有 resolver" 立刻无解。PipelineLayout 的 per-pass 性质落在资产内部
/// 分层 (ShaderPassProgram), 不上升为资产边界。
///
/// 【构造即完整的兑现方式】: Asset 要求放进 AssetManager 时已可用, 而字节码本质按
/// variant 惰性。解法是加载期只做 variant / target 【无关】的部分 —— desc 解析、
/// PipelineLayout、vertex input storage。"完整"指 ABI 与 layout 就绪, 不指所有变体
/// 已编译。字节码归 ShaderPassProgram::GetOrCreateVariant 惰性。
///
/// 【这也让加载路径不碰 DXC】: 加载协程里只有同步文件 IO 与同步 GPU 调用, 两者都短,
/// 所以 AssetManager 的单线程泵不会被 JIT 阻塞。JIT 发生在后续 GetOrCreateVariant,
/// 那是调用方主动触发的。
/// 一份 shader manifest 的【内容】。生命周期独立于资产槽位, 见 asset.h。
///
/// 【为何 shader 是最需要分离的一类】: ShaderPassProgram* 会被 PipelineStateCache 的条目
/// 长期缓存 (那是派生数据 —— PSO 从 program 的字节码与 layout 建出), 而
/// AssetManager::Unload 可以随时销毁槽位。分离后 PSO 缓存只要持有一份内容引用,
/// program 就不会在它脚下消失。
class ShaderContent {
public:
    /// 【recycler 只收不存】: 归零时的释放由 AssetContentDeleter 完成, 它自己持有 recycler
    /// (见 asset.h)。这里保留形参是因为 MakeContent 统一把 GetRecycler() 作第二实参转发,
    /// 内容类型自身不再需要它。
    ShaderContent(
        AssetContentKey key,
        IRenderResourceRecycler& recycler,
        ShaderAssetDesc desc,
        unique_ptr<ShaderResolver> resolver,
        vector<unique_ptr<ShaderPassProgram>> passes) noexcept;
    ShaderContent(const ShaderContent&) = delete;
    ShaderContent(ShaderContent&&) = delete;
    ShaderContent& operator=(const ShaderContent&) = delete;
    ShaderContent& operator=(ShaderContent&&) = delete;
    ~ShaderContent() noexcept;

    bool IsValid() const noexcept { return !_passes.empty(); }

    const string& GetName() const noexcept { return _desc.Name; }
    const ShaderAssetDesc& GetDesc() const noexcept { return _desc; }

    /// 返回的指针在【本内容】存活期内稳定 (unique_ptr 后备存储)。持有一份 shared_ptr 即
    /// 保证它不悬垂 —— 这正是分离要买到的东西。
    Nullable<ShaderPassProgram*> FindPass(std::string_view name) noexcept;
    Nullable<const ShaderPassProgram*> FindPass(std::string_view name) const noexcept;

    size_t GetPassCount() const noexcept { return _passes.size(); }
    Nullable<ShaderPassProgram*> GetPass(size_t index) noexcept;

    /// 【只由 AssetContentDeleter 在引用归零时调用, 普通代码不得调用】: 它跑在析构【之前】,
    /// 故此刻成员仍然完整; 提前调用会留下一个成员已被搬空的内容对象。见 asset.h。
    void ReleaseRenderResources(IRenderResourceRecycler& recycler) noexcept;

private:
    ShaderAssetDesc _desc;
    /// 【必须声明在 _passes 之前】: program 借用 resolver 裸指针, 析构逆序保证
    /// program 先死。
    unique_ptr<ShaderResolver> _resolver;
    vector<unique_ptr<ShaderPassProgram>> _passes;
};

class ShaderAsset : public Asset {
public:
    ShaderAsset(
        shared_ptr<ShaderContent> content,
        ShaderResolveContext* context,
        PipelineLayoutCache* layoutCache) noexcept;
    ~ShaderAsset() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    /// 取内容的强引用。持有它期间内容保证存活, 即使本资产的槽位已被 Unload。
    ///
    /// 【刻意不提供 FindPass / GetDesc 等转发】: 那会让"哪个是真相"重新含糊, 而分离正是
    /// 为了消除这种含糊 (见 Asset 的说明)。热路径上请把内容引用提出来存住, 而不是
    /// 每次穿两层 —— 双重间接的代价由调用方一次性付掉。
    shared_ptr<ShaderContent> AcquireContent() const noexcept { return _content; }

    /// 内容是否仍挂在本槽位上。OnUnload 之后为 false。
    bool HasContent() const noexcept { return _content != nullptr; }

    /// 建本资产时用的共享设施。
    ///
    /// 【存在理由是让 dedup 命中可核对】: AssetManager 按 id 去重后直接返回既有 handle,
    /// 第二次调用携带的 options 连协程都没启动就被丢弃 (见 LoadShaderAsset)。若不把当初
    /// 用的是哪一份记下来, "所有调用点必须传同一个"这句话就无法兑现 —— 传错既不报错也
    /// 无从事后发现。
    ///
    /// 【为何留在 Asset 而不下沉到 ShaderContent】: 它们不是内容数据, 而是"这个 id 是用
    /// 哪些设施建起来的"这一条槽位级记录, 供 dedup 时比较。放在这里也免去"为了比两个指针
    /// 而先取一份内容引用"。
    ///
    /// 【只用于比较, 不解引用】: LayoutCache 允许先于资产销毁 (见 render_system.h 的
    /// 关停顺序说明), 故 OnUnload 后这两个指针可能已悬垂。它们不参与任何 GPU 调用。
    Nullable<ShaderResolveContext*> GetResolveContext() const noexcept { return _context; }
    Nullable<PipelineLayoutCache*> GetLayoutCache() const noexcept { return _layoutCache; }

private:
    shared_ptr<ShaderContent> _content;
    /// 【只记不用】: 供 dedup 命中时核对, 见 GetResolveContext。不解引用, 故允许悬垂。
    ShaderResolveContext* _context{nullptr};
    PipelineLayoutCache* _layoutCache{nullptr};
};

template <>
struct RuntimeTypeTrait<ShaderAsset> {
    static constexpr RuntimeTypeId value{0x6f2a91c4, 0x3e58, 0x4b07, 0x9d, 0x62, 0x14, 0xa7, 0x8c, 0x35, 0xe0, 0x1b};
    using Bases = std::tuple<Asset>;
};

/// 【本结构只放"进程级共享设施的指针", 不放 per-load 决策】: ShaderRoot / Staleness /
/// AllowJit / Dxc 曾在这里各占一项, 于是每个加载调用点都能自行决定"这是开发构建还是
/// 发布包"。那是一个进程级答案, 放在这里就成了第二套真相 —— 而且从未被兑现: AssetId
/// 只哈希 manifest 路径 (MakeShaderAssetId), AssetManager 按 id 去重后直接返回既有
/// handle, 第二次调用携带的 options 连协程都没启动就被静默丢弃。那四项现已统一落在
/// ShaderResolveContext。
///
/// 下面两个字段都是【指向唯一一份共享设施的指针】, 不是决策 —— 所有调用点必须传同一个,
/// 传错不会产生"两套策略", 只会产生一个错误的依赖注入。这与上面被删掉的四项性质不同。
///
/// 【"必须传同一个"由 LoadShaderAsset 兑现, 不只是注释】: 上一段指出 dedup 会静默丢弃
/// 第二次的 options —— 那个观察对本结构同样成立, 所以资产会记下它是用哪一份建的
/// (ShaderAsset::GetResolveContext), 而 LoadShaderAsset 在 dedup 命中时核对并 abort。
/// 若不核对, 这两个字段就和被删掉的四项一样是"写了没人管"的约定。
struct ShaderAssetLoadOptions {
    /// 全进程共享的解析上下文 (ShaderRoot / Staleness / AllowJit / Dxc / 源码缓存)。
    /// 【不持有生命周期】: 必须在资产存活期间保持有效, 通常由 RenderSystem 持有。
    /// 为空则加载失败 —— 不猜 include 根, 见 CreateShaderAsset。
    Nullable<ShaderResolveContext*> Context{nullptr};
    /// PipelineLayout 的内容去重缓存, 由 RenderSystem 持有。
    /// 【不持有生命周期】: 但允许它先于资产销毁 —— layout 按引用计数自保, 见
    /// pipeline_layout_cache.h。
    /// 【为空即加载失败】: 不提供"绕过缓存直接建 layout"的回退, 那会让 layout 的所有权
    /// 有两条路径。调用方给一个缓存即可, 不必是 RenderSystem 那一个。
    ///
    /// 【它同时决定了资产用哪个 device】: 本资产的 layout 全部来自这个缓存, 而
    /// PipelineLayoutCache 自己绑定一个 device。故加载 shader 资产不需要另传 device ——
    /// 传了只会多出一条"两者是否一致"的校验, 见 CreateShaderAsset。缓存必须已绑定
    /// device, 否则加载失败。
    Nullable<PipelineLayoutCache*> LayoutCache{nullptr};
};

/// manifest 路径 -> AssetId。归一化与命名空间隔离的规则见 MakeAssetIdFromPath。
///
/// 【身份是"这份文件"而非"逻辑资产名"】: manifest 在源码树与输出目录各有一份, 归一化后
/// 路径仍不同故 id 不同 —— 这是正确的 (确实是两份文件, 产物目录也各自独立)。按逻辑名
/// 寻址需要另一层名字->路径映射, 属材质层。
///
/// 注意这条与"同一个文件必须得到同一个 id"不矛盾: 前者说的是两份真实存在的副本, 后者
/// 说的是同一份文件的多种写法 ("a/../b/x" 与 "b/x")。归一化只消除后者。
AssetId MakeShaderAssetId(const std::filesystem::path& manifestPath);

/// 同步创建 ShaderAsset。读 manifest、建每个 pass 的 PipelineLayout 与 vertex input。
/// 不编译任何字节码。任一 pass 失败则整体失败 (返回 nullptr, 原因写入 outDiag)。
///
/// options.Context 为空即失败。旧版在 ShaderRoot 留空时按"父目录的父目录"猜 include 根,
/// 那个兜底存在恰恰说明当时拿不到唯一真相 —— context 里有确切答案, 不必猜。
///
/// 【刻意不收 render::Device】: 本函数不直接建任何 GPU 对象 —— layout 由
/// options.LayoutCache 建, resolver 与 vertex input storage 都是纯 CPU 数据。曾有一个
/// `render::Device&` 参数, 它唯一的用途是被拿去与 LayoutCache->GetDevice() 比对, 即
/// 【一个只为了被校验而存在的参数】。删掉它, "device 与 cache 错配"就从一条运行时校验
/// 变成类型上不可表达, 顺带少一条只有 dedup 路径才会漏掉的检查。
///
/// 【为何收 AssetManager&】: ShaderContent 只能经 AssetManager::MakeContent 创建 (它要
/// 一张 AssetContentKey), recycler 由那里注入。这与上面被删掉的 device 参数性质相反:
/// device 是"只为被校验而存在", manager 是真的要用来建内容。
Nullable<unique_ptr<ShaderAsset>> CreateShaderAsset(
    AssetManager& assetManager,
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options,
    ShaderAssetDiagnostic& outDiag) noexcept;

/// 经 AssetManager 加载 (按 id 去重)。
///
/// 【options 在发起加载【之前】被校验, 这是必须的】: AssetManager::Load 命中既有 slot
/// 时直接返回 handle, request.Task 那个协程帧一次都不 resume。故校验若只写在
/// CreateShaderAsset 里, 第二次调用带的空 options 会被静默接受, 调用方以为自己的
/// context 生效了, 实际用的是第一次那份。
///
/// options 不合法时返回【无效 ref】且不占用 id —— 不发起一个注定 Faulted 的加载:
/// Faulted slot 会把 id 占住, 之后拿对 options 重试反而会被 dedup 命中那个坏 slot。
///
/// dedup 命中且既有资产用的是【另一份】Context / LayoutCache 时 abort。那是依赖注入
/// 接错了线, 不是可恢复的运行时状况: 调用方拿到的资产其 layout 来自别人的缓存, 后续
/// 建 PSO 的行为无从预测。
StreamingAssetRef<ShaderAsset> LoadShaderAsset(
    AssetManager& assetManager,
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options);

StreamingAssetRef<ShaderAsset> LoadShaderAsset(
    AssetManager& assetManager,
    const AssetId& assetId,
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options);

}  // namespace radray
