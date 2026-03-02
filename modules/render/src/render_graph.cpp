#include <radray/render/render_graph.h>

#include <algorithm>
#include <iterator>

#include <fmt/format.h>

#include <radray/logger.h>

namespace radray::render {

namespace {

constexpr bool RGIsTextureResourceType(RGResourceType type) noexcept {
    return type == RGResourceType::Texture;
}

constexpr bool RGIsBufferResourceType(RGResourceType type) noexcept {
    return type == RGResourceType::Buffer || type == RGResourceType::IndirectArgs;
}

const RGTextureDescriptor* RGGetTextureDescriptor(const VirtualResource& resource) noexcept {
    return std::get_if<RGTextureDescriptor>(&resource.Descriptor);
}

const RGBufferDescriptor* RGGetBufferDescriptor(const VirtualResource& resource) noexcept {
    if (const auto* buffer = std::get_if<RGBufferDescriptor>(&resource.Descriptor)) {
        return buffer;
    }
    if (const auto* indirect = std::get_if<RGIndirectArgsDescriptor>(&resource.Descriptor)) {
        return &indirect->Buffer;
    }
    return nullptr;
}

uint64_t RGIntervalEnd(uint64_t begin, uint64_t count, uint64_t allValue) noexcept {
    if (count == allValue) {
        return std::numeric_limits<uint64_t>::max();
    }
    if (begin > std::numeric_limits<uint64_t>::max() - count) {
        return std::numeric_limits<uint64_t>::max();
    }
    return begin + count;
}

bool RGIntervalsOverlap(
    uint64_t beginA,
    uint64_t countA,
    uint64_t beginB,
    uint64_t countB,
    uint64_t allValue) noexcept {
    const uint64_t endA = RGIntervalEnd(beginA, countA, allValue);
    const uint64_t endB = RGIntervalEnd(beginB, countB, allValue);
    return beginA < endB && beginB < endA;
}

bool RGTextureRangesOverlap(const RGTextureRange& lhs, const RGTextureRange& rhs) noexcept {
    return RGIntervalsOverlap(
               lhs.BaseMipLevel,
               lhs.MipLevelCount,
               rhs.BaseMipLevel,
               rhs.MipLevelCount,
               RGTextureRange::All) &&
           RGIntervalsOverlap(
               lhs.BaseArrayLayer,
               lhs.ArrayLayerCount,
               rhs.BaseArrayLayer,
               rhs.ArrayLayerCount,
               RGTextureRange::All);
}

bool RGBufferRangesOverlap(const RGBufferRange& lhs, const RGBufferRange& rhs) noexcept {
    return RGIntervalsOverlap(lhs.Offset, lhs.Size, rhs.Offset, rhs.Size, RGBufferRange::All);
}

bool RGSubresourceRangesOverlap(
    const VirtualResource& resource,
    const RGSubresourceRange& lhs,
    const RGSubresourceRange& rhs) {
    if (RGIsTextureResourceType(resource.GetType())) {
        if (!std::holds_alternative<RGTextureRange>(lhs) || !std::holds_alternative<RGTextureRange>(rhs)) {
            return false;
        }
        return RGTextureRangesOverlap(
            std::get<RGTextureRange>(lhs),
            std::get<RGTextureRange>(rhs));
    }

    if (!std::holds_alternative<RGBufferRange>(lhs) || !std::holds_alternative<RGBufferRange>(rhs)) {
        return false;
    }
    return RGBufferRangesOverlap(
        std::get<RGBufferRange>(lhs),
        std::get<RGBufferRange>(rhs));
}

template <typename T>
void RGSortAndUnique(vector<T>* values) {
    if (values == nullptr || values->empty()) {
        return;
    }
    std::sort(values->begin(), values->end());
    values->erase(std::unique(values->begin(), values->end()), values->end());
}

uint64_t RGRangeEnd(uint64_t begin, uint64_t count) noexcept {
    if (count == RGBufferRange::All) {
        return std::numeric_limits<uint64_t>::max();
    }
    if (begin > std::numeric_limits<uint64_t>::max() - count) {
        return std::numeric_limits<uint64_t>::max();
    }
    return begin + count;
}

vector<RGSubresourceRange> RGBuildAtomicTextureRanges(const vector<RGTextureRange>& usedRanges) {
    vector<RGSubresourceRange> atomicRanges{};
    if (usedRanges.empty()) {
        return atomicRanges;
    }

    vector<uint32_t> mipCuts{};
    vector<uint32_t> layerCuts{};
    mipCuts.reserve(usedRanges.size() * 2u);
    layerCuts.reserve(usedRanges.size() * 2u);

    for (const auto& range : usedRanges) {
        mipCuts.push_back(range.BaseMipLevel);
        mipCuts.push_back(range.BaseMipLevel + range.MipLevelCount);
        layerCuts.push_back(range.BaseArrayLayer);
        layerCuts.push_back(range.BaseArrayLayer + range.ArrayLayerCount);
    }

    RGSortAndUnique(&mipCuts);
    RGSortAndUnique(&layerCuts);
    if (mipCuts.size() < 2 || layerCuts.size() < 2) {
        return atomicRanges;
    }

    for (size_t mipIndex = 0; mipIndex + 1 < mipCuts.size(); ++mipIndex) {
        const uint32_t mipBegin = mipCuts[mipIndex];
        const uint32_t mipEnd = mipCuts[mipIndex + 1];
        if (mipEnd <= mipBegin) {
            continue;
        }
        for (size_t layerIndex = 0; layerIndex + 1 < layerCuts.size(); ++layerIndex) {
            const uint32_t layerBegin = layerCuts[layerIndex];
            const uint32_t layerEnd = layerCuts[layerIndex + 1];
            if (layerEnd <= layerBegin) {
                continue;
            }

            RGTextureRange slice{
                .BaseMipLevel = mipBegin,
                .MipLevelCount = mipEnd - mipBegin,
                .BaseArrayLayer = layerBegin,
                .ArrayLayerCount = layerEnd - layerBegin};

            const bool used = std::any_of(
                usedRanges.begin(),
                usedRanges.end(),
                [&slice](const RGTextureRange& range) {
                    return RGTextureRangesOverlap(slice, range);
                });
            if (used) {
                atomicRanges.push_back(slice);
            }
        }
    }

    return atomicRanges;
}

vector<RGSubresourceRange> RGBuildAtomicBufferRanges(const vector<RGBufferRange>& usedRanges) {
    vector<RGSubresourceRange> atomicRanges{};
    if (usedRanges.empty()) {
        return atomicRanges;
    }

    vector<uint64_t> cuts{};
    cuts.reserve(usedRanges.size() * 2u);
    bool hasUnboundedTail = false;
    for (const auto& range : usedRanges) {
        const uint64_t end = RGRangeEnd(range.Offset, range.Size);
        cuts.push_back(range.Offset);
        if (range.Size == RGBufferRange::All || end == std::numeric_limits<uint64_t>::max()) {
            hasUnboundedTail = true;
        } else {
            cuts.push_back(end);
        }
    }

    RGSortAndUnique(&cuts);
    if (cuts.empty()) {
        return atomicRanges;
    }

    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        const uint64_t begin = cuts[i];
        const uint64_t end = cuts[i + 1];
        if (end <= begin) {
            continue;
        }

        RGBufferRange slice{
            .Offset = begin,
            .Size = end - begin};

        const bool used = std::any_of(
            usedRanges.begin(),
            usedRanges.end(),
            [&slice](const RGBufferRange& range) {
                return RGBufferRangesOverlap(slice, range);
            });
        if (used) {
            atomicRanges.push_back(slice);
        }
    }

    if (hasUnboundedTail) {
        RGBufferRange tail{
            .Offset = cuts.back(),
            .Size = RGBufferRange::All};
        const bool used = std::any_of(
            usedRanges.begin(),
            usedRanges.end(),
            [&tail](const RGBufferRange& range) {
                return RGBufferRangesOverlap(tail, range);
            });
        if (used) {
            atomicRanges.push_back(tail);
        }
    }

