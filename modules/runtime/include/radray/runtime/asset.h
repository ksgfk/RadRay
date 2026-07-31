#pragma once

#include <filesystem>
#include <string_view>

#include <radray/runtime_type.h>
#include <radray/types.h>

namespace radray {

class AssetManager;

/// 资产的持久标识。落盘/去重缓存的 key,跨进程有效。
using AssetId = Guid;

/// == 资产的生命周期由引用计数【唯一】决定 ==
///
/// 没有强制卸载。AssetManager 不提供任何"无视引用计数销毁资产"的入口, 关卡切换与热重载
/// 靠放开引用完成。这条不变量买到的是一整类问题的消失:
///
/// 资产普遍向外交出指向自身内部的指针 —— ShaderAsset 交出 ShaderPassProgram*,
/// TextureAsset 交出 render::TextureView*, StaticMesh 交出 GpuMesh::DrawData*, 而这些
/// 指针会被长期缓存 (PSO 缓存条目、写进描述符的 view、SceneProxy 的 draw args)。
/// 只要"持有一份 StreamingAssetRef"就足以保证这些指针不悬垂, 依赖者便无需注册任何失效
/// 回调 —— 而漏注册回调是不可检测的错误。
///
/// 【曾经的另一条路及其代价】: 从前 Unload 可以单方面销毁槽位, 于是每种资产都被迫拆成
/// "槽位 + 引用计数的不可变内容"两层 (ShaderContent / TextureContent / StaticMeshContent),
/// 依赖者要同时持有 ref 与 content 两份引用才安全。那一层的全部存在理由就是防住强制卸载;
/// 强制卸载消失后它变成纯粹的双重间接与两倍样板, 故已合并回资产自身。
///
/// == 资产内部数据的延迟销毁 ==
///
/// 引用归零后资产【立即】被销毁 (对齐到 AssetManager::Pump 这一确定时刻), 不做闲置缓存。
/// 但资产内部的 GPU 对象可能仍被 GPU 读取, 故 OnUnload 是资产把这类数据交出去延迟销毁的
/// 时机, 见 AssetManager::DeferDestroy。
class Asset {
public:
    Asset() noexcept = default;
    Asset(const Asset&) = delete;
    Asset(Asset&&) = delete;
    Asset& operator=(const Asset&) = delete;
    Asset& operator=(Asset&&) = delete;
    virtual ~Asset() noexcept = default;

    /// 引用归零、本资产即将析构之前调用一次 (由 AssetManager::Pump 驱动)。
    ///
    /// 【职责是"交出需要延迟销毁的数据", 不是"释放资源"】: 纯 CPU 数据无需在此处理 ——
    /// 析构函数会做, 且那才是唯一正确的地方。这里只处理【不能立刻析构】的东西: GPU 对象
    /// 可能仍被上一帧录进命令列表, 必须经 manager.DeferDestroy 整包交出, 等一个帧边界后
    /// 再销毁。
    ///
    /// 【为何不放进析构函数】: 析构里拿不到 manager, 而"交给谁延迟"这件事必须由 manager
    /// 回答 —— 它持有 IWaitFrameProcessor。
    virtual void OnUnload(AssetManager& manager) = 0;

    /// 返回资产自身的运行时类型 id。
    virtual RuntimeTypeId GetTypeId() const noexcept = 0;

    const AssetId& GetAssetId() const noexcept { return _id; }

private:
    friend class AssetManager;

    AssetId _id;
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
