#pragma once

#include <radray/types.h>

namespace radray {

class RGraphNode {
public:
};

class RGraphEdge {
public:
};

class RGResourceNode : public RGraphNode {
public:
};

class RGBufferNode : public RGResourceNode {
public:
};

class RGTextureNode : public RGResourceNode {
public:
};

class RGAccelerationStructureNode : public RGResourceNode {
public:
};

class RGPassNode : public RGraphNode {
public:
};

class RGGraphicsPassNode : public RGPassNode {
public:
};

class RGComputePassNode : public RGPassNode {
public:
};

class RGCopyPassNode : public RGPassNode {
public:
};

class RGBufferEdge : public RGraphEdge {
public:
};

class RGBufferReadEdge : public RGBufferEdge {
public:
};

class RGBufferReadWriteEdge : public RGBufferEdge {
public:
};

class RGTextureEdge : public RGraphEdge {
public:
};

class RGTextureReadEdge : public RGTextureEdge {
public:
};

class RGTextureWriteEdge : public RGTextureEdge {
public:
};

class RGTextureReadWriteEdge : public RGTextureEdge {
public:
};

class RGAccelerationStructureEdge : public RGraphEdge {
public:
};

class RGAccelerationStructureReadEdge : public RGAccelerationStructureEdge {
public:
};

class RenderGraph {
public:
    void Link(RGraphNode* from, RGraphNode* to, RGraphEdge* edge);

private:
    vector<unique_ptr<RGraphNode>> _nodes;
    vector<unique_ptr<RGraphEdge>> _edges;
};

}  // namespace radray
