#include "example_descriptor_reuse.h"

#include <charconv>
#include <fmt/format.h>

namespace radray::example {
std::optional<DescriptorReuseOptions> ParseDescriptorReuseOptions(int argc, char** argv) {
    DescriptorReuseOptions options;
    options.ShaderRoot = RADRAY_DESCRIPTOR_SHADER_DIR;
    options.Compiler = RADRAY_DESCRIPTOR_COMPILER_PATH;
    options.Output = "descriptor-reuse.json";
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        const auto split = argument.find('=');
        if (split == std::string_view::npos) return std::nullopt;
        const auto key = argument.substr(0, split), value = argument.substr(split + 1);
        if (key == "--backend") {
            if (value != "d3d12" && value != "vulkan") return std::nullopt;
            options.Backend = value == "d3d12" ? render::RenderBackend::D3D12 : render::RenderBackend::Vulkan;
        } else if (key == "--scene") {
            const array<std::string_view, 9> scenes{"shared", "independent", "color-depth", "compatible-color", "equal-override", "changed-override", "visible-order", "peak-drop", "mixed"};
            if (std::find(scenes.begin(), scenes.end(), value) == scenes.end()) return std::nullopt;
            options.Scene = value;
        } else if (key == "--shader-root")
            options.ShaderRoot = string{value};
        else if (key == "--compiler")
            options.Compiler = string{value};
        else if (key == "--artifacts")
            options.Artifacts = string{value};
        else if (key == "--output")
            options.Output = string{value};
        else {
            uint32_t number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !number) return std::nullopt;
            if (key == "--objects")
                options.Objects = number;
            else if (key == "--materials")
                options.Materials = number;
            else if (key == "--views")
                options.Views = number;
            else if (key == "--flights")
                options.Flights = number;
            else if (key == "--frames")
                options.SteadyFrames = number;
            else
                return std::nullopt;
        }
    }
    if (options.Flights > 8 || options.Views > 32 || options.Materials > 4096 || options.ObjectCount() > 4096) return std::nullopt;
    return options;
}
}  // namespace radray::example

int main(int argc, char** argv) {
    const auto options = radray::example::ParseDescriptorReuseOptions(argc, argv);
    if (!options) {
        fmt::print(stderr, "Usage: example_descriptor_reuse --backend=d3d12|vulkan --scene=shared|independent|color-depth|compatible-color|equal-override|changed-override|visible-order|peak-drop|mixed --materials=32 --views=3 --flights=3 --output=result.json\n");
        return 2;
    }
    return radray::example::RunDescriptorReuse(*options) ? 0 : 1;
}
