#pragma once
#include <atomic>
#include <functional>
#include <radray/imgui/imgui_graph.h>
#include <radray/runtime/shader_program.h>

namespace radray {
struct UiFrameTexture {
    ImGuiTextureDescriptor Descriptor;
    Nullable<render::Texture*> Texture{nullptr};
    Nullable<ImGuiTextureLease*> Lease{nullptr};
    RenderOutputId Output;
    bool Graph{false}, Dynamic{false};
};
struct UiDrawCommand {
    ImVec4 Clip;
    ImTextureID Texture{0};
    uint32_t Count{0}, IndexOffset{0};
    int32_t VertexOffset{0};
    int Sampler{0};  // 0 = texture/default, 1 = linear, 2 = nearest
};
struct UiViewportFrame {
    RenderOutputId Output;
    ImVec2 Position{}, Size{}, Scale{1, 1};
    vector<ImDrawVert> Vertices;
    vector<ImDrawIdx> Indices;
    vector<UiDrawCommand> Commands;
};
struct UiTextureRequest {
    ImTextureID Id{0};
    uint64_t Version{0};
    ImTextureStatus Status{ImTextureStatus_OK};
    uint32_t Width{0}, Height{0};
    render::TextureFormat Format{render::TextureFormat::RGBA8_UNORM};
    vector<ImTextureRect> Regions;
    vector<byte> Pixels;  // Always owned, tightly packed RGBA32.
};
struct UiGpuTexture {
    unique_ptr<render::Texture> Texture;
    array<render::TextureStates, 1> States{render::TextureState::Undefined};
    array<uint8_t, 1> Valid{0};
};
struct UiFlight {
    vector<UiViewportFrame> Viewports;
    unordered_map<ImTextureID, UiFrameTexture> Textures;
    vector<UiTextureRequest> Requests;
    vector<shared_ptr<UiGpuTexture>> Retained;
    vector<unique_ptr<RenderExternalTexture>> ExternalTextures;
    vector<vector<render::TextureStates>> AssetStates;
    vector<vector<uint8_t>> AssetValid;
    vector<RgOperationTicket> UploadTickets;
    uint64_t FrameSerial{0};
    bool GraphSuccess{false}, Valid{true};
    std::atomic_bool Completed{false};
};

struct UiGraphResources {
    render::Device& Device;
    std::function<Nullable<ShaderProgram*>(std::span<const byte>, const shader::GpuArtifactHash&)> CreateProgram;
    std::atomic_bool& Error;
    unordered_map<ImTextureID, shared_ptr<UiGpuTexture>> GpuTextures;
    Nullable<ShaderProgram*> DrawProgram{nullptr};
};
}  // namespace radray