    return atomicRanges;
}

vector<RGSubresourceRange> RGCollectAtomicRanges(
    const VirtualResource& resource,
    const vector<RGSubresourceRange>& usedRanges) {
    if (usedRanges.empty()) {
        return {};
    }

    if (RGIsTextureResourceType(resource.GetType())) {
        vector<RGTextureRange> textureRanges{};
        textureRanges.reserve(usedRanges.size());
        for (const auto& range : usedRanges) {
            if (std::holds_alternative<RGTextureRange>(range)) {
                textureRanges.push_back(std::get<RGTextureRange>(range));
            }
        }
        return RGBuildAtomicTextureRanges(textureRanges);
    }

    vector<RGBufferRange> bufferRanges{};
    bufferRanges.reserve(usedRanges.size());
    for (const auto& range : usedRanges) {
        if (std::holds_alternative<RGBufferRange>(range)) {
            bufferRanges.push_back(std::get<RGBufferRange>(range));
        }
    }
    return RGBuildAtomicBufferRanges(bufferRanges);
}

struct RGRegionKey {
    uint32_t ResourceIndex{0};
    RGSubresourceRange Range{RGTextureRange{}};

    friend bool operator==(const RGRegionKey& lhs, const RGRegionKey& rhs) noexcept = default;
};

size_t RGHashCombine(size_t seed, size_t value) noexcept {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
}

struct RGRegionKeyHash {
    size_t operator()(const RGRegionKey& key) const noexcept {
        size_t seed = 0;
        seed = RGHashCombine(seed, std::hash<uint32_t>{}(key.ResourceIndex));
        seed = RGHashCombine(seed, std::hash<size_t>{}(key.Range.index()));
        if (std::holds_alternative<RGTextureRange>(key.Range)) {
            const auto& texture = std::get<RGTextureRange>(key.Range);
            seed = RGHashCombine(seed, std::hash<uint32_t>{}(texture.BaseMipLevel));
            seed = RGHashCombine(seed, std::hash<uint32_t>{}(texture.MipLevelCount));
            seed = RGHashCombine(seed, std::hash<uint32_t>{}(texture.BaseArrayLayer));
            seed = RGHashCombine(seed, std::hash<uint32_t>{}(texture.ArrayLayerCount));
        } else {
            const auto& buffer = std::get<RGBufferRange>(key.Range);
            seed = RGHashCombine(seed, std::hash<uint64_t>{}(buffer.Offset));
            seed = RGHashCombine(seed, std::hash<uint64_t>{}(buffer.Size));
        }
        return seed;
    }
};

bool RGCanonicalizeTextureRange(
    const RGTextureDescriptor& desc,
    const RGTextureRange& input,
    RGTextureRange* outRange) {
    const uint32_t mipLevels = desc.MipLevels == 0 ? 1 : desc.MipLevels;
    const uint32_t arrayLayers = desc.DepthOrArraySize == 0 ? 1 : desc.DepthOrArraySize;

    if (input.BaseMipLevel >= mipLevels) {
        RADRAY_ERR_LOG(
            "RenderGraph: texture range base mip {} out of bounds (mips={})",
            input.BaseMipLevel,
            mipLevels);
        return false;
    }
    if (input.BaseArrayLayer >= arrayLayers) {
        RADRAY_ERR_LOG(
            "RenderGraph: texture range base layer {} out of bounds (layers={})",
            input.BaseArrayLayer,
            arrayLayers);
        return false;
    }

    const uint32_t maxMipCount = mipLevels - input.BaseMipLevel;
    const uint32_t maxLayerCount = arrayLayers - input.BaseArrayLayer;

    const uint32_t mipCount = input.MipLevelCount == RGTextureRange::All
                                  ? maxMipCount
                                  : input.MipLevelCount;
    const uint32_t layerCount = input.ArrayLayerCount == RGTextureRange::All
                                    ? maxLayerCount
                                    : input.ArrayLayerCount;

    if (mipCount == 0 || mipCount > maxMipCount) {
        RADRAY_ERR_LOG(
            "RenderGraph: texture mip count {} out of bounds (max={})",
            mipCount,
            maxMipCount);
        return false;
    }
    if (layerCount == 0 || layerCount > maxLayerCount) {
        RADRAY_ERR_LOG(
            "RenderGraph: texture layer count {} out of bounds (max={})",
            layerCount,
            maxLayerCount);
        return false;
    }

    *outRange = RGTextureRange{
        .BaseMipLevel = input.BaseMipLevel,
        .MipLevelCount = mipCount,
        .BaseArrayLayer = input.BaseArrayLayer,
        .ArrayLayerCount = layerCount};
    return true;
}

bool RGCanonicalizeBufferRange(
    const RGBufferDescriptor& desc,
    const RGBufferRange& input,
    RGBufferRange* outRange) {
    if (input.Size == 0) {
        RADRAY_ERR_LOG("RenderGraph: buffer range size must not be 0");
        return false;
    }

    if (desc.Size == RGBufferRange::All) {
        *outRange = RGBufferRange{
            .Offset = input.Offset,
            .Size = input.Size == RGBufferRange::All ? RGBufferRange::All : input.Size};
        return true;
    }

    if (input.Offset >= desc.Size) {
        RADRAY_ERR_LOG(
            "RenderGraph: buffer range offset {} out of bounds (size={})",
            input.Offset,
            desc.Size);
        return false;
    }

    const uint64_t maxSize = desc.Size - input.Offset;
    const uint64_t size = input.Size == RGBufferRange::All ? maxSize : input.Size;
    if (size == 0 || size > maxSize) {
        RADRAY_ERR_LOG(
            "RenderGraph: buffer range size {} out of bounds (max={})",
            size,
            maxSize);
        return false;
    }

    *outRange = RGBufferRange{
        .Offset = input.Offset,
        .Size = size};
    return true;
}

bool RGAccessEdgeEquals(const ResourceAccessEdge& lhs, const ResourceAccessEdge& rhs) noexcept {
    return lhs.Handle == rhs.Handle && lhs.Mode == rhs.Mode && lhs.Range == rhs.Range;
}

bool RGContainsEdge(
    const vector<ResourceAccessEdge>& edges,
    const ResourceAccessEdge& candidate) noexcept {
    return std::find_if(
               edges.begin(),
               edges.end(),
               [&candidate](const ResourceAccessEdge& edge) {
                   return RGAccessEdgeEquals(edge, candidate);
               }) != edges.end();
}

std::optional<RGRegionKey> RGMakeRegionKey(
    const vector<VirtualResource>& resources,
    const ResourceAccessEdge& edge) {
    if (!edge.Handle.IsValid() || edge.Handle.Index >= resources.size()) {
        return std::nullopt;
    }

    const auto& resource = resources[edge.Handle.Index];
    if (RGIsTextureResourceType(resource.GetType()) && !std::holds_alternative<RGTextureRange>(edge.Range)) {
        return std::nullopt;
    }
    if (RGIsBufferResourceType(resource.GetType()) && !std::holds_alternative<RGBufferRange>(edge.Range)) {
        return std::nullopt;
    }

    return RGRegionKey{
        .ResourceIndex = edge.Handle.Index,
        .Range = edge.Range};
}

bool RGEdgesOverlap(
    const VirtualResource& resource,
    const ResourceAccessEdge& lhs,
    const ResourceAccessEdge& rhs) {
    if (lhs.Handle.Index != rhs.Handle.Index) {
        return false;
    }
    return RGSubresourceRangesOverlap(resource, lhs.Range, rhs.Range);
}

bool RGPassesConflict(
    const vector<VirtualResource>& resources,
    const RGPassNode& lhs,
    const RGPassNode& rhs) {
    // RAW + WAW
    for (const auto& lhsWrite : lhs.Writes) {
        for (const auto& rhsRead : rhs.Reads) {
            if (lhsWrite.Handle.Index == rhsRead.Handle.Index && RGEdgesOverlap(resources[lhsWrite.Handle.Index], lhsWrite, rhsRead)) {
                return true;
            }
        }
        for (const auto& rhsWrite : rhs.Writes) {
            if (lhsWrite.Handle.Index == rhsWrite.Handle.Index && RGEdgesOverlap(resources[lhsWrite.Handle.Index], lhsWrite, rhsWrite)) {
                return true;
            }
        }
    }

    // WAR
    for (const auto& lhsRead : lhs.Reads) {
        for (const auto& rhsWrite : rhs.Writes) {
            if (lhsRead.Handle.Index == rhsWrite.Handle.Index && RGEdgesOverlap(resources[lhsRead.Handle.Index], lhsRead, rhsWrite)) {
                return true;
            }
        }
    }

    return false;
}

ResourceAccessEdge RGMakeFullRangeEdge(
    uint32_t resourceIndex,
    const VirtualResource& resource) {
    ResourceAccessEdge edge{};
    edge.Handle = RGResourceHandle{resourceIndex};
    edge.Mode = RGAccessMode::Unknown;
    if (RGIsTextureResourceType(resource.GetType())) {
        edge.Range = RGTextureRange{};
    } else {
        edge.Range = RGBufferRange{};
    }
    return edge;
}

vector<ResourceAccessEdge> RGCollectImplicitRoots(const RGGraphBuilder& graph) {
    vector<ResourceAccessEdge> roots{};
    const auto& resources = graph.GetResources();
    for (uint32_t i = 0; i < static_cast<uint32_t>(resources.size()); ++i) {
        const auto& resource = resources[i];
        if (resource.Flags.HasFlag(RGResourceFlag::Persistent) || resource.Flags.HasFlag(RGResourceFlag::Output) || resource.Flags.HasFlag(RGResourceFlag::ForceRetain) || resource.Flags.HasFlag(RGResourceFlag::Temporal)) {
            roots.push_back(RGMakeFullRangeEdge(i, resource));
        }
    }
    return roots;
}

string RGFormatTextureRange(const RGTextureRange& range) {
    const string mipCount = range.MipLevelCount == RGTextureRange::All
                                ? "*"
                                : fmt::format("{}", range.MipLevelCount);
    const string layerCount = range.ArrayLayerCount == RGTextureRange::All
                                  ? "*"
                                  : fmt::format("{}", range.ArrayLayerCount);
    return fmt::format(
        "mip[{}+{}] layer[{}+{}]",
        range.BaseMipLevel,
        mipCount,
        range.BaseArrayLayer,
        layerCount);
}

string RGFormatBufferRange(const RGBufferRange& range) {
    const string size = range.Size == RGBufferRange::All
                            ? "*"
                            : fmt::format("{}", range.Size);
    return fmt::format("offset[{}] size[{}]", range.Offset, size);
}

string RGFormatEdgeRange(const ResourceAccessEdge& edge) {
    if (std::holds_alternative<RGTextureRange>(edge.Range)) {
        return RGFormatTextureRange(std::get<RGTextureRange>(edge.Range));
    }
    return RGFormatBufferRange(std::get<RGBufferRange>(edge.Range));
}

string RGFormatSubresourceRange(const RGSubresourceRange& range) {
    if (std::holds_alternative<RGTextureRange>(range)) {
        return RGFormatTextureRange(std::get<RGTextureRange>(range));
    }
    return RGFormatBufferRange(std::get<RGBufferRange>(range));
}

bool RGNeedBarrier(RGAccessMode stateBefore, RGAccessMode stateAfter) {
    if (stateBefore != stateAfter) {
        return true;
    }
    return RGIsWriteAccess(stateAfter);
}

RGAccessMode RGMergeRequestedMode(RGAccessMode current, RGAccessMode incoming) {
    if (current == RGAccessMode::Unknown) {
        return incoming;
    }
    if (incoming == RGAccessMode::Unknown || current == incoming) {
        return current;
    }

    const bool currentWrite = RGIsWriteAccess(current);
    const bool incomingWrite = RGIsWriteAccess(incoming);
    if (currentWrite != incomingWrite) {
        return incomingWrite ? incoming : current;
    }
    return incoming;
}

bool RGTryMergeBufferRanges(
    const RGBufferRange& lhs,
    const RGBufferRange& rhs,
    RGBufferRange* outRange) {
    if (outRange == nullptr) {
        return false;
    }

    RGBufferRange a = lhs;
    RGBufferRange b = rhs;
    if (b.Offset < a.Offset) {
        std::swap(a, b);
    }

    if (a.Size == RGBufferRange::All) {
        if (b.Offset < a.Offset) {
            return false;
        }
        *outRange = a;
        return true;
    }

    const uint64_t endA = RGRangeEnd(a.Offset, a.Size);
    if (b.Size == RGBufferRange::All) {
        if (b.Offset > endA) {
            return false;
        }
        *outRange = RGBufferRange{
            .Offset = a.Offset,
            .Size = RGBufferRange::All};
        return true;
    }

    if (b.Offset > endA) {
        return false;
    }

    const uint64_t endB = RGRangeEnd(b.Offset, b.Size);
    const uint64_t mergedEnd = std::max(endA, endB);
    if (mergedEnd == std::numeric_limits<uint64_t>::max()) {
        *outRange = RGBufferRange{
            .Offset = a.Offset,
            .Size = RGBufferRange::All};
        return true;
    }

    *outRange = RGBufferRange{
        .Offset = a.Offset,
        .Size = mergedEnd - a.Offset};
    return true;
}

bool RGTryMergeTextureRanges(
    const RGTextureRange& lhs,
    const RGTextureRange& rhs,
    RGTextureRange* outRange) {
    if (outRange == nullptr) {
        return false;
    }

    if (lhs.BaseArrayLayer == rhs.BaseArrayLayer && lhs.ArrayLayerCount == rhs.ArrayLayerCount) {
        const uint64_t lhsEndMip = static_cast<uint64_t>(lhs.BaseMipLevel) + lhs.MipLevelCount;
        const uint64_t rhsEndMip = static_cast<uint64_t>(rhs.BaseMipLevel) + rhs.MipLevelCount;
        if (rhs.BaseMipLevel > lhsEndMip || lhs.BaseMipLevel > rhsEndMip) {
            return false;
        }

        const uint64_t mergedBaseMip = std::min<uint64_t>(lhs.BaseMipLevel, rhs.BaseMipLevel);
        const uint64_t mergedEndMip = std::max<uint64_t>(lhsEndMip, rhsEndMip);
        if (mergedEndMip > std::numeric_limits<uint32_t>::max()) {
            return false;
        }

        *outRange = RGTextureRange{
            .BaseMipLevel = static_cast<uint32_t>(mergedBaseMip),
            .MipLevelCount = static_cast<uint32_t>(mergedEndMip - mergedBaseMip),
            .BaseArrayLayer = lhs.BaseArrayLayer,
            .ArrayLayerCount = lhs.ArrayLayerCount};
        return true;
    }

    if (lhs.BaseMipLevel == rhs.BaseMipLevel && lhs.MipLevelCount == rhs.MipLevelCount) {
        const uint64_t lhsEndLayer = static_cast<uint64_t>(lhs.BaseArrayLayer) + lhs.ArrayLayerCount;
        const uint64_t rhsEndLayer = static_cast<uint64_t>(rhs.BaseArrayLayer) + rhs.ArrayLayerCount;
        if (rhs.BaseArrayLayer > lhsEndLayer || lhs.BaseArrayLayer > rhsEndLayer) {
            return false;
        }

        const uint64_t mergedBaseLayer = std::min<uint64_t>(lhs.BaseArrayLayer, rhs.BaseArrayLayer);
        const uint64_t mergedEndLayer = std::max<uint64_t>(lhsEndLayer, rhsEndLayer);
        if (mergedEndLayer > std::numeric_limits<uint32_t>::max()) {
            return false;
        }

        *outRange = RGTextureRange{
            .BaseMipLevel = lhs.BaseMipLevel,
            .MipLevelCount = lhs.MipLevelCount,
            .BaseArrayLayer = static_cast<uint32_t>(mergedBaseLayer),
            .ArrayLayerCount = static_cast<uint32_t>(mergedEndLayer - mergedBaseLayer)};
        return true;
    }

    return false;
}

bool RGBarrierMergeKeyEquals(const RGBarrier& lhs, const RGBarrier& rhs) {
    return lhs.Handle == rhs.Handle && lhs.StateBefore == rhs.StateBefore && lhs.StateAfter == rhs.StateAfter && lhs.Range.index() == rhs.Range.index();
}

vector<RGBarrier> RGMergePassBarriers(vector<RGBarrier> barriers) {
    if (barriers.empty()) {
        return barriers;
    }

    std::sort(
        barriers.begin(),
        barriers.end(),
        [](const RGBarrier& lhs, const RGBarrier& rhs) {
            if (lhs.Handle.Index != rhs.Handle.Index) {
                return lhs.Handle.Index < rhs.Handle.Index;
            }
            if (lhs.Range.index() != rhs.Range.index()) {
                return lhs.Range.index() < rhs.Range.index();
            }
            if (lhs.StateBefore != rhs.StateBefore) {
                return lhs.StateBefore < rhs.StateBefore;
            }
            if (lhs.StateAfter != rhs.StateAfter) {
                return lhs.StateAfter < rhs.StateAfter;
            }

            if (std::holds_alternative<RGTextureRange>(lhs.Range) && std::holds_alternative<RGTextureRange>(rhs.Range)) {
                const auto& left = std::get<RGTextureRange>(lhs.Range);
                const auto& right = std::get<RGTextureRange>(rhs.Range);
                if (left.BaseArrayLayer != right.BaseArrayLayer) {
                    return left.BaseArrayLayer < right.BaseArrayLayer;
                }
                if (left.ArrayLayerCount != right.ArrayLayerCount) {
                    return left.ArrayLayerCount < right.ArrayLayerCount;
                }
                if (left.BaseMipLevel != right.BaseMipLevel) {
                    return left.BaseMipLevel < right.BaseMipLevel;
                }
                return left.MipLevelCount < right.MipLevelCount;
            }

            const auto& left = std::get<RGBufferRange>(lhs.Range);
            const auto& right = std::get<RGBufferRange>(rhs.Range);
            if (left.Offset != right.Offset) {
                return left.Offset < right.Offset;
            }
            return left.Size < right.Size;
        });

    vector<RGBarrier> merged{};
    merged.reserve(barriers.size());
    RGBarrier current = barriers.front();

    for (size_t i = 1; i < barriers.size(); ++i) {
        const RGBarrier& next = barriers[i];
        if (!RGBarrierMergeKeyEquals(current, next)) {
            merged.push_back(current);
            current = next;
            continue;
        }

        RGSubresourceRange mergedRange = current.Range;
        bool mergedThisStep = false;
        if (std::holds_alternative<RGTextureRange>(current.Range) && std::holds_alternative<RGTextureRange>(next.Range)) {
            RGTextureRange mergedTexture{};
            mergedThisStep = RGTryMergeTextureRanges(
                std::get<RGTextureRange>(current.Range),
                std::get<RGTextureRange>(next.Range),
                &mergedTexture);
            if (mergedThisStep) {
                mergedRange = mergedTexture;
            }
        } else if (std::holds_alternative<RGBufferRange>(current.Range) && std::holds_alternative<RGBufferRange>(next.Range)) {
            RGBufferRange mergedBuffer{};
            mergedThisStep = RGTryMergeBufferRanges(
                std::get<RGBufferRange>(current.Range),
                std::get<RGBufferRange>(next.Range),
                &mergedBuffer);
            if (mergedThisStep) {
                mergedRange = mergedBuffer;
            }
        }

        if (mergedThisStep) {
            current.Range = mergedRange;
            continue;
        }

        merged.push_back(current);
        current = next;
    }

    merged.push_back(current);
    return merged;
}

}  // namespace

