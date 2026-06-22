#pragma once

#include <optional>
#include <span>
#include <utility>

#include <radray/types.h>
#include <radray/hash.h>
#include <radray/basic_math.h>
#include <radray/structured_buffer.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material.h>

namespace radray {

/// 一条按名的材质参数赋值(向量型)。用于把 per-使用点的材质值
/// 从资产加载层(如 glTF)携带到 SceneProxy,再写进 MaterialInstance。
/// 当前仅需 float4(glTF 的 5 个 Principled 参数均为 float4);
/// 后续需要标量/矩阵时再扩展。
struct MaterialParameterAssignment {
    string Name;
    Eigen::Vector4f Value{Eigen::Vector4f::Zero()};
};

/// 一条按名的材质贴图绑定。名字为 shader 反射出的贴图槽名(如 "gBaseColor")。
/// 从资产加载层(如 glTF)携到 SceneProxy,再绑入 MaterialInstance 的贴图槽。
/// Texture 为类型擦除弱句柄(通常指向 TextureAsset)。
struct MaterialTextureAssignment {
    string Name;
    StreamingAssetRefAny Texture;
};

/// 材质实例。纯 CPU 层,对应 UE5 FMaterialInstance 的最小化等价物。
///
/// 设计:
/// - 持非拥有的 Material*(PSO 模板 + 参数布局来源)。
/// - 克隆 Material 的默认值存储模板,得到一份可独立写值的 per-instance 参数副本。
/// - 贴图按名持类型擦除的弱句柄(StreamingAssetRefAny),与具体 TextureAsset 类型解耦。
/// - 不碰 GPU:上传/绑定由 MaterialRenderProxy(阶段 2/3)完成。
class MaterialInstance {
public:
    MaterialInstance() = default;

    /// 从 Material 构造:克隆其存储模板。material 为空或无效时实例无效。
    explicit MaterialInstance(Material* material) noexcept;

    bool IsValid() const noexcept { return _material != nullptr; }

    Material* GetMaterial() const noexcept { return _material; }

    bool HasConstantBuffer() const noexcept { return _storage.has_value(); }

    /// 打包好的常量缓冲字节流,直接用于上传到 per-material cbuffer。
    /// 无 cbuffer 时返回空。
    std::span<const byte> GetConstantData() const noexcept;

    /// 按名写标量(float / int / uint 等 POD)。字段不存在时静默忽略,返回 false。
    template <class T>
    bool SetScalar(std::string_view name, const T& value) noexcept {
        return SetValueImpl(name, value);
    }

    /// 按名写向量(Eigen::Vector2/3/4f 等)。
    template <class T>
    bool SetVector(std::string_view name, const T& value) noexcept {
        return SetValueImpl(name, value);
    }

    /// 按名写矩阵(Eigen::Matrix4f 等)。
    template <class T>
    bool SetMatrix(std::string_view name, const T& value) noexcept {
        return SetValueImpl(name, value);
    }

    /// 按名绑贴图(类型擦除弱句柄)。槽不存在时返回 false。
    bool SetTexture(std::string_view name, StreamingAssetRefAny texture) noexcept;

    /// 取按名绑定的贴图句柄;未绑定时返回空句柄。
    StreamingAssetRefAny GetTexture(std::string_view name) const noexcept;

    std::span<const std::pair<string, StreamingAssetRefAny>> GetBoundTextures() const noexcept { return _textures; }

private:
    // 从存储模板解析出「字段名 → 稳定 globalId」句柄缓存。构造时一次性构建,
    // 热路径写值复用句柄,避免每次按名线性扫描 GetMembers()。
    void BuildFieldHandleCache() noexcept;

    template <class T>
    bool SetValueImpl(std::string_view name, const T& value) noexcept {
        if (!_storage.has_value()) {
            RADRAY_ERR_LOG("MaterialInstance: set '{}' on instance without constant buffer", name);
            return false;
        }
        auto it = _fieldHandles.find(name);
        if (it == _fieldHandles.end()) {
            RADRAY_ERR_LOG("MaterialInstance: unknown parameter field '{}'", name);
            return false;
        }
        // 复用缓存句柄直接寻址叶子字段,走 fail-fast 写入(未知/尺寸不符即报错)。
        StructuredBufferView field{&_storage.value(), it->second};
        return field.TrySetValue(value);
    }

    Material* _material{nullptr};
    std::optional<StructuredBufferStorage> _storage{};
    string _rootName{};  // 根 cbuffer 名,用于 GetVar 寻址。
    unordered_map<string, StructuredBufferId, StringHash, StringEqual> _fieldHandles{};
    vector<std::pair<string, StreamingAssetRefAny>> _textures{};
};

}  // namespace radray
