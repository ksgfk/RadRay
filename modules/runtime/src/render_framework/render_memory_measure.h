#pragma once

#include <radray/inline_vector.h>
#include <radray/runtime/render_framework/render_memory_stats.h>

namespace radray::detail {

template <class T>
void MeasureVector(RenderMemoryStats& result, const vector<T>& values, bool payload = false) noexcept {
    result.VectorCapacityBytes += values.capacity() * sizeof(T);
    if (payload) {
        result.VariablePayloadBytes += values.size() * sizeof(T);
        result.VariablePayloadCapacityBytes += values.capacity() * sizeof(T);
    }
}
template <class T, size_t N>
void MeasureVector(RenderMemoryStats& result, const InlineVector<T, N>& values, bool payload = false) noexcept {
    if (values.capacity() > N) result.VectorCapacityBytes += values.capacity() * sizeof(T);
    if (payload) {
        result.VariablePayloadBytes += values.size() * sizeof(T);
        result.VariablePayloadCapacityBytes += values.capacity() * sizeof(T);
    }
}
inline void MeasureString(RenderMemoryStats& result, const string& value) noexcept {
    const auto object = reinterpret_cast<uintptr_t>(&value);
    const auto data = reinterpret_cast<uintptr_t>(value.data());
    if (data < object || data - object >= sizeof(value)) result.StringCapacityBytes += value.capacity() + 1;
    result.VariablePayloadBytes += value.size();
    result.VariablePayloadCapacityBytes += value.capacity();
}
template <class Map>
void MeasureMap(RenderMemoryStats& result, const Map& values) noexcept {
    ++result.MapContainers;
    result.MapNodes += values.size();
    result.MapBuckets += values.bucket_count();
    result.MapValueBytes += values.size() * sizeof(typename Map::value_type);
}

}  // namespace radray::detail