RGPassBuilder::RGPassBuilder(RGGraphBuilder* graph, uint32_t passIndex) noexcept
    : _graph(graph), _passIndex(passIndex) {}

RGPassBuilder& RGPassBuilder::ReadTexture(
    RGResourceHandle handle,
    const RGTextureRange& range,
    RGAccessMode mode) {
    if (_graph == nullptr) {
        return *this;
    }
    _graph->AddReadTextureEdge(_passIndex, handle, range, mode);
    return *this;
}

RGPassBuilder& RGPassBuilder::ReadBuffer(
    RGResourceHandle handle,
    const RGBufferRange& range,
    RGAccessMode mode) {
    if (_graph == nullptr) {
        return *this;
    }
    _graph->AddReadBufferEdge(_passIndex, handle, range, mode);
    return *this;
}

RGResourceHandle RGPassBuilder::WriteTexture(
    RGResourceHandle handle,
    const RGTextureRange& range,
    RGAccessMode mode) {
    if (_graph == nullptr) {
        return RGResourceHandle::Invalid();
    }
    return _graph->AddWriteTextureEdge(_passIndex, handle, range, mode);
}

RGResourceHandle RGPassBuilder::WriteBuffer(
    RGResourceHandle handle,
    const RGBufferRange& range,
    RGAccessMode mode) {
    if (_graph == nullptr) {
        return RGResourceHandle::Invalid();
    }
    return _graph->AddWriteBufferEdge(_passIndex, handle, range, mode);
}

