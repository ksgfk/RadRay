#pragma once

#include <radray/runtime/graph.h>

namespace radray {

// render::Shader and layout owners must outlive this cache. Keys copy descriptor values and strings.
class GraphicsPipelineCache {
public:
    explicit GraphicsPipelineCache(render::Device& device);
    Nullable<render::GraphicsPipelineState*> Get(const render::GraphicsPipelineStateDescriptor&, const render::AttachmentSignature&);
    size_t Size() const { return _entries.size(); }

private:
    struct Entry {
        render::PipelineLayout* layout{};
        render::Shader* vs{};
        render::Shader* ps{};
        string vsEntry, psEntry;
        vector<render::VertexBufferLayout> buffers;
        struct Attribute {
            uint32_t binding, offset;
            string semantic;
            uint32_t semanticIndex;
            render::VertexFormat format;
            uint32_t location;
            friend bool operator==(const Attribute&, const Attribute&) = default;
        };
        vector<Attribute> attributes;
        render::PrimitiveState primitive;
        std::optional<render::DepthStencilState> depth;
        render::MultiSampleState samples;
        vector<render::ColorTargetState> colors;
        render::AttachmentSignature attachments;
        unique_ptr<render::GraphicsPipelineState> pipeline;
        bool Matches(const Entry&) const;
    };
    render::Device& _device;
    vector<Entry> _entries;
};

}  // namespace radray
