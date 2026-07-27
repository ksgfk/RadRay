#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include <radray/nullable.h>
#include <radray/render/rhi.h>
#include <radray/render/hlsl.h>
#include <radray/types.h>

namespace radray::render {

class Dxc;

struct DxcOutput {
    vector<byte> Data;
    vector<byte> Refl;
    ShaderBlobCategory Category{ShaderBlobCategory::DXIL};
};

struct DxcCompileOptions {
    std::string_view EntryPoint{};
    ShaderStage Stage{ShaderStage::UNKNOWN};
    HlslShaderModel SM{HlslShaderModel::SM60};
    std::span<const std::string_view> Defines{};
    std::span<const std::string_view> Includes{};
    bool IsOptimize{false};
    bool IsSpirv{false};
    bool EnableUnbounded{false};
};

#if defined(RADRAY_ENABLE_DXC)

class Dxc : public enable_shared_from_this<Dxc> {
public:
    Dxc(const Dxc&) = delete;
    Dxc(Dxc&&) = delete;
    Dxc& operator=(const Dxc&) = delete;
    Dxc& operator=(Dxc&&) = delete;
    ~Dxc() noexcept;

    bool IsValid() const noexcept { return _impl != nullptr; }
    void Destroy() noexcept;

    std::optional<DxcOutput> CompileMemory(
        std::string_view code,
        std::string_view sourceName,
        const DxcCompileOptions& options) noexcept;

    std::optional<DxcOutput> CompileFile(
        const std::filesystem::path& path,
        const DxcCompileOptions& options) noexcept;

    /// 仅运行预处理器 (dxc -P), 返回展开 include 与条件编译后的 HLSL 文本。
    ///
    /// 【为何需要】: 有些工具要在编译【之前】读到源码的结构化信息 (例如
    /// runtime/shader_asset_template.h 的 keyword pragma)。直接扫原始文件等于自己写
    /// 一个 HLSL 词法器 —— 要正确处理块注释、续行符、#if 0、include 展开。交给 DXC
    /// 做则天然准确, 且 include 只解析一次。
    ///
    /// 输出保留 `#line N "file"` 指令, 调用方可据此判定每一行的归属文件。未知 pragma
    /// (如自定义的 radray_* ) 会被原样保留, 而注释掉或被 #if 0 排除的不会出现。
    ///
    /// 【会 respect options.Defines】: 被 #ifdef 包住的内容是否出现取决于宏, 与真实
    /// 编译一致。
    ///
    /// options 里 EntryPoint / Stage / SM 对预处理无实际作用, 但 DXC 的参数构建要求
    /// 它们存在, 故仍需给出合法值。
    std::optional<string> PreprocessMemory(
        std::string_view code,
        std::string_view sourceName,
        const DxcCompileOptions& options) noexcept;

    std::optional<string> PreprocessFile(
        const std::filesystem::path& path,
        const DxcCompileOptions& options) noexcept;

    std::optional<HlslShaderDesc> GetShaderDescFromOutput(std::span<const byte> refl) noexcept;

private:
    class Impl;

    explicit Dxc(unique_ptr<Impl> impl) noexcept;

    unique_ptr<Impl> _impl;

    friend Nullable<shared_ptr<Dxc>> CreateDxc() noexcept;
};

Nullable<shared_ptr<Dxc>> CreateDxc() noexcept;

#endif

}  // namespace radray::render