RGResourceHandle RGPassBuilder::ReadWriteTexture(
    RGResourceHandle handle,
    const RGTextureRange& range,
    RGAccessMode readMode,
    RGAccessMode writeMode) {
    ReadTexture(handle, range, readMode);
    return WriteTexture(handle, range, writeMode);
}

RGResourceHandle RGPassBuilder::ReadWriteBuffer(
    RGResourceHandle handle,
    const RGBufferRange& range,
    RGAccessMode readMode,
    RGAccessMode writeMode) {
    ReadBuffer(handle, range, readMode);
    return WriteBuffer(handle, range, writeMode);
}

RGPassBuilder& RGPassBuilder::SetExecuteFunc(RGPassExecuteFunc func) {
    if (_graph == nullptr || _passIndex >= _graph->_passes.size()) {
        return *this;
    }
    _graph->_passes[_passIndex].ExecuteFunc = std::move(func);
    return *this;
}

RGResourceHandle RGGraphBuilder::CreateTexture(const RGTextureDescriptor& desc) {
    const uint32_t index = static_cast<uint32_t>(_resources.size());
    VirtualResource resource{};
    resource.Flags = desc.Flags;
    resource.Name = string{desc.Name};
    resource.Descriptor = desc;
    _resources.push_back(resource);
    return RGResourceHandle{index};
}

RGResourceHandle RGGraphBuilder::CreateBuffer(const RGBufferDescriptor& desc) {
    const uint32_t index = static_cast<uint32_t>(_resources.size());
    VirtualResource resource{};
    resource.Flags = desc.Flags;
    resource.Name = string{desc.Name};
    resource.Descriptor = desc;
    _resources.push_back(resource);
    return RGResourceHandle{index};
}

RGResourceHandle RGGraphBuilder::CreateIndirectArgs(const RGBufferDescriptor& desc) {
    const uint32_t index = static_cast<uint32_t>(_resources.size());
    VirtualResource resource{};
    resource.Flags = desc.Flags;
    resource.Name = string{desc.Name};
    RGIndirectArgsDescriptor indirectDesc{};
    indirectDesc.Buffer = desc;
    resource.Descriptor = indirectDesc;
    _resources.push_back(resource);
    return RGResourceHandle{index};
}

RGResourceHandle RGGraphBuilder::ImportExternalTexture(
    std::string_view name,
    RGResourceFlags flags,
    RGAccessMode initialMode) {
    RGTextureDescriptor desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleCount = 1;
    desc.Format = TextureFormat::UNKNOWN;
    desc.Flags = flags | RGResourceFlag::External;
    desc.Name = name;
    const auto handle = CreateTexture(desc);
    if (ValidateHandleIndex(handle)) {
        _resources[handle.Index].InitialMode = initialMode;
    }
    return handle;
}

RGResourceHandle RGGraphBuilder::ImportExternalBuffer(
    std::string_view name,
    RGResourceFlags flags,
    RGAccessMode initialMode) {
    RGBufferDescriptor desc{};
    desc.Size = RGBufferRange::All;
    desc.Flags = flags | RGResourceFlag::External;
    desc.Name = name;
    const auto handle = CreateBuffer(desc);
    if (ValidateHandleIndex(handle)) {
        _resources[handle.Index].InitialMode = initialMode;
    }
    return handle;
}

RGPassBuilder RGGraphBuilder::AddPass(std::string_view name, QueueType queueClass) {
    const uint32_t passId = static_cast<uint32_t>(_passes.size());
    RGPassNode node{};
    node.Id = passId;
    node.Name = string{name};
    node.QueueClass = queueClass;
    _passes.push_back(std::move(node));
    return RGPassBuilder{this, passId};
}

void RGGraphBuilder::MarkOutput(RGResourceHandle handle) {
    if (!ValidateHandleIndex(handle)) {
        return;
    }
    _resources[handle.Index].Flags |= RGResourceFlag::Output;
    AddRootEdge(MakeFullRangeEdge(handle));
}

void RGGraphBuilder::MarkOutput(RGResourceHandle handle, const RGTextureRange& range) {
    if (!ValidateHandleIndex(handle)) {
        return;
    }
    _resources[handle.Index].Flags |= RGResourceFlag::Output;
    ResourceAccessEdge edge{};
    edge.Handle = handle;
    edge.Mode = RGAccessMode::Unknown;
    edge.Range = range;
    AddRootEdge(edge);
}

void RGGraphBuilder::MarkOutput(RGResourceHandle handle, const RGBufferRange& range) {
    if (!ValidateHandleIndex(handle)) {
        return;
    }
    _resources[handle.Index].Flags |= RGResourceFlag::Output;
    ResourceAccessEdge edge{};
    edge.Handle = handle;
    edge.Mode = RGAccessMode::Unknown;
    edge.Range = range;
    AddRootEdge(edge);
}

void RGGraphBuilder::ForceRetain(RGResourceHandle handle) {
    if (!ValidateHandleIndex(handle)) {
        return;
    }
    _resources[handle.Index].Flags |= RGResourceFlag::ForceRetain;
    AddRootEdge(MakeFullRangeEdge(handle));
}

void RGGraphBuilder::ForceRetain(RGResourceHandle handle, const RGTextureRange& range) {
    if (!ValidateHandleIndex(handle)) {
        return;
    }
    _resources[handle.Index].Flags |= RGResourceFlag::ForceRetain;
    ResourceAccessEdge edge{};
    edge.Handle = handle;
    edge.Mode = RGAccessMode::Unknown;
    edge.Range = range;
    AddRootEdge(edge);
}

void RGGraphBuilder::ForceRetain(RGResourceHandle handle, const RGBufferRange& range) {
    if (!ValidateHandleIndex(handle)) {
        return;
    }
    _resources[handle.Index].Flags |= RGResourceFlag::ForceRetain;
    ResourceAccessEdge edge{};
    edge.Handle = handle;
    edge.Mode = RGAccessMode::Unknown;
    edge.Range = range;
    AddRootEdge(edge);
}

void RGGraphBuilder::ExportTemporal(RGResourceHandle handle) {
    if (!ValidateHandleIndex(handle)) {
        return;
    }
    _resources[handle.Index].Flags |= RGResourceFlag::Temporal;
    AddRootEdge(MakeFullRangeEdge(handle));
}

void RGGraphBuilder::ExportTemporal(RGResourceHandle handle, const RGTextureRange& range) {
    if (!ValidateHandleIndex(handle)) {
        return;
    }
    _resources[handle.Index].Flags |= RGResourceFlag::Temporal;
    ResourceAccessEdge edge{};
    edge.Handle = handle;
    edge.Mode = RGAccessMode::Unknown;
    edge.Range = range;
    AddRootEdge(edge);
}

void RGGraphBuilder::ExportTemporal(RGResourceHandle handle, const RGBufferRange& range) {
    if (!ValidateHandleIndex(handle)) {
        return;
    }
    _resources[handle.Index].Flags |= RGResourceFlag::Temporal;
    ResourceAccessEdge edge{};
    edge.Handle = handle;
    edge.Mode = RGAccessMode::Unknown;
    edge.Range = range;
    AddRootEdge(edge);
}

bool RGGraphBuilder::ValidateHandleIndex(RGResourceHandle handle) const noexcept {
    return handle.IsValid() && handle.Index < _resources.size();
}

RGResourceHandle RGGraphBuilder::AddReadTextureEdge(
    uint32_t passIndex,
    RGResourceHandle handle,
    const RGTextureRange& range,
    RGAccessMode mode) {
    if (passIndex >= _passes.size()) {
        RADRAY_ERR_LOG("RenderGraph: invalid pass index {} in AddReadTextureEdge", passIndex);
        return RGResourceHandle::Invalid();
    }
    if (!ValidateHandleIndex(handle)) {
        RADRAY_ERR_LOG("RenderGraph: invalid handle {} in ReadTexture", handle.Index);
        return RGResourceHandle::Invalid();
    }
    if (!RGIsReadAccess(mode)) {
        RADRAY_ERR_LOG("RenderGraph: invalid read mode {} in ReadTexture", mode);
        return RGResourceHandle::Invalid();
    }

    const auto& resource = _resources[handle.Index];
    const auto* textureDesc = RGGetTextureDescriptor(resource);
    if (!RGIsTextureResourceType(resource.GetType()) || textureDesc == nullptr) {
        RADRAY_ERR_LOG("RenderGraph: ReadTexture used on non-texture resource {}", handle.Index);
        return RGResourceHandle::Invalid();
    }

    RGTextureRange canonical{};
    if (!RGCanonicalizeTextureRange(*textureDesc, range, &canonical)) {
        return RGResourceHandle::Invalid();
    }

    ResourceAccessEdge edge{};
    edge.Handle = handle;
    edge.Mode = mode;
    edge.Range = canonical;

    auto& reads = _passes[passIndex].Reads;
    if (!RGContainsEdge(reads, edge)) {
        reads.push_back(edge);
    }
    return handle;
}

RGResourceHandle RGGraphBuilder::AddReadBufferEdge(
    uint32_t passIndex,
    RGResourceHandle handle,
    const RGBufferRange& range,
    RGAccessMode mode) {
    if (passIndex >= _passes.size()) {
        RADRAY_ERR_LOG("RenderGraph: invalid pass index {} in AddReadBufferEdge", passIndex);
        return RGResourceHandle::Invalid();
    }
    if (!ValidateHandleIndex(handle)) {
        RADRAY_ERR_LOG("RenderGraph: invalid handle {} in ReadBuffer", handle.Index);
        return RGResourceHandle::Invalid();
    }
    if (!RGIsReadAccess(mode)) {
        RADRAY_ERR_LOG("RenderGraph: invalid read mode {} in ReadBuffer", mode);
        return RGResourceHandle::Invalid();
    }

    const auto& resource = _resources[handle.Index];
    const auto* bufferDesc = RGGetBufferDescriptor(resource);
    if (!RGIsBufferResourceType(resource.GetType()) || bufferDesc == nullptr) {
        RADRAY_ERR_LOG("RenderGraph: ReadBuffer used on non-buffer resource {}", handle.Index);
        return RGResourceHandle::Invalid();
    }

    RGBufferRange canonical{};
    if (!RGCanonicalizeBufferRange(*bufferDesc, range, &canonical)) {
        return RGResourceHandle::Invalid();
    }

    ResourceAccessEdge edge{};
    edge.Handle = handle;
    edge.Mode = mode;
    edge.Range = canonical;

    auto& reads = _passes[passIndex].Reads;
    if (!RGContainsEdge(reads, edge)) {
        reads.push_back(edge);
    }
    return handle;
}

RGResourceHandle RGGraphBuilder::AddWriteTextureEdge(
    uint32_t passIndex,
    RGResourceHandle handle,
    const RGTextureRange& range,
    RGAccessMode mode) {
    if (passIndex >= _passes.size()) {
        RADRAY_ERR_LOG("RenderGraph: invalid pass index {} in AddWriteTextureEdge", passIndex);
        return RGResourceHandle::Invalid();
    }
    if (!ValidateHandleIndex(handle)) {
        RADRAY_ERR_LOG("RenderGraph: invalid handle {} in WriteTexture", handle.Index);
        return RGResourceHandle::Invalid();
    }
    if (!RGIsWriteAccess(mode)) {
        RADRAY_ERR_LOG("RenderGraph: invalid write mode {} in WriteTexture", mode);
        return RGResourceHandle::Invalid();
    }

    const auto& resource = _resources[handle.Index];
    const auto* textureDesc = RGGetTextureDescriptor(resource);
    if (!RGIsTextureResourceType(resource.GetType()) || textureDesc == nullptr) {
        RADRAY_ERR_LOG("RenderGraph: WriteTexture used on non-texture resource {}", handle.Index);
        return RGResourceHandle::Invalid();
    }

    RGTextureRange canonical{};
    if (!RGCanonicalizeTextureRange(*textureDesc, range, &canonical)) {
        return RGResourceHandle::Invalid();
    }

    ResourceAccessEdge edge{};
    edge.Handle = handle;
    edge.Mode = mode;
    edge.Range = canonical;

    auto& writes = _passes[passIndex].Writes;
    if (!RGContainsEdge(writes, edge)) {
        writes.push_back(edge);
    }
    return handle;
}

RGResourceHandle RGGraphBuilder::AddWriteBufferEdge(
    uint32_t passIndex,
    RGResourceHandle handle,
    const RGBufferRange& range,
    RGAccessMode mode) {
    if (passIndex >= _passes.size()) {
        RADRAY_ERR_LOG("RenderGraph: invalid pass index {} in AddWriteBufferEdge", passIndex);
        return RGResourceHandle::Invalid();
    }
    if (!ValidateHandleIndex(handle)) {
        RADRAY_ERR_LOG("RenderGraph: invalid handle {} in WriteBuffer", handle.Index);
        return RGResourceHandle::Invalid();
    }
    if (!RGIsWriteAccess(mode)) {
        RADRAY_ERR_LOG("RenderGraph: invalid write mode {} in WriteBuffer", mode);
        return RGResourceHandle::Invalid();
    }

    const auto& resource = _resources[handle.Index];
    const auto* bufferDesc = RGGetBufferDescriptor(resource);
    if (!RGIsBufferResourceType(resource.GetType()) || bufferDesc == nullptr) {
        RADRAY_ERR_LOG("RenderGraph: WriteBuffer used on non-buffer resource {}", handle.Index);
        return RGResourceHandle::Invalid();
    }

    RGBufferRange canonical{};
    if (!RGCanonicalizeBufferRange(*bufferDesc, range, &canonical)) {
        return RGResourceHandle::Invalid();
    }

    ResourceAccessEdge edge{};
    edge.Handle = handle;
    edge.Mode = mode;
    edge.Range = canonical;

    auto& writes = _passes[passIndex].Writes;
    if (!RGContainsEdge(writes, edge)) {
        writes.push_back(edge);
    }
    return handle;
}

void RGGraphBuilder::AddRootEdge(const ResourceAccessEdge& edge) {
    if (!ValidateHandleIndex(edge.Handle)) {
        return;
    }

    auto normalized = edge;
    const auto& resource = _resources[edge.Handle.Index];
    if (RGIsTextureResourceType(resource.GetType())) {
        const auto* textureDesc = RGGetTextureDescriptor(resource);
        if (textureDesc == nullptr) {
            return;
        }
        RGTextureRange canonical{};
        RGTextureRange inputRange = RGTextureRange{};
        if (std::holds_alternative<RGTextureRange>(normalized.Range)) {
            inputRange = std::get<RGTextureRange>(normalized.Range);
        }
        if (!RGCanonicalizeTextureRange(*textureDesc, inputRange, &canonical)) {
            return;
        }
        normalized.Range = canonical;
    } else {
        const auto* bufferDesc = RGGetBufferDescriptor(resource);
        if (bufferDesc == nullptr) {
            return;
        }
        RGBufferRange canonical{};
        RGBufferRange inputRange = RGBufferRange{};
        if (std::holds_alternative<RGBufferRange>(normalized.Range)) {
            inputRange = std::get<RGBufferRange>(normalized.Range);
        }
        if (!RGCanonicalizeBufferRange(*bufferDesc, inputRange, &canonical)) {
            return;
        }
        normalized.Range = canonical;
    }
    normalized.Mode = RGAccessMode::Unknown;

    if (!RGContainsEdge(_roots, normalized)) {
        _roots.push_back(normalized);
    }
}

ResourceAccessEdge RGGraphBuilder::MakeFullRangeEdge(RGResourceHandle handle) const {
    if (!ValidateHandleIndex(handle)) {
        return ResourceAccessEdge{};
    }
    return RGMakeFullRangeEdge(handle.Index, _resources[handle.Index]);
}

CompiledGraph RGGraphBuilder::Compile() const {
    CompiledGraph compiled{};
    compiled.Success = true;
    compiled.Allocations.resize(_resources.size());

    if (_passes.empty()) {
        return compiled;
    }

    const auto& resources = _resources;
    const auto& passes = _passes;

    vector<ResourceAccessEdge> roots = _roots;
    const auto implicitRoots = RGCollectImplicitRoots(*this);
    for (const auto& root : implicitRoots) {
        if (!RGContainsEdge(roots, root)) {
            roots.push_back(root);
        }
    }

    struct WriterRef {
        uint32_t PassIndex{0};
        const ResourceAccessEdge* Edge{nullptr};
    };
    vector<vector<WriterRef>> writersByResource(resources.size());
    for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(passes.size()); ++passIndex) {
        const auto& pass = passes[passIndex];
        for (const auto& writeEdge : pass.Writes) {
            if (!ValidateHandleIndex(writeEdge.Handle)) {
                continue;
            }
            writersByResource[writeEdge.Handle.Index].push_back(WriterRef{
                .PassIndex = passIndex,
                .Edge = &writeEdge});
        }
    }

    vector<bool> passAlive(passes.size(), false);
    queue<ResourceAccessEdge> worklist{};
    unordered_set<RGRegionKey, RGRegionKeyHash> visitedRegions{};

    auto enqueueRegion = [&](const ResourceAccessEdge& edge) {
        const auto key = RGMakeRegionKey(resources, edge);
        if (!key.has_value()) {
            return;
        }
        if (visitedRegions.insert(key.value()).second) {
            worklist.push(edge);
        }
    };

    for (const auto& root : roots) {
        enqueueRegion(root);
    }

    while (!worklist.empty()) {
        const auto target = worklist.front();
        worklist.pop();

        if (!ValidateHandleIndex(target.Handle)) {
            continue;
        }

        const uint32_t resourceIndex = target.Handle.Index;
        const auto& resource = resources[resourceIndex];
        for (const auto& writer : writersByResource[resourceIndex]) {
            if (writer.Edge == nullptr) {
                continue;
            }
            if (!RGEdgesOverlap(resource, *writer.Edge, target)) {
                continue;
            }

            if (passAlive[writer.PassIndex]) {
                continue;
            }
            passAlive[writer.PassIndex] = true;

            for (const auto& readEdge : passes[writer.PassIndex].Reads) {
                enqueueRegion(readEdge);
            }
        }
    }

    uint32_t aliveCount = 0;
    for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(passes.size()); ++passIndex) {
        if (passAlive[passIndex]) {
            aliveCount += 1;
        } else {
            compiled.CulledPasses.push_back(passIndex);
        }
    }
    if (aliveCount == 0) {
        return compiled;
    }

    vector<vector<uint32_t>> successors(passes.size());
    vector<uint32_t> indegree(passes.size(), 0);

    for (uint32_t lhsPass = 0; lhsPass < static_cast<uint32_t>(passes.size()); ++lhsPass) {
        if (!passAlive[lhsPass]) {
            continue;
        }
        for (uint32_t rhsPass = lhsPass + 1; rhsPass < static_cast<uint32_t>(passes.size()); ++rhsPass) {
            if (!passAlive[rhsPass]) {
                continue;
            }
            if (!RGPassesConflict(resources, passes[lhsPass], passes[rhsPass])) {
                continue;
            }

            successors[lhsPass].push_back(rhsPass);
            indegree[rhsPass] += 1;
            compiled.DependencyEdges.push_back(std::pair<uint32_t, uint32_t>{lhsPass, rhsPass});
        }
    }

    priority_queue<uint32_t, vector<uint32_t>, std::greater<uint32_t>> ready{};
    for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(passes.size()); ++passIndex) {
        if (passAlive[passIndex] && indegree[passIndex] == 0) {
            ready.push(passIndex);
        }
    }

    compiled.SortedPasses.reserve(aliveCount);
    while (!ready.empty()) {
        const uint32_t currentPass = ready.top();
        ready.pop();
        compiled.SortedPasses.push_back(currentPass);

        for (uint32_t succ : successors[currentPass]) {
            if (indegree[succ] == 0) {
                continue;
            }
            indegree[succ] -= 1;
            if (indegree[succ] == 0) {
                ready.push(succ);
            }
        }
    }

    if (compiled.SortedPasses.size() != aliveCount) {
        compiled.Success = false;
        compiled.ErrorMessage = "Fatal Error: Circular Dependency Detected";
        RADRAY_ERR_LOG("RenderGraph: {}", compiled.ErrorMessage);
        return compiled;
    }

    vector<vector<RGSubresourceRange>> usedRangesByResource(resources.size());
    for (uint32_t sortedIndex = 0; sortedIndex < static_cast<uint32_t>(compiled.SortedPasses.size()); ++sortedIndex) {
        const auto passIndex = compiled.SortedPasses[sortedIndex];
        const auto& pass = passes[passIndex];
        for (const auto& readEdge : pass.Reads) {
            if (ValidateHandleIndex(readEdge.Handle)) {
                usedRangesByResource[readEdge.Handle.Index].push_back(readEdge.Range);
            }
        }
        for (const auto& writeEdge : pass.Writes) {
            if (ValidateHandleIndex(writeEdge.Handle)) {
                usedRangesByResource[writeEdge.Handle.Index].push_back(writeEdge.Range);
            }
        }
    }

    vector<vector<RGSubresourceRange>> atomicRangesByResource(resources.size());
    for (uint32_t resourceIndex = 0; resourceIndex < static_cast<uint32_t>(resources.size()); ++resourceIndex) {
        atomicRangesByResource[resourceIndex] = RGCollectAtomicRanges(
            resources[resourceIndex],
            usedRangesByResource[resourceIndex]);
    }

    compiled.PassBarriers.resize(compiled.SortedPasses.size());
    unordered_map<RGRegionKey, RGAccessMode, RGRegionKeyHash> stateByRegion{};
    for (uint32_t resourceIndex = 0; resourceIndex < static_cast<uint32_t>(resources.size()); ++resourceIndex) {
        const RGAccessMode initialMode = resources[resourceIndex].InitialMode;
        const auto& atomicRanges = atomicRangesByResource[resourceIndex];
        for (const auto& atomicRange : atomicRanges) {
            ResourceAccessEdge regionEdge{};
            regionEdge.Handle = RGResourceHandle{resourceIndex};
            regionEdge.Mode = RGAccessMode::Unknown;
            regionEdge.Range = atomicRange;
            const auto key = RGMakeRegionKey(resources, regionEdge);
            if (!key.has_value()) {
                continue;
            }
            stateByRegion.insert_or_assign(key.value(), initialMode);
        }
    }

    auto collectAtomicRangesForEdge = [&](const ResourceAccessEdge& edge) {
        vector<RGSubresourceRange> ranges{};
        if (!ValidateHandleIndex(edge.Handle)) {
            return ranges;
        }

        const uint32_t resourceIndex = edge.Handle.Index;
        const auto& resource = resources[resourceIndex];
        const auto& atomicRanges = atomicRangesByResource[resourceIndex];

        if (atomicRanges.empty()) {
            ranges.push_back(edge.Range);
            return ranges;
        }

        for (const auto& atomicRange : atomicRanges) {
            if (RGSubresourceRangesOverlap(resource, edge.Range, atomicRange)) {
                ranges.push_back(atomicRange);
            }
        }
        if (ranges.empty()) {
            ranges.push_back(edge.Range);
        }
        return ranges;
    };

    for (uint32_t sortedIndex = 0; sortedIndex < static_cast<uint32_t>(compiled.SortedPasses.size()); ++sortedIndex) {
        const uint32_t passIndex = compiled.SortedPasses[sortedIndex];
        const auto& pass = passes[passIndex];

        unordered_map<RGRegionKey, RGAccessMode, RGRegionKeyHash> requestedModeByRegion{};
        auto mergeEdgeRequest = [&](const ResourceAccessEdge& edge) {
            if (!ValidateHandleIndex(edge.Handle)) {
                return;
            }

            const auto overlappedAtomicRanges = collectAtomicRangesForEdge(edge);
            for (const auto& range : overlappedAtomicRanges) {
                ResourceAccessEdge regionEdge{};
                regionEdge.Handle = edge.Handle;
                regionEdge.Mode = RGAccessMode::Unknown;
                regionEdge.Range = range;

                const auto key = RGMakeRegionKey(resources, regionEdge);
                if (!key.has_value()) {
                    continue;
                }

                const auto requestIt = requestedModeByRegion.find(key.value());
                if (requestIt == requestedModeByRegion.end()) {
                    requestedModeByRegion.insert_or_assign(key.value(), edge.Mode);
                } else {
                    requestIt->second = RGMergeRequestedMode(requestIt->second, edge.Mode);
                }
            }
        };

        for (const auto& readEdge : pass.Reads) {
            mergeEdgeRequest(readEdge);
        }
        for (const auto& writeEdge : pass.Writes) {
            mergeEdgeRequest(writeEdge);
        }

        vector<RGBarrier> passBarriers{};
        passBarriers.reserve(requestedModeByRegion.size());
        for (const auto& [region, requestedMode] : requestedModeByRegion) {
            const RGAccessMode defaultState = resources[region.ResourceIndex].InitialMode;
            const auto stateIt = stateByRegion.find(region);
            const RGAccessMode stateBefore = stateIt == stateByRegion.end()
                                                 ? defaultState
                                                 : stateIt->second;

            if (RGNeedBarrier(stateBefore, requestedMode)) {
                passBarriers.push_back(RGBarrier{
                    .Handle = RGResourceHandle{region.ResourceIndex},
                    .Range = region.Range,
                    .StateBefore = stateBefore,
                    .StateAfter = requestedMode});
            }
            stateByRegion.insert_or_assign(region, requestedMode);
        }

        compiled.PassBarriers[sortedIndex] = RGMergePassBarriers(std::move(passBarriers));
    }

    unordered_map<RGRegionKey, uint32_t, RGRegionKeyHash> lifetimeIndexByRegion{};
    vector<bool> lifetimeHasRead{};
    vector<bool> lifetimeHasWrite{};

    auto touchSingleLifetimeRange = [&](RGResourceHandle handle, const RGSubresourceRange& range, uint32_t sortedIndex, bool isRead) {
        ResourceAccessEdge regionEdge{};
        regionEdge.Handle = handle;
        regionEdge.Mode = RGAccessMode::Unknown;
        regionEdge.Range = range;

        const auto key = RGMakeRegionKey(resources, regionEdge);
        if (!key.has_value()) {
            return;
        }

        const auto it = lifetimeIndexByRegion.find(key.value());
        uint32_t lifetimeIndex = 0;
        if (it == lifetimeIndexByRegion.end()) {
            lifetimeIndex = static_cast<uint32_t>(compiled.SubresourceLifetimes.size());
            lifetimeIndexByRegion.insert_or_assign(key.value(), lifetimeIndex);

            RGSubresourceLifetime lifetime{};
            lifetime.Handle = handle;
            lifetime.Range = range;
            lifetime.FirstPass = sortedIndex;
            lifetime.LastPass = sortedIndex;
            lifetime.IsExternal = resources[handle.Index].Flags.HasFlag(RGResourceFlag::External);
            compiled.SubresourceLifetimes.push_back(lifetime);
            lifetimeHasRead.push_back(false);
            lifetimeHasWrite.push_back(false);
        } else {
            lifetimeIndex = it->second;
            compiled.SubresourceLifetimes[lifetimeIndex].LastPass = sortedIndex;
        }

        if (isRead) {
            lifetimeHasRead[lifetimeIndex] = true;
        } else {
            lifetimeHasWrite[lifetimeIndex] = true;
        }
    };

    auto touchLifetime = [&](const ResourceAccessEdge& edge, uint32_t sortedIndex, bool isRead) {
        if (!ValidateHandleIndex(edge.Handle)) {
            return;
        }

        const uint32_t resourceIndex = edge.Handle.Index;
        const auto& resource = resources[resourceIndex];
        const auto& atomicRanges = atomicRangesByResource[resourceIndex];

        if (atomicRanges.empty()) {
            touchSingleLifetimeRange(edge.Handle, edge.Range, sortedIndex, isRead);
            return;
        }

        for (const auto& atomicRange : atomicRanges) {
            if (!RGSubresourceRangesOverlap(resource, edge.Range, atomicRange)) {
                continue;
            }
            touchSingleLifetimeRange(edge.Handle, atomicRange, sortedIndex, isRead);
        }
    };

    for (uint32_t sortedIndex = 0; sortedIndex < static_cast<uint32_t>(compiled.SortedPasses.size()); ++sortedIndex) {
        const auto passIndex = compiled.SortedPasses[sortedIndex];
        const auto& pass = passes[passIndex];
        for (const auto& readEdge : pass.Reads) {
            touchLifetime(readEdge, sortedIndex, true);
        }
        for (const auto& writeEdge : pass.Writes) {
            touchLifetime(writeEdge, sortedIndex, false);
        }
    }

    if (!compiled.SortedPasses.empty()) {
        const uint32_t graphLastPass = static_cast<uint32_t>(compiled.SortedPasses.size() - 1);
        for (uint32_t i = 0; i < static_cast<uint32_t>(compiled.SubresourceLifetimes.size()); ++i) {
            auto& lifetime = compiled.SubresourceLifetimes[i];
            if (!ValidateHandleIndex(lifetime.Handle)) {
                continue;
            }
            const auto& flags = resources[lifetime.Handle.Index].Flags;
            const bool isRootLike = flags.HasFlag(RGResourceFlag::Output) || flags.HasFlag(RGResourceFlag::ForceRetain) || flags.HasFlag(RGResourceFlag::Persistent) || flags.HasFlag(RGResourceFlag::Temporal);
            if (isRootLike && lifetimeHasWrite[i]) {
                lifetime.LastPass = graphLastPass;
            }
        }
    }

    std::sort(
        compiled.DependencyEdges.begin(),
        compiled.DependencyEdges.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.first != rhs.first) {
                return lhs.first < rhs.first;
            }
            return lhs.second < rhs.second;
        });

    return compiled;
}

string RGGraphBuilder::DumpRecording() const {
    fmt::memory_buffer buffer{};
    fmt::format_to(std::back_inserter(buffer), "RenderGraph Recording\n");
    fmt::format_to(std::back_inserter(buffer), "Resources: {}\n", _resources.size());

    for (uint32_t resourceIndex = 0; resourceIndex < static_cast<uint32_t>(_resources.size()); ++resourceIndex) {
        const auto& resource = _resources[resourceIndex];
        fmt::format_to(
            std::back_inserter(buffer),
            "  [{}] {} type={} flags={}\n",
            resourceIndex,
            resource.Name,
            resource.GetType(),
            resource.Flags);
    }

    fmt::format_to(std::back_inserter(buffer), "Passes: {}\n", _passes.size());
    for (const auto& pass : _passes) {
        fmt::format_to(
            std::back_inserter(buffer),
            "  Pass[{}] {} queue={} reads={} writes={}\n",
            pass.Id,
            pass.Name,
            pass.QueueClass,
            pass.Reads.size(),
            pass.Writes.size());
        for (const auto& readEdge : pass.Reads) {
            fmt::format_to(
                std::back_inserter(buffer),
                "    R res[{}] mode={} {}\n",
                readEdge.Handle.Index,
                readEdge.Mode,
                RGFormatEdgeRange(readEdge));
        }
        for (const auto& writeEdge : pass.Writes) {
            fmt::format_to(
                std::back_inserter(buffer),
                "    W res[{}] mode={} {}\n",
                writeEdge.Handle.Index,
                writeEdge.Mode,
                RGFormatEdgeRange(writeEdge));
        }
    }

    return string{buffer.data(), buffer.size()};
}

string RGGraphBuilder::DumpCompiledGraph(const CompiledGraph& compiled) const {
    fmt::memory_buffer buffer{};
    fmt::format_to(std::back_inserter(buffer), "RenderGraph Compiled\n");
    fmt::format_to(std::back_inserter(buffer), "success={}", compiled.Success ? "true" : "false");
    if (!compiled.ErrorMessage.empty()) {
        fmt::format_to(std::back_inserter(buffer), " error=\"{}\"", compiled.ErrorMessage);
    }
    fmt::format_to(std::back_inserter(buffer), "\n");

    fmt::format_to(std::back_inserter(buffer), "sorted_passes:");
    for (uint32_t passIndex : compiled.SortedPasses) {
        fmt::format_to(std::back_inserter(buffer), " {}", passIndex);
    }
    fmt::format_to(std::back_inserter(buffer), "\n");

    fmt::format_to(std::back_inserter(buffer), "culled_passes:");
    for (uint32_t passIndex : compiled.CulledPasses) {
        fmt::format_to(std::back_inserter(buffer), " {}", passIndex);
    }
    fmt::format_to(std::back_inserter(buffer), "\n");

    fmt::format_to(std::back_inserter(buffer), "dependency_edges:");
    for (const auto& edge : compiled.DependencyEdges) {
        fmt::format_to(std::back_inserter(buffer), " ({}->{})", edge.first, edge.second);
    }
    fmt::format_to(std::back_inserter(buffer), "\n");

    fmt::format_to(std::back_inserter(buffer), "subresource_lifetimes:\n");
    for (const auto& lifetime : compiled.SubresourceLifetimes) {
        const string range = RGFormatSubresourceRange(lifetime.Range);
        fmt::format_to(
            std::back_inserter(buffer),
            "  res[{}] {} [{}, {}] external={}\n",
            lifetime.Handle.Index,
            range,
            lifetime.FirstPass,
            lifetime.LastPass,
            lifetime.IsExternal ? "true" : "false");
    }

    fmt::format_to(std::back_inserter(buffer), "pass_barriers:\n");
    for (uint32_t sortedIndex = 0; sortedIndex < static_cast<uint32_t>(compiled.PassBarriers.size()); ++sortedIndex) {
        const uint32_t passIndex = sortedIndex < static_cast<uint32_t>(compiled.SortedPasses.size())
                                       ? compiled.SortedPasses[sortedIndex]
                                       : RGSubresourceLifetime::InvalidPassIndex;
        for (const auto& barrier : compiled.PassBarriers[sortedIndex]) {
            fmt::format_to(
                std::back_inserter(buffer),
                "  [Pass {}] Barrier: handle={} range={} {} -> {}\n",
                passIndex,
                barrier.Handle.Index,
                RGFormatSubresourceRange(barrier.Range),
                barrier.StateBefore,
                barrier.StateAfter);
        }
    }

    fmt::format_to(std::back_inserter(buffer), "allocations: {}\n", compiled.Allocations.size());

    return string{buffer.data(), buffer.size()};
}

std::string_view format_as(RGResourceType v) noexcept {
    switch (v) {
        case RGResourceType::Unknown: return "UNKNOWN";
        case RGResourceType::Texture: return "Texture";
        case RGResourceType::Buffer: return "Buffer";
        case RGResourceType::IndirectArgs: return "IndirectArgs";
    }
    return "UNKNOWN";
}

std::string_view format_as(RGResourceFlag v) noexcept {
    switch (v) {
        case RGResourceFlag::None: return "None";
        case RGResourceFlag::Persistent: return "Persistent";
        case RGResourceFlag::External: return "External";
        case RGResourceFlag::Output: return "Output";
        case RGResourceFlag::ForceRetain: return "ForceRetain";
        case RGResourceFlag::Temporal: return "Temporal";
    }
    return "UNKNOWN";
}

std::string_view format_as(RGAccessMode v) noexcept {
    switch (v) {
        case RGAccessMode::Unknown: return "Unknown";
        case RGAccessMode::SampledRead: return "SampledRead";
        case RGAccessMode::StorageRead: return "StorageRead";
        case RGAccessMode::StorageWrite: return "StorageWrite";
        case RGAccessMode::ColorAttachmentWrite: return "ColorAttachmentWrite";
        case RGAccessMode::DepthStencilRead: return "DepthStencilRead";
        case RGAccessMode::DepthStencilWrite: return "DepthStencilWrite";
        case RGAccessMode::CopySource: return "CopySource";
        case RGAccessMode::CopyDestination: return "CopyDestination";
        case RGAccessMode::IndirectRead: return "IndirectRead";
    }
    return "UNKNOWN";
}

}  // namespace radray::render
